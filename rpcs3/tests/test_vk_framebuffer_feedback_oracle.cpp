#include <gtest/gtest.h>

#include "Emu/RSX/VK/VKFramebufferFeedbackOracle.h"

#include <algorithm>
#include <bit>
#include <ranges>

namespace
{
	using namespace vk::framebuffer_feedback;

	safety_observation make_safe_observation()
	{
		return
		{
			.exact_coordinates = true,
			.exact_coverage = true,
			.fragment_stage = true,
			.single_feedback_alias = true,
			.plain_texture_opcode = true,
			.single_texture_instruction = true,
			.direct_coordinate_source = true,
			.coordinate_expression_known_identity = true,
			.compatible_coordinate_transform = true,
			.no_indexed_or_mixed_source = true,
			.single_2d_subresource = true,
			.exact_region = true,
			.exact_format = true,
			.exact_remap = true,
			.no_texture_compare = true,
			.texture_offsets_known_zero = true,
			.no_bem_or_projection = true,
			.no_bias_gradient_or_lod = true,
			.nearest_filter = true,
			.clamp_addressing = true,
			.no_anisotropy = true,
			.same_draw_overlap_known_absent = true,
			.no_a2c_or_sample_mask = true,
			.no_fragment_discard = true,
			.no_conditional_render = true,
			.no_depth_test = true,
			.no_stencil_test = true,
			.destination_independent_write = true,
			.full_color_export_and_mask = true,
			.single_sample_target = true,
			.known_source_attachment = true,
			.live_rop_reason = true,
			.no_fragment_depth_export = true,
			.no_known_partial_coverage = true,
			.has_consumer = true,
		};
	}

	draw_observation make_draw(u64 draw_id = 1)
	{
		return
		{
			.frame_id = 1,
			.draw_id = draw_id,
			.fp_hash = 0x100,
			.vp_hash = 0x200,
			.pipeline_hash = 0x300,
			.renderpass_key = 0x400,
			.command_buffer_identity = 0x500,
			.command_buffer_reset_id = 1,
			.scissor_width = 1280,
			.scissor_height = 720,
			.target_width = 1280,
			.target_height = 720,
		};
	}

	request_observation make_request(
		u64 serial,
		u64 bytes,
		rsx::framebuffer_feedback_oracle_outcome outcome,
		u64 semantic_reject_bits = 0)
	{
		return
		{
			.request_serial = serial,
			.snapshot_serial = serial,
			.copy_serial = outcome == rsx::framebuffer_feedback_oracle_outcome::new_snapshot ||
				outcome == rsx::framebuffer_feedback_oracle_outcome::refresh ? serial : 0,
			.source_identity = serial,
			.source_generation = 1,
			.logical_bytes = bytes,
			.command_buffer_identity = 0x500,
			.command_buffer_reset_id = 1,
			.semantic_reject_bits = semantic_reject_bits,
			.stage = texture_stage::fragment,
			.reason = rsx::framebuffer_feedback_copy_reason::live_rop,
			.outcome = outcome,
			.has_bound_view = true,
		};
	}

	void observe_consumed(oracle& value, request_observation request, u64 draw_id)
	{
		value.begin_draw(make_draw(draw_id));
		value.record_request(request);
		value.note_subdraw(
		{
			.rsx_subdraw = 1,
			.host_subdraw = 1,
			.topology = 3,
			.vertex_count = 3,
			.instance_count = 1,
			.host_draw_count = 1,
		});
		value.finish_draw();
	}

	TEST(TestFramebufferFeedbackOracle, ClassifierAndRouteMasksStayIndependent)
	{
		auto observation = make_safe_observation();
		EXPECT_EQ(classify_rejects(observation), 0);

		observation.exact_format = false;
		EXPECT_EQ(classify_rejects(observation), reject_inexact_format);

		EXPECT_EQ(local_read_reject_mask & reject_known_partial_coverage, 0);
		EXPECT_EQ(local_read_reject_mask & reject_destination_dependent_write, 0);
		EXPECT_EQ(ping_pong_reject_mask & reject_unknown_coordinates, 0);
		EXPECT_EQ(ping_pong_reject_mask & reject_texture_opcode, 0);
		EXPECT_EQ(local_read_potential_reject_mask & reject_unknown_coordinates, 0);
		EXPECT_NE(local_read_potential_reject_mask & reject_coordinate_expression_unknown, 0);
		EXPECT_EQ(ping_pong_potential_reject_mask & reject_unknown_coverage, 0);
		EXPECT_NE(ping_pong_potential_reject_mask & reject_known_partial_coverage, 0);
		EXPECT_EQ(classify_routes(0), 0xf);
		EXPECT_EQ(classify_routes(reject_unknown_coordinates), 0xe);
		EXPECT_EQ(classify_routes(reject_known_partial_coverage), 0x3);
	}

	TEST(TestFramebufferFeedbackOracle, ActualCopyRouteCountersExcludeReuseStaticAndFailure)
	{
		oracle value;
		value.set_enabled(true);
		observe_consumed(value, make_request(1, 10, rsx::framebuffer_feedback_oracle_outcome::new_snapshot), 1);
		observe_consumed(value, make_request(2, 20, rsx::framebuffer_feedback_oracle_outcome::refresh), 2);
		observe_consumed(value, make_request(3, 30, rsx::framebuffer_feedback_oracle_outcome::generation_reuse), 3);
		observe_consumed(value, make_request(4, 40, rsx::framebuffer_feedback_oracle_outcome::static_hit), 4);
		observe_consumed(value, make_request(5, 50, rsx::framebuffer_feedback_oracle_outcome::failure), 5);
		value.end_frame({});

		const auto summary = value.get_summary();
		EXPECT_EQ(summary.frame.requests, 5);
		EXPECT_EQ(summary.frame_routes.local_read.requests, 4);
		EXPECT_EQ(summary.frame_routes.local_read.actual_copies, 2);
		EXPECT_EQ(summary.frame_routes.local_read.actual_copy_bytes, 30);
		EXPECT_EQ(summary.frame_routes.ping_pong.actual_copies, 2);
		EXPECT_EQ(summary.frame_routes.ping_pong.actual_copy_bytes, 30);
		EXPECT_EQ(summary.frame_rejects.by_bit[32].requests, 1);
		EXPECT_EQ(summary.frame_rejects.by_bit[32].actual_copies, 0);
		EXPECT_EQ(summary.frame.copy_map_records, 2);
		EXPECT_EQ(summary.frame.copy_map_drops, 0);
		EXPECT_EQ(summary.frame.copy_map_missing_serials, 0);
		EXPECT_EQ(summary.frame.copy_map_duplicate_serials, 0);
	}

	TEST(TestFramebufferFeedbackOracle, ReusedPendingSlotResetsAttribution)
	{
		oracle value;
		value.set_enabled(true);
		auto request = make_request(1, 7, rsx::framebuffer_feedback_oracle_outcome::static_hit);
		observe_consumed(value, request, 1);
		observe_consumed(value, request, 2);

		u64 attributed_requests = 0;
		u64 consumer_links = 0;
		u64 logical_bytes = 0;
		for (const auto& group : value.groups())
		{
			attributed_requests += group.attributed_requests;
			consumer_links += group.consumer_links;
			logical_bytes += group.logical_request_bytes;
		}

		EXPECT_EQ(attributed_requests, 2);
		EXPECT_EQ(consumer_links, 2);
		EXPECT_EQ(logical_bytes, 14);
	}

	TEST(TestFramebufferFeedbackOracle, AliasCountUsesUniqueBindingSlotsNotRetries)
	{
		oracle value;
		value.set_enabled(true);
		value.begin_draw(make_draw());
		value.record_request(make_request(1, 16,
			rsx::framebuffer_feedback_oracle_outcome::new_snapshot));
		auto retry = make_request(2, 16,
			rsx::framebuffer_feedback_oracle_outcome::generation_reuse);
		retry.texture_unit = 0;
		value.record_request(retry);
		value.note_subdraw({.vertex_count = 3, .instance_count = 1, .host_draw_count = 1});
		value.finish_draw();

		ASSERT_EQ(value.copy_map().size(), 1);
		EXPECT_EQ(value.copy_map()[0].route_flags, 0xf);
		EXPECT_EQ(value.copy_map()[0].reject_bits & reject_feedback_alias_count, 0);
	}

	TEST(TestFramebufferFeedbackOracle, NoConsumerStaysInDenominatorButCannotQualify)
	{
		oracle value;
		value.set_enabled(true);
		value.begin_draw(make_draw());
		value.record_request(make_request(1, 64, rsx::framebuffer_feedback_oracle_outcome::new_snapshot));
		value.finish_draw();

		ASSERT_EQ(value.copy_map().size(), 1);
		EXPECT_EQ(value.copy_map()[0].copy_serial, 1);
		EXPECT_EQ(value.copy_map()[0].route_flags, 0);
		EXPECT_EQ(value.copy_map()[0].reject_bits, reject_no_consumer);
		EXPECT_EQ(value.copy_map()[0].logical_bytes, 64);
		EXPECT_EQ(value.copy_map()[0].outcome,
			rsx::framebuffer_feedback_oracle_outcome::new_snapshot);

		value.end_frame({});
		const auto summary = value.get_summary();
		EXPECT_EQ(summary.frame.requests, 1);
		EXPECT_EQ(summary.frame_rejects.by_bit[32].requests, 1);
		EXPECT_EQ(summary.frame_rejects.by_bit[32].actual_copies, 1);
		EXPECT_EQ(summary.frame_rejects.by_bit[32].actual_copy_bytes, 64);
		EXPECT_EQ(summary.frame_routes.local_read.actual_copies, 0);
		EXPECT_EQ(summary.frame_routes.ping_pong.actual_copies, 0);
		EXPECT_EQ(summary.frame.copy_map_records, 1);
	}

	TEST(TestFramebufferFeedbackOracle, CopyMapPersistsExactRouteAndRejectContract)
	{
		oracle value;
		value.set_enabled(true);
		auto draw = make_draw();
		draw.pipeline_reject_bits = reject_known_partial_coverage;
		value.begin_draw(draw);
		value.record_request(make_request(77, 96,
			rsx::framebuffer_feedback_oracle_outcome::refresh,
			reject_unknown_coordinates));
		value.note_subdraw({.vertex_count = 3, .instance_count = 1, .host_draw_count = 1});
		value.finish_draw();

		ASSERT_EQ(value.copy_map().size(), 1);
		const auto& record = value.copy_map()[0];
		EXPECT_EQ(record.copy_serial, 77);
		EXPECT_EQ(record.route_flags, route_local_read_potential);
		EXPECT_EQ(record.reject_bits,
			reject_unknown_coordinates | reject_known_partial_coverage);
		EXPECT_EQ(record.logical_bytes, 96);
		EXPECT_EQ(record.outcome, rsx::framebuffer_feedback_oracle_outcome::refresh);
		EXPECT_EQ(format_copy_map_record(record, false),
			"77:2:0000000080000001:96:R");
		EXPECT_EQ(format_copy_map_record(record, true),
			",77:2:0000000080000001:96:R");
	}

	TEST(TestFramebufferFeedbackOracle, CopyMapIntersectsEveryConsumerOfSnapshot)
	{
		oracle value;
		value.set_enabled(true);
		observe_consumed(value, make_request(1, 64,
			rsx::framebuffer_feedback_oracle_outcome::new_snapshot), 1);

		auto incompatible_reuse = make_request(2, 64,
			rsx::framebuffer_feedback_oracle_outcome::generation_reuse,
			reject_inexact_format);
		incompatible_reuse.copy_serial = 1;
		observe_consumed(value, incompatible_reuse, 2);

		ASSERT_EQ(value.copy_map().size(), 1);
		const auto& record = value.copy_map()[0];
		EXPECT_EQ(record.copy_serial, 1);
		EXPECT_EQ(record.route_flags, 0);
		EXPECT_EQ(record.reject_bits, reject_inexact_format);
		EXPECT_EQ(record.logical_bytes, 64);
		EXPECT_EQ(record.outcome,
			rsx::framebuffer_feedback_oracle_outcome::new_snapshot);

		value.end_frame({});
		const auto summary = value.get_summary();
		EXPECT_EQ(summary.frame.copy_map_records, 1);
		EXPECT_EQ(summary.frame.copy_map_duplicate_serials, 0);
		EXPECT_EQ(summary.frame.copy_map_mismatches, 0);
		EXPECT_EQ(summary.frame_rejects.zero_rejects.requests, 1);
		EXPECT_EQ(summary.frame_rejects.zero_rejects.actual_copies, 0);
		const auto& format_reject = summary.frame_rejects.by_bit[
			std::countr_zero(static_cast<u64>(reject_inexact_format))];
		EXPECT_EQ(format_reject.requests, 1);
		EXPECT_EQ(format_reject.actual_copies, 1);
		EXPECT_EQ(format_reject.actual_copy_bytes, 64);
		EXPECT_EQ(summary.frame_routes.local_read.requests, 1);
		EXPECT_EQ(summary.frame_routes.local_read.actual_copies, 0);
		EXPECT_EQ(summary.frame_routes.ping_pong.requests, 1);
		EXPECT_EQ(summary.frame_routes.ping_pong.actual_copies, 0);
	}

	TEST(TestFramebufferFeedbackOracle, ExactRejectMarginalsCountRequestsAndBytes)
	{
		oracle value;
		value.set_enabled(true);
		observe_consumed(value, make_request(1, 96,
			rsx::framebuffer_feedback_oracle_outcome::refresh,
			reject_inexact_format | reject_texture_compare), 1);
		value.end_frame({});

		const auto summary = value.get_summary();
		for (const auto bit : {reject_inexact_format, reject_texture_compare})
		{
			const auto& counter = summary.frame_rejects.by_bit[std::countr_zero(static_cast<u64>(bit))];
			EXPECT_EQ(counter.requests, 1);
			EXPECT_EQ(counter.logical_bytes, 96);
			EXPECT_EQ(counter.actual_copies, 1);
			EXPECT_EQ(counter.actual_copy_bytes, 96);
		}
		EXPECT_EQ(summary.frame_rejects.zero_rejects.requests, 0);
	}

	TEST(TestFramebufferFeedbackOracle, SpaceSavingKeepsHeavyHitterAndBoundsGroups)
	{
		oracle value;
		value.set_enabled(true);
		for (u64 repeat = 0; repeat < 101; ++repeat)
		{
			auto request = make_request(1, 1, rsx::framebuffer_feedback_oracle_outcome::static_hit);
			observe_consumed(value, request, repeat + 1);
		}
		for (u64 serial = 2; serial <= oracle::group_capacity + 11; ++serial)
		{
			observe_consumed(value, make_request(serial, 1,
				rsx::framebuffer_feedback_oracle_outcome::static_hit), serial + 200);
		}

		const auto& groups = value.groups();
		EXPECT_EQ(std::ranges::count_if(groups, [](const group_entry& entry) { return entry.occupied(); }),
			oracle::group_capacity);
		const auto heavy = std::ranges::find_if(groups,
			[](const group_entry& entry) { return entry.occupied() && entry.key.source_identity == 1; });
		ASSERT_NE(heavy, groups.end());
		EXPECT_EQ(heavy->attributed_requests, 101);
		EXPECT_GE(value.get_summary().frame.topk_replacements, 11);
	}

	TEST(TestFramebufferFeedbackOracle, CopyMapCapacityAndSerialDefectsAreExplicit)
	{
		oracle value;
		value.set_enabled(true);
		for (u64 serial = 1; serial <= oracle::copy_map_capacity + 1; ++serial)
		{
			observe_consumed(value, make_request(serial, 1,
				rsx::framebuffer_feedback_oracle_outcome::new_snapshot), serial);
		}

		auto duplicate = make_request(1, 1, rsx::framebuffer_feedback_oracle_outcome::refresh);
		observe_consumed(value, duplicate, oracle::copy_map_capacity + 2);
		auto missing = make_request(oracle::copy_map_capacity + 3, 1,
			rsx::framebuffer_feedback_oracle_outcome::refresh);
		missing.copy_serial = 0;
		observe_consumed(value, missing, oracle::copy_map_capacity + 3);

		const auto summary = value.get_summary();
		EXPECT_EQ(summary.frame.copy_map_records, oracle::copy_map_capacity);
		EXPECT_EQ(summary.frame.copy_map_drops, 1);
		EXPECT_EQ(summary.frame.copy_map_duplicate_serials, 1);
		EXPECT_EQ(summary.frame.copy_map_missing_serials, 1);
	}
}
