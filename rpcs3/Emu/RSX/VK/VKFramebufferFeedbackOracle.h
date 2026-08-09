#pragma once

#include "util/types.hpp"
#include "Emu/RSX/Common/framebuffer_feedback_types.h"

#include <array>
#include <span>

namespace vk::framebuffer_feedback
{
	enum class texture_stage : u8
	{
		fragment,
		vertex,
	};

	enum reject_bit : u64
	{
		reject_unknown_coordinates = 1ull << 0,
		reject_unknown_coverage = 1ull << 1,
		reject_non_fragment_stage = 1ull << 2,
		reject_feedback_alias_count = 1ull << 3,
		reject_texture_opcode = 1ull << 4,
		reject_texture_instruction_count = 1ull << 5,
		reject_coordinate_source = 1ull << 6,
		reject_indexed_or_mixed_source = 1ull << 7,
		reject_texture_subresource = 1ull << 8,
		reject_inexact_region = 1ull << 9,
		reject_inexact_format = 1ull << 10,
		reject_inexact_remap = 1ull << 11,
		reject_texture_compare = 1ull << 12,
		reject_texture_offset_unknown = 1ull << 13,
		reject_bem_or_projection = 1ull << 14,
		reject_bias_gradient_or_lod = 1ull << 15,
		reject_non_nearest_filter = 1ull << 16,
		reject_non_clamp_address = 1ull << 17,
		reject_anisotropy = 1ull << 18,
		reject_same_draw_overlap_unknown = 1ull << 19,
		reject_a2c_or_sample_mask = 1ull << 20,
		reject_fragment_discard = 1ull << 21,
		reject_conditional_render = 1ull << 22,
		reject_depth_test = 1ull << 23,
		reject_stencil_test = 1ull << 24,
		reject_destination_dependent_write = 1ull << 25,
		reject_partial_color_export_or_mask = 1ull << 26,
		reject_multisample_target = 1ull << 27,
		reject_unknown_source_attachment = 1ull << 28,
		reject_non_live_rop_reason = 1ull << 29,
		reject_fragment_depth_export = 1ull << 30,
		reject_known_partial_coverage = 1ull << 31,
		reject_no_consumer = 1ull << 32,
		reject_coordinate_transform_mismatch = 1ull << 33,
		reject_coordinate_expression_unknown = 1ull << 34,
	};

	static constexpr usz reject_bit_count = 35;

	// Route-specific v1 gates. Unknown semantic axes are members of the strict
	// masks and therefore never qualify. Potential masks quantify the upper bound
	// if later shader/coverage analysis resolves only those explicit unknowns.
	static constexpr u64 local_read_reject_mask =
		reject_unknown_coordinates |
		reject_non_fragment_stage |
		reject_feedback_alias_count |
		reject_texture_opcode |
		reject_texture_instruction_count |
		reject_coordinate_source |
		reject_coordinate_transform_mismatch |
		reject_coordinate_expression_unknown |
		reject_indexed_or_mixed_source |
		reject_texture_subresource |
		reject_inexact_region |
		reject_inexact_format |
		reject_inexact_remap |
		reject_texture_compare |
		reject_texture_offset_unknown |
		reject_bem_or_projection |
		reject_bias_gradient_or_lod |
		reject_non_nearest_filter |
		reject_non_clamp_address |
		reject_anisotropy |
		reject_same_draw_overlap_unknown |
		reject_multisample_target |
		reject_unknown_source_attachment |
		reject_non_live_rop_reason |
		reject_no_consumer;

	static constexpr u64 ping_pong_reject_mask =
		reject_unknown_coverage |
		reject_known_partial_coverage |
		reject_texture_subresource |
		reject_inexact_region |
		reject_inexact_format |
		reject_inexact_remap |
		reject_a2c_or_sample_mask |
		reject_fragment_discard |
		reject_conditional_render |
		reject_depth_test |
		reject_stencil_test |
		reject_destination_dependent_write |
		reject_partial_color_export_or_mask |
		reject_multisample_target |
		reject_unknown_source_attachment |
		reject_non_live_rop_reason |
		reject_fragment_depth_export |
		reject_no_consumer;

	static constexpr u64 local_read_potential_reject_mask = local_read_reject_mask &
		~(reject_unknown_coordinates | reject_texture_offset_unknown | reject_same_draw_overlap_unknown);
	static constexpr u64 ping_pong_potential_reject_mask = ping_pong_reject_mask & ~reject_unknown_coverage;

	enum route_flag : u8
	{
		route_local_read = 1 << 0,
		route_local_read_potential = 1 << 1,
		route_ping_pong = 1 << 2,
		route_ping_pong_potential = 1 << 3,
	};

	constexpr u8 classify_routes(u64 reject_bits) noexcept
	{
		return
			(!(reject_bits & local_read_reject_mask) ? route_local_read : 0) |
			(!(reject_bits & local_read_potential_reject_mask) ? route_local_read_potential : 0) |
			(!(reject_bits & ping_pong_reject_mask) ? route_ping_pong : 0) |
			(!(reject_bits & ping_pong_potential_reject_mask) ? route_ping_pong_potential : 0);
	}

	struct safety_observation
	{
		bool exact_coordinates = false;
		bool exact_coverage = false;
		bool fragment_stage = false;
		bool single_feedback_alias = false;
		bool plain_texture_opcode = false;
		bool single_texture_instruction = false;
		bool direct_coordinate_source = false;
		bool coordinate_expression_known_identity = false;
		bool compatible_coordinate_transform = false;
		bool no_indexed_or_mixed_source = false;
		bool single_2d_subresource = false;
		bool exact_region = false;
		bool exact_format = false;
		bool exact_remap = false;
		bool no_texture_compare = false;
		bool texture_offsets_known_zero = false;
		bool no_bem_or_projection = false;
		bool no_bias_gradient_or_lod = false;
		bool nearest_filter = false;
		bool clamp_addressing = false;
		bool no_anisotropy = false;
		bool same_draw_overlap_known_absent = false;
		bool no_a2c_or_sample_mask = false;
		bool no_fragment_discard = false;
		bool no_conditional_render = false;
		bool no_depth_test = false;
		bool no_stencil_test = false;
		bool destination_independent_write = false;
		bool full_color_export_and_mask = false;
		bool single_sample_target = false;
		bool known_source_attachment = false;
		bool live_rop_reason = false;
		bool no_fragment_depth_export = false;
		bool no_known_partial_coverage = false;
		bool has_consumer = false;
	};

	constexpr u64 classify_rejects(const safety_observation& value) noexcept
	{
		u64 result = 0;
		result |= value.exact_coordinates ? 0 : reject_unknown_coordinates;
		result |= value.exact_coverage ? 0 : reject_unknown_coverage;
		result |= value.fragment_stage ? 0 : reject_non_fragment_stage;
		result |= value.single_feedback_alias ? 0 : reject_feedback_alias_count;
		result |= value.plain_texture_opcode ? 0 : reject_texture_opcode;
		result |= value.single_texture_instruction ? 0 : reject_texture_instruction_count;
		result |= value.direct_coordinate_source ? 0 : reject_coordinate_source;
		result |= value.coordinate_expression_known_identity ? 0 : reject_coordinate_expression_unknown;
		result |= value.compatible_coordinate_transform ? 0 : reject_coordinate_transform_mismatch;
		result |= value.no_indexed_or_mixed_source ? 0 : reject_indexed_or_mixed_source;
		result |= value.single_2d_subresource ? 0 : reject_texture_subresource;
		result |= value.exact_region ? 0 : reject_inexact_region;
		result |= value.exact_format ? 0 : reject_inexact_format;
		result |= value.exact_remap ? 0 : reject_inexact_remap;
		result |= value.no_texture_compare ? 0 : reject_texture_compare;
		result |= value.texture_offsets_known_zero ? 0 : reject_texture_offset_unknown;
		result |= value.no_bem_or_projection ? 0 : reject_bem_or_projection;
		result |= value.no_bias_gradient_or_lod ? 0 : reject_bias_gradient_or_lod;
		result |= value.nearest_filter ? 0 : reject_non_nearest_filter;
		result |= value.clamp_addressing ? 0 : reject_non_clamp_address;
		result |= value.no_anisotropy ? 0 : reject_anisotropy;
		result |= value.same_draw_overlap_known_absent ? 0 : reject_same_draw_overlap_unknown;
		result |= value.no_a2c_or_sample_mask ? 0 : reject_a2c_or_sample_mask;
		result |= value.no_fragment_discard ? 0 : reject_fragment_discard;
		result |= value.no_conditional_render ? 0 : reject_conditional_render;
		result |= value.no_depth_test ? 0 : reject_depth_test;
		result |= value.no_stencil_test ? 0 : reject_stencil_test;
		result |= value.destination_independent_write ? 0 : reject_destination_dependent_write;
		result |= value.full_color_export_and_mask ? 0 : reject_partial_color_export_or_mask;
		result |= value.single_sample_target ? 0 : reject_multisample_target;
		result |= value.known_source_attachment ? 0 : reject_unknown_source_attachment;
		result |= value.live_rop_reason ? 0 : reject_non_live_rop_reason;
		result |= value.no_fragment_depth_export ? 0 : reject_fragment_depth_export;
		result |= value.no_known_partial_coverage ? 0 : reject_known_partial_coverage;
		result |= value.has_consumer ? 0 : reject_no_consumer;
		return result;
	}

	struct draw_observation
	{
		u64 frame_id = 0;
		u64 draw_id = 0;
		u64 fp_hash = 0;
		u64 vp_hash = 0;
		u64 pipeline_hash = 0;
		u64 renderpass_key = 0;
		u64 command_buffer_identity = 0;
		u64 command_buffer_reset_id = 0;
		u32 fragment_control = 0;
		u64 pipeline_reject_bits = 0;
		s32 scissor_x = 0;
		s32 scissor_y = 0;
		u32 scissor_width = 0;
		u32 scissor_height = 0;
		u32 target_width = 0;
		u32 target_height = 0;
	};

	struct request_observation
	{
		u64 request_serial = 0;
		u64 snapshot_serial = 0;
		u64 copy_serial = 0;
		u64 source_identity = 0;
		u64 source_generation = 0;
		u64 view_identity = 0;
		u64 sampler_signature = 0;
		u64 texcoord_xform_signature = 0;
		u64 logical_bytes = 0;
		u64 command_buffer_identity = 0;
		u64 command_buffer_reset_id = 0;
		u32 request_address = 0;
		u32 source_address = 0;
		u32 pitch = 0;
		u32 gcm_format = 0;
		u32 remap = 0;
		u64 semantic_reject_bits = 0;
		u32 source_vk_format = 0;
		u32 view_vk_format = 0;
		u32 source_component_map = 0;
		u32 view_component_map = 0;
		u32 texcoord_scale_x = 0;
		u32 texcoord_scale_y = 0;
		u32 texcoord_bias_x = 0;
		u32 texcoord_bias_y = 0;
		u32 texcoord_clamp_min_x = 0;
		u32 texcoord_clamp_min_y = 0;
		u32 texcoord_clamp_max_x = 0;
		u32 texcoord_clamp_max_y = 0;
		u32 resolution_scale = 0;
		u32 wpos_scale = 0;
		u32 wpos_bias_x = 0;
		u32 wpos_bias_y = 0;
		u16 shader_window_height = 0;
		u16 source_width = 0;
		u16 source_height = 0;
		u16 texture_instruction_count = 0;
		u16 texture_opcode_class_mask = 0;
		u16 x = 0;
		u16 y = 0;
		u16 width = 0;
		u16 height = 0;
		u16 depth = 0;
		u8 bpp = 0;
		u8 texture_unit = 0;
		u8 source_attachment = 0xff;
		u8 coordinate_source_mask = 0;
		u8 texture_dimension = 0;
		u8 mipmap_count = 0;
		u8 sample_count = 0;
		u8 shader_window_origin = 0;
		u8 pixel_center = 0;
		texture_stage stage = texture_stage::fragment;
		rsx::framebuffer_feedback_copy_reason reason;
		rsx::framebuffer_feedback_oracle_outcome outcome;
		bool has_bound_view = false;
		bool uncached = false;
		bool indexed_coordinate_source = false;
		bool mixed_coordinate_source = false;
		bool coordinate_expression_known_identity = false;
		bool unnormalized_coordinates = false;
		bool texcoord_clamp = false;
	};

	struct subdraw_observation
	{
		u32 rsx_subdraw = 0;
		u32 host_subdraw = 0;
		u32 topology = 0;
		u32 vertex_count = 0;
		u32 instance_count = 0;
		u32 host_draw_count = 0;
		bool indexed = false;
	};

	struct reference_statistics
	{
		u64 requests = 0;
		u64 live_rop_requests = 0;
		u64 edge_clamped_requests = 0;
		u64 actual_copies = 0;
		u64 new_snapshots = 0;
		u64 refreshes = 0;
		u64 generation_reuses = 0;
		u64 static_hits = 0;
		u64 failures = 0;
		u64 uncached_copies = 0;
		u64 logical_copied_bytes = 0;
		u64 logical_reused_bytes = 0;
	};

	struct statistics
	{
		u64 frames = 0;
		u64 requests = 0;
		u64 live_rop_requests = 0;
		u64 edge_clamped_requests = 0;
		u64 new_snapshots = 0;
		u64 refreshes = 0;
		u64 generation_reuses = 0;
		u64 static_hits = 0;
		u64 failures = 0;
		u64 unknown_outcomes = 0;
		u64 uncached_copies = 0;
		u64 logical_copied_bytes = 0;
		u64 logical_reused_bytes = 0;
		u64 consumer_links = 0;
		u64 subdraw_groups = 0;
		u64 unconsumed_requests = 0;
		u64 orphan_requests = 0;
		u64 request_record_drops = 0;
		u64 topk_replacements = 0;
		u64 reconciled_frames = 0;
		u64 reconciliation_mismatches = 0;
		u64 submits_with_feedback = 0;
		u64 copy_map_records = 0;
		u64 copy_map_drops = 0;
		u64 copy_map_missing_serials = 0;
		u64 copy_map_duplicate_serials = 0;
		u64 copy_map_mismatches = 0;
	};

	struct group_key
	{
		u64 source_identity = 0;
		u64 fp_hash = 0;
		u64 vp_hash = 0;
		u64 pipeline_hash = 0;
		u64 sampler_signature = 0;
		u64 texcoord_xform_signature = 0;
		u64 draw_signature = 0;
		u32 request_address = 0;
		u32 source_address = 0;
		u32 pitch = 0;
		u32 gcm_format = 0;
		u32 remap = 0;
		u64 reject_bits = 0;
		u32 source_vk_format = 0;
		u32 view_vk_format = 0;
		u32 source_component_map = 0;
		u32 view_component_map = 0;
		u32 texcoord_scale_x = 0;
		u32 texcoord_scale_y = 0;
		u32 texcoord_bias_x = 0;
		u32 texcoord_bias_y = 0;
		u32 texcoord_clamp_min_x = 0;
		u32 texcoord_clamp_min_y = 0;
		u32 texcoord_clamp_max_x = 0;
		u32 texcoord_clamp_max_y = 0;
		u32 resolution_scale = 0;
		u32 wpos_scale = 0;
		u32 wpos_bias_x = 0;
		u32 wpos_bias_y = 0;
		s32 scissor_x = 0;
		s32 scissor_y = 0;
		u32 scissor_width = 0;
		u32 scissor_height = 0;
		u32 target_width = 0;
		u32 target_height = 0;
		u16 shader_window_height = 0;
		u16 source_width = 0;
		u16 source_height = 0;
		u16 texture_instruction_count = 0;
		u16 texture_opcode_class_mask = 0;
		u16 x = 0;
		u16 y = 0;
		u16 width = 0;
		u16 height = 0;
		u16 depth = 0;
		u8 bpp = 0;
		u8 texture_unit = 0;
		u8 source_attachment = 0xff;
		u8 feedback_alias_count = 0;
		u8 coordinate_source_mask = 0;
		u8 texture_dimension = 0;
		u8 mipmap_count = 0;
		u8 sample_count = 0;
		u8 shader_window_origin = 0;
		u8 pixel_center = 0;
		bool unnormalized_coordinates = false;
		bool texcoord_clamp = false;
		bool indexed_coordinate_source = false;
		bool mixed_coordinate_source = false;
		bool coordinate_expression_known_identity = false;
		texture_stage stage = texture_stage::fragment;
		rsx::framebuffer_feedback_copy_reason reason;
		rsx::framebuffer_feedback_oracle_outcome outcome;

		bool operator ==(const group_key&) const = default;
	};

	struct reject_counter
	{
		u64 requests = 0;
		u64 logical_bytes = 0;
		u64 actual_copies = 0;
		u64 actual_copy_bytes = 0;
	};

	struct reject_partition
	{
		reject_counter zero_rejects{};
		reject_counter unpartitioned{};
		std::array<reject_counter, reject_bit_count> by_bit{};
	};

	struct route_partition
	{
		reject_counter local_read{};
		reject_counter local_read_potential{};
		reject_counter ping_pong{};
		reject_counter ping_pong_potential{};
	};

	struct group_entry
	{
		group_key key{};
		u64 score_estimate = 0;
		u64 score_error = 0;
		u64 attributed_requests = 0;
		u64 consumer_links = 0;
		u64 logical_request_bytes = 0;
		u64 first_generation = 0;
		u64 last_generation = 0;
		u64 last_request_serial = 0;
		u64 last_snapshot_serial = 0;
		u64 last_copy_serial = 0;
		u64 last_frame = 0;
		u64 last_draw = 0;
		u64 last_command_buffer_identity = 0;
		u64 last_command_buffer_reset_id = 0;
		u64 last_submit_serial = 0;
		u32 last_rsx_subdraw = 0;
		u32 last_host_subdraw = 0;
		u32 last_topology = 0;
		u32 last_vertex_count = 0;
		u32 last_instance_count = 0;
		u32 last_host_draw_count = 0;
		bool last_indexed = false;

		bool occupied() const noexcept
		{
			return score_estimate != 0;
		}
	};

	struct copy_map_record
	{
		u64 copy_serial = 0;
		u64 reject_bits = 0;
		u64 logical_bytes = 0;
		u8 route_flags = 0;
		rsx::framebuffer_feedback_oracle_outcome outcome =
			rsx::framebuffer_feedback_oracle_outcome::none;
	};

	std::string format_copy_map_record(const copy_map_record& record, bool prepend_comma);

	struct summary
	{
		statistics frame{};
		statistics total{};
		reference_statistics last_reference{};
		bool last_frame_reconciled = true;
		u64 last_submit_serial = 0;
		u64 last_request_serial = 0;
		u64 last_snapshot_serial = 0;
		u64 last_copy_serial = 0;
		reject_partition frame_rejects{};
		reject_partition total_rejects{};
		route_partition frame_routes{};
		route_partition total_routes{};
	};

	class oracle
	{
	public:
		static constexpr usz pending_capacity = 64;
		static constexpr usz group_capacity = 64;
		static constexpr usz report_group_count = 12;
		static constexpr usz copy_map_capacity = 512;
		static constexpr usz copy_map_records_per_line = 32;

	private:
		struct pending_request
		{
			request_observation value{};
			bool attributed = false;
		};

		bool m_enabled = false;
		bool m_draw_active = false;
		bool m_draw_had_subdraw = false;
		draw_observation m_draw{};
		std::array<pending_request, pending_capacity> m_pending{};
		usz m_pending_count = 0;
		std::array<group_entry, group_capacity> m_groups{};
		std::array<copy_map_record, copy_map_capacity> m_copy_map{};
		usz m_copy_map_count = 0;
		u64 m_copy_map_frame_id = 0;
		statistics m_frame{};
		statistics m_last_completed_frame{};
		statistics m_total{};
		reject_partition m_frame_rejects{};
		reject_partition m_last_completed_frame_rejects{};
		reject_partition m_total_rejects{};
		route_partition m_frame_routes{};
		route_partition m_last_completed_frame_routes{};
		route_partition m_total_routes{};
		reference_statistics m_last_reference{};
		bool m_last_frame_reconciled = true;
		u64 m_last_report_time_us = 0;
		u64 m_last_submit_serial = 0;
		u64 m_last_request_serial = 0;
		u64 m_last_snapshot_serial = 0;
		u64 m_last_copy_serial = 0;
		bool m_command_buffer_has_feedback = false;
		u64 m_feedback_command_buffer_identity = 0;
		u64 m_feedback_command_buffer_reset_id = 0;

		void observe_group(pending_request& request, const subdraw_observation* subdraw);
		void append_copy_map(const request_observation& request, u64 reject_bits,
			u8 route_flags, u64 frame_id);
		void report_copy_map();
		void report(u64 now);

	public:
		void set_enabled(bool enabled);
		bool enabled() const noexcept;
		bool has_pending_requests() const noexcept;
		void begin_draw(const draw_observation& draw);
		void record_request(const request_observation& request);
		void note_subdraw(const subdraw_observation& subdraw);
		void finish_draw();
		void note_submit(u64 command_buffer_identity, u64 command_buffer_reset_id);
		void end_frame(const reference_statistics& reference);

		summary get_summary() const noexcept;
		const std::array<group_entry, group_capacity>& groups() const noexcept;
		std::span<const copy_map_record> copy_map() const noexcept;
	};
}
