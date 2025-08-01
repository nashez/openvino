// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "rms_norm.h"

#include <common/utils.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <oneapi/dnnl/dnnl_common.hpp>

#include "cpu/x64/cpu_isa_traits.hpp"
#include "cpu_memory.h"
#include "graph_context.h"
#include "memory_desc/cpu_memory_desc.h"
#include "memory_desc/cpu_memory_desc_utils.h"
#include "node.h"
#include "onednn/iml_type_mapper.h"
#include "openvino/core/except.hpp"
#include "openvino/core/node.hpp"
#include "openvino/core/shape.hpp"
#include "openvino/core/type.hpp"
#include "openvino/core/type/element_type.hpp"
#include "ov_ops/rms.hpp"
#include "shape_inference/custom/rms_norm.hpp"
#include "utils/general_utils.h"
#ifdef OPENVINO_ARCH_X86_64
#    include "kernels/x64/rms_kernel.hpp"
#    include "nodes/kernels/x64/jit_kernel_base.hpp"
#    include "openvino/core/parallel.hpp"
#endif

#include <string>
#include <vector>

using namespace dnnl::impl;
using namespace dnnl::impl::cpu::x64;

namespace ov::intel_cpu::node {

struct RMSNormKey {
    ov::element::Type precision;
    size_t data_size;
    size_t scale_size;
    float eps;
    [[nodiscard]] size_t hash() const;
    bool operator==(const RMSNormKey& rhs) const;
};

size_t RMSNormKey::hash() const {
    size_t seed = 0;
    seed = hash_combine(seed, precision.hash());
    seed = hash_combine(seed, data_size);
    seed = hash_combine(seed, scale_size);
    seed = hash_combine(seed, eps);

    return seed;
}

bool RMSNormKey::operator==(const RMSNormKey& rhs) const {
    auto retVal =
        precision == rhs.precision && data_size == rhs.data_size && scale_size == rhs.scale_size && eps == rhs.eps;

    return retVal;
}

#if defined(OPENVINO_ARCH_X86_64)
static std::shared_ptr<kernel::JitKernelBase> createJitKernel(const kernel::jit_rms_compile_params& param) {
    std::shared_ptr<kernel::JitKernelBase> res;

    if (dnnl::impl::cpu::x64::mayiuse(dnnl::impl::cpu::x64::avx512_core)) {
        res = std::make_shared<kernel::jit_rms_kernel<dnnl::impl::cpu::x64::avx512_core>>(param);
    } else if (dnnl::impl::cpu::x64::mayiuse(dnnl::impl::cpu::x64::avx2)) {
        res = std::make_shared<kernel::jit_rms_kernel<dnnl::impl::cpu::x64::avx2>>(param);
    }

    if (res) {
        res->create_kernel();
    }

    return res;
}

static void execJitKernel(const std::shared_ptr<kernel::JitKernelBase>& ker,
                          const uint8_t* src,
                          uint8_t* dst,
                          const float* scale) {
    kernel::jit_rms_call_args call_args{src, scale, dst};
    (*ker)(&call_args);
}

struct RMSNorm::RMSNormExecutor : public RMSNorm::Executor {
    RMSNormExecutor(ov::element::Type precision, size_t data_size, size_t scale_size, float eps)
        : m_precision(precision) {
        kernel::jit_rms_compile_params jcp;
        jcp.src_prc = precision;
        jcp.dst_prc = precision;
        jcp.data_size = data_size;
        jcp.scale_size = scale_size;
        jcp.eps = eps;
        m_kernel = createJitKernel(jcp);
    }
    void execute(const std::vector<MemoryPtr>& inputs, const MemoryPtr output) override {
        auto* src = inputs[0]->getDataAs<uint8_t>();
        auto* dst = output->getDataAs<uint8_t>();
        auto* scale = inputs[1]->getDataAs<float>();

        const auto& src_strides = inputs[0]->getDescWithType<BlockedMemoryDesc>()->getStrides();
        const auto& dst_strides = output->getDescWithType<BlockedMemoryDesc>()->getStrides();
        const auto& shape = inputs[0]->getStaticDims();
        const auto src_stride = src_strides[src_strides.size() - 2] * m_precision.size();
        const auto dst_stride = dst_strides[dst_strides.size() - 2] * m_precision.size();
        auto n = shape_size(shape) / shape[shape.size() - 1];
        parallel_for(n, [&](size_t i) {
            execJitKernel(m_kernel, src + i * src_stride, dst + i * dst_stride, scale);
        });
    }

private:
    ov::element::Type m_precision;
    std::shared_ptr<kernel::JitKernelBase> m_kernel;
};
#endif  // OPENVINO_ARCH_X86_64

RMSNorm::RMSNorm(const std::shared_ptr<ov::Node>& op, const GraphContext::CPtr& context)
    : Node(op, context, RMSNormShapeInferFactory(op)) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        OPENVINO_THROW_NOT_IMPLEMENTED(errorMessage);
    }
    const auto rms = ov::as_type_ptr<const ov::op::internal::RMS>(op);
    m_eps = static_cast<float>(rms->get_epsilon());
}

void RMSNorm::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty()) {
        return;
    }
    auto precision = getOriginalInputPrecisionAtPort(0);
    if (none_of(precision, ov::element::f32, ov::element::bf16, ov::element::f16)) {
        precision = ov::element::f32;
    }

    auto impl_type = [&]() {
        if (mayiuse(cpu::x64::avx512_core)) {
            return impl_desc_type::jit_avx512;
        }
        if (mayiuse(cpu::x64::avx2)) {
            return impl_desc_type::jit_avx2;
        }
        return impl_desc_type::ref;
    }();

    addSupportedPrimDesc({{LayoutType::ncsp, precision}, {LayoutType::ncsp, ov::element::f32}},
                         {{LayoutType::ncsp, precision}},
                         impl_type);
}

void RMSNorm::createPrimitive() {
    auto precision = getOriginalInputPrecisionAtPort(0);
    auto data_dims = getSrcMemoryAtPort(0)->getDescWithType<BlockedMemoryDesc>()->getBlockDims();
    size_t data_size = data_dims[data_dims.size() - 1];
    size_t scale_size = shape_size(getSrcMemoryAtPort(1)->getDescWithType<BlockedMemoryDesc>()->getBlockDims());

    RMSNormKey key = {precision, data_size, scale_size, m_eps};

    auto builder = [&]([[maybe_unused]] const RMSNormKey& key) -> std::shared_ptr<Executor> {
#ifdef OPENVINO_ARCH_X86_64
        return std::make_shared<RMSNormExecutor>(precision, data_size, scale_size, m_eps);
#else
        return nullptr;
#endif
    };

    auto cache = context->getParamsCache();
    auto result = cache->getOrCreate(key, builder);
    OPENVINO_ASSERT(result.first, "RMSNorm Executor creation fails with precision " + precision.to_string());
    m_executor = result.first;
}

void RMSNorm::execute([[maybe_unused]] const dnnl::stream& strm) {
    auto orginInputNumber = getOriginalInputsNumber();
    std::vector<MemoryPtr> inputs(orginInputNumber);

    for (size_t i = 0; i < orginInputNumber; i++) {
        inputs[i] = getSrcMemoryAtPort(i);
    }

    m_executor->execute(inputs, getDstMemoryAtPort(0));
}

bool RMSNorm::isSupportedOperation(const std::shared_ptr<const ov::Node>& op, std::string& errorMessage) noexcept {
    try {
        const auto rms = ov::as_type_ptr<const ov::op::internal::RMS>(op);
        if (rms) {
            if (!dnnl::impl::cpu::x64::mayiuse(dnnl::impl::cpu::x64::avx2)) {
                errorMessage = "RMSNorm needs avx2+.";
                return false;
            }
            // check the last dimension of data
            auto data_pshape = op->input_value(0).get_partial_shape();
            if (data_pshape.rank().is_dynamic()) {
                errorMessage = "RMSNorm data rank is not static.";
                return false;
            }
            const auto& data_rank = op->get_input_partial_shape(0).rank().get_length();
            if (data_rank <= 1) {
                errorMessage = "RMSNorm data rank must be greater than 1.";
                return false;
            }
            if (data_pshape[data_rank - 1].is_dynamic()) {
                errorMessage = "RMSNorm last dimension of data is not static.";
                return false;
            }

            // check scale
            if (op->get_input_partial_shape(1).is_dynamic()) {
                errorMessage = "RMSNorm scale shape is not static.";
                return false;
            }
            auto scale_pshape = op->get_input_partial_shape(1);
            if (scale_pshape.rank().get_length() > 1) {
                for (int64_t i = 0; i < scale_pshape.rank().get_length() - 1; i++) {
                    if (scale_pshape[i] != 1) {
                        errorMessage = "RMSNorm scale shape must be [1,..., N].";
                        return false;
                    }
                }
            }
        } else {
            errorMessage = "Only RMSNorm operation is supported";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace ov::intel_cpu::node
