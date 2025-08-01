// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "intel_gpu/plugin/program_builder.hpp"
#include "intel_gpu/plugin/common_utils.hpp"

#include "openvino/op/constant.hpp"
#include "openvino/op/paged_attention.hpp"

#include "intel_gpu/primitives/paged_attention.hpp"

namespace ov {
namespace op {
namespace internal {
using PagedAttentionExtension = ov::op::PagedAttentionExtension;
}  // namespace internal
}  // namespace op
}  // namespace ov

namespace ov::intel_gpu {

static void CreatePagedAttentionExtensionOp(ProgramBuilder& p, const std::shared_ptr<ov::op::PagedAttentionExtension>& op) {
    validate_inputs_count(op, {14, 17});
    auto inputs = p.GetInputInfo(op);
    auto prim = cldnn::paged_attention(layer_type_name_ID(op), inputs);

    const auto& rt_info = op->get_rt_info();
    const auto k_head_size_id = "k_head_size";
    const auto v_head_size_id = "v_head_size";
    const auto num_k_heads_id = "num_k_heads";
    const auto has_rt_params = rt_info.find(k_head_size_id) != rt_info.end() &&
                               rt_info.find(v_head_size_id) != rt_info.end() &&
                               rt_info.find(num_k_heads_id) != rt_info.end();

    auto query_ps = op->get_input_partial_shape(0);
    auto key_cache_ps = op->get_input_partial_shape(3);
    auto value_cache_ps = op->get_input_partial_shape(4);

    auto k_head_size = has_rt_params ? rt_info.at(k_head_size_id).as<int64_t>() : key_cache_ps[2].get_length();
    auto v_head_size = has_rt_params ? rt_info.at(v_head_size_id).as<int64_t>() : value_cache_ps[3].get_length();
    auto kv_heads_num = has_rt_params ? rt_info.at(num_k_heads_id).as<int64_t>() : key_cache_ps[1].get_length();

    // WA: in some cases, the query input may have a bounded dimension
    // Use input shape of the input node in such cases
    auto heads_num = 0;
    auto query_merged_dim = query_ps[1];
    if (query_merged_dim.is_static()) {
        heads_num = query_merged_dim.get_length() / k_head_size;
    } else {
        auto reshape_input = op->get_input_node_shared_ptr(0)->get_input_partial_shape(0);
        heads_num = reshape_input[2].get_length();
    }

    prim.k_head_size = k_head_size;
    prim.v_head_size = v_head_size;
    prim.kv_heads_num = kv_heads_num;
    prim.heads_num = heads_num;

    const size_t scale_idx = cldnn::paged_attention::PagedAttentionInputIdx::SCALE;
    const size_t sliding_window_idx = cldnn::paged_attention::PagedAttentionInputIdx::SLIDING_WINDOW;
    const size_t alibi_idx = cldnn::paged_attention::PagedAttentionInputIdx::ALIBI;
    const size_t score_aggregation_idx = cldnn::paged_attention::PagedAttentionInputIdx::SCORE_AGGREGATION;

    auto scale_const = ov::as_type_ptr<ov::op::v0::Constant>(op->get_input_node_shared_ptr(scale_idx));
    if (scale_const) {
        OPENVINO_ASSERT(ov::shape_size(scale_const->get_output_shape(0)) == 1);
        prim.scale_val = scale_const->cast_vector<float>()[0];
    } else {
        prim.scale_val = std::optional<float>();
    }

    auto sliding_windows_const = ov::as_type_ptr<ov::op::v0::Constant>(op->get_input_node_shared_ptr(sliding_window_idx));
    if (sliding_windows_const) {
        OPENVINO_ASSERT(ov::shape_size(sliding_windows_const->get_output_shape(0)) == 1);
        prim.sliding_window = sliding_windows_const->cast_vector<size_t>()[0];
    }

    auto alibi_const = ov::as_type_ptr<ov::op::v0::Constant>(op->get_input_node_shared_ptr(alibi_idx));
    OPENVINO_ASSERT(alibi_const != nullptr);
    prim.has_alibi = ov::shape_size(alibi_const->get_output_shape(0)) > 0;

    auto score_aggregation_input = ov::as_type_ptr<ov::op::v0::Parameter>(op->get_input_node_shared_ptr(score_aggregation_idx));
    if (score_aggregation_input && score_aggregation_input->get_output_partial_shape(0).is_dynamic() && op->get_output_size() > 1) {
        prim.has_score_aggregation = true;
    }

    prim.has_rotated_blocks = op->get_input_size() > cldnn::paged_attention::PagedAttentionInputIdx::ROTATION_TRIG_LUT;
    prim.is_key_by_channel = p.get_config().get_key_cache_quant_mode() == ov::internal::CacheQuantMode::BY_CHANNEL;
    prim.num_outputs = 1;

    if (op->get_output_size() > 1) {
        const auto scores_output_idx = 1;
        const auto& users = op->get_output_target_inputs(scores_output_idx);
        if (users.size() > 0) {
            prim.num_outputs++; // Add scores output
        }
    }

    p.add_primitive(*op, prim);
}

REGISTER_FACTORY_IMPL(internal, PagedAttentionExtension)

}  // namespace ov::intel_gpu
