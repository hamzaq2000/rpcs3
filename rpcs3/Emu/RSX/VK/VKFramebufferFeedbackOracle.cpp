#include "stdafx.h"
#include "VKFramebufferFeedbackOracle.h"

#include "Emu/Cell/timers.hpp"

#include <bit>

namespace vk::framebuffer_feedback
{
	namespace
	{
		constexpr u64 hash_seed = 14695981039346656037ull;
		constexpr u64 hash_prime = 1099511628211ull;

		constexpr u64 hash_append(u64 hash, u64 value) noexcept
		{
			for (u32 index = 0; index < 8; ++index)
			{
				hash ^= static_cast<u8>(value >> (index * 8));
				hash *= hash_prime;
			}
			return hash;
		}

		u64 make_draw_signature(const draw_observation& draw, const subdraw_observation* subdraw) noexcept
		{
			u64 hash = hash_seed;
			hash = hash_append(hash, draw.renderpass_key);
			hash = hash_append(hash, draw.fragment_control);
			hash = hash_append(hash, static_cast<u32>(draw.scissor_x));
			hash = hash_append(hash, static_cast<u32>(draw.scissor_y));
			hash = hash_append(hash, draw.scissor_width);
			hash = hash_append(hash, draw.scissor_height);
			hash = hash_append(hash, draw.target_width);
			hash = hash_append(hash, draw.target_height);

			if (!subdraw)
			{
				return hash_append(hash, umax);
			}

			hash = hash_append(hash, subdraw->rsx_subdraw);
			hash = hash_append(hash, subdraw->host_subdraw);
			hash = hash_append(hash, subdraw->topology);
			hash = hash_append(hash, subdraw->vertex_count);
			hash = hash_append(hash, subdraw->instance_count);
			hash = hash_append(hash, subdraw->host_draw_count);
			return hash_append(hash, subdraw->indexed);
		}

		const char* get_reason_name(rsx::framebuffer_feedback_copy_reason reason) noexcept
		{
			switch (reason)
			{
			case rsx::framebuffer_feedback_copy_reason::live_rop:
				return "live";
			case rsx::framebuffer_feedback_copy_reason::edge_clamped_merge:
				return "edge";
			default:
				return "none";
			}
		}

		const char* get_outcome_name(rsx::framebuffer_feedback_oracle_outcome outcome) noexcept
		{
			switch (outcome)
			{
			case rsx::framebuffer_feedback_oracle_outcome::new_snapshot:
				return "new";
			case rsx::framebuffer_feedback_oracle_outcome::refresh:
				return "refresh";
			case rsx::framebuffer_feedback_oracle_outcome::generation_reuse:
				return "reuse";
			case rsx::framebuffer_feedback_oracle_outcome::static_hit:
				return "static";
			case rsx::framebuffer_feedback_oracle_outcome::failure:
				return "fail";
			default:
				return "unknown";
			}
		}

		void accumulate(statistics& dst, const statistics& src) noexcept
		{
#define FB_ACCUMULATE(member) dst.member += src.member
			FB_ACCUMULATE(frames);
			FB_ACCUMULATE(requests);
			FB_ACCUMULATE(live_rop_requests);
			FB_ACCUMULATE(edge_clamped_requests);
			FB_ACCUMULATE(new_snapshots);
			FB_ACCUMULATE(refreshes);
			FB_ACCUMULATE(generation_reuses);
			FB_ACCUMULATE(static_hits);
			FB_ACCUMULATE(failures);
			FB_ACCUMULATE(unknown_outcomes);
			FB_ACCUMULATE(uncached_copies);
			FB_ACCUMULATE(logical_copied_bytes);
			FB_ACCUMULATE(logical_reused_bytes);
			FB_ACCUMULATE(consumer_links);
			FB_ACCUMULATE(subdraw_groups);
			FB_ACCUMULATE(unconsumed_requests);
			FB_ACCUMULATE(orphan_requests);
			FB_ACCUMULATE(request_record_drops);
			FB_ACCUMULATE(topk_replacements);
			FB_ACCUMULATE(reconciled_frames);
			FB_ACCUMULATE(reconciliation_mismatches);
			FB_ACCUMULATE(submits_with_feedback);
			FB_ACCUMULATE(copy_map_records);
			FB_ACCUMULATE(copy_map_drops);
			FB_ACCUMULATE(copy_map_missing_serials);
			FB_ACCUMULATE(copy_map_duplicate_serials);
			FB_ACCUMULATE(copy_map_mismatches);
#undef FB_ACCUMULATE
		}

		bool is_successful_binding(rsx::framebuffer_feedback_oracle_outcome outcome) noexcept
		{
			return outcome != rsx::framebuffer_feedback_oracle_outcome::failure &&
				outcome != rsx::framebuffer_feedback_oracle_outcome::none;
		}

		bool is_actual_copy(rsx::framebuffer_feedback_oracle_outcome outcome) noexcept
		{
			return outcome == rsx::framebuffer_feedback_oracle_outcome::new_snapshot ||
				outcome == rsx::framebuffer_feedback_oracle_outcome::refresh;
		}

		void add_request_counter(reject_counter& counter, const request_observation& request) noexcept
		{
			counter.requests++;
			counter.logical_bytes += request.logical_bytes;
		}

		void add_actual_copy_counter(reject_counter& counter,
			const copy_map_record& record) noexcept
		{
			counter.actual_copies++;
			counter.actual_copy_bytes += record.logical_bytes;
		}

		void accumulate(reject_counter& dst, const reject_counter& src) noexcept
		{
			dst.requests += src.requests;
			dst.logical_bytes += src.logical_bytes;
			dst.actual_copies += src.actual_copies;
			dst.actual_copy_bytes += src.actual_copy_bytes;
		}

		void accumulate(reject_partition& dst, const reject_partition& src) noexcept
		{
			accumulate(dst.zero_rejects, src.zero_rejects);
			accumulate(dst.unpartitioned, src.unpartitioned);
			for (usz index = 0; index < reject_bit_count; ++index)
			{
				accumulate(dst.by_bit[index], src.by_bit[index]);
			}
		}

		void accumulate(route_partition& dst, const route_partition& src) noexcept
		{
			accumulate(dst.local_read, src.local_read);
			accumulate(dst.local_read_potential, src.local_read_potential);
			accumulate(dst.ping_pong, src.ping_pong);
			accumulate(dst.ping_pong_potential, src.ping_pong_potential);
		}
	}

	std::string format_copy_map_record(const copy_map_record& record, bool prepend_comma)
	{
		const char outcome = record.outcome == rsx::framebuffer_feedback_oracle_outcome::new_snapshot
			? 'N' : 'R';
		return fmt::format("%s%llu:%x:%016llx:%llu:%c",
			prepend_comma ? "," : "", record.copy_serial,
			static_cast<u32>(record.route_flags), record.reject_bits,
			record.logical_bytes, outcome);
	}

	void oracle::set_enabled(bool enabled)
	{
		if (m_enabled == enabled)
		{
			return;
		}

		if (!enabled)
		{
			m_draw_active = false;
			m_draw_had_subdraw = false;
			m_pending_count = 0;
			m_command_buffer_has_feedback = false;
			// A later opt-in session must not reconcile a partial, abandoned frame
			// against a fresh texture-cache frame denominator.
			m_frame = {};
			m_frame_rejects = {};
			m_frame_routes = {};
			m_copy_map_count = 0;
			m_copy_map_frame_id = 0;
		}

		m_enabled = enabled;
	}

	bool oracle::enabled() const noexcept
	{
		return m_enabled;
	}

	bool oracle::has_pending_requests() const noexcept
	{
		return m_enabled && m_draw_active && m_pending_count;
	}

	void oracle::begin_draw(const draw_observation& draw)
	{
		if (!m_enabled)
		{
			return;
		}

		if (m_draw_active)
		{
			finish_draw();
		}

		m_draw = draw;
		m_draw_active = true;
		m_draw_had_subdraw = false;
		m_pending_count = 0;
	}

	void oracle::record_request(const request_observation& request)
	{
		if (!m_enabled)
		{
			return;
		}

		m_frame.requests++;
		m_last_request_serial = std::max(m_last_request_serial, request.request_serial);
		m_last_snapshot_serial = std::max(m_last_snapshot_serial, request.snapshot_serial);
		m_last_copy_serial = std::max(m_last_copy_serial, request.copy_serial);
		if (request.reason == rsx::framebuffer_feedback_copy_reason::live_rop)
		{
			m_frame.live_rop_requests++;
		}
		else if (request.reason == rsx::framebuffer_feedback_copy_reason::edge_clamped_merge)
		{
			m_frame.edge_clamped_requests++;
		}

		switch (request.outcome)
		{
		case rsx::framebuffer_feedback_oracle_outcome::new_snapshot:
			m_frame.new_snapshots++;
			m_frame.logical_copied_bytes += request.logical_bytes;
			break;
		case rsx::framebuffer_feedback_oracle_outcome::refresh:
			m_frame.refreshes++;
			m_frame.logical_copied_bytes += request.logical_bytes;
			break;
		case rsx::framebuffer_feedback_oracle_outcome::generation_reuse:
			m_frame.generation_reuses++;
			m_frame.logical_reused_bytes += request.logical_bytes;
			break;
		case rsx::framebuffer_feedback_oracle_outcome::static_hit:
			m_frame.static_hits++;
			break;
		case rsx::framebuffer_feedback_oracle_outcome::failure:
			m_frame.failures++;
			break;
		default:
			m_frame.unknown_outcomes++;
			break;
		}

		if (request.uncached && request.outcome == rsx::framebuffer_feedback_oracle_outcome::new_snapshot)
		{
			m_frame.uncached_copies++;
		}

		if (!m_draw_active)
		{
			m_frame.orphan_requests++;
			add_request_counter(m_frame_rejects.unpartitioned, request);
			append_copy_map(request, request.semantic_reject_bits | reject_no_consumer, 0, 0);
			return;
		}

		m_command_buffer_has_feedback = true;
		m_feedback_command_buffer_identity = request.command_buffer_identity;
		m_feedback_command_buffer_reset_id = request.command_buffer_reset_id;

		if (m_pending_count == m_pending.size())
		{
			m_frame.request_record_drops++;
			add_request_counter(m_frame_rejects.unpartitioned, request);
			append_copy_map(request,
				m_draw.pipeline_reject_bits | request.semantic_reject_bits | reject_no_consumer,
				0, m_draw.frame_id);
			return;
		}

		m_pending[m_pending_count++] =
		{
			.value = request,
			.attributed = false,
		};
	}

	void oracle::append_copy_map(const request_observation& request, u64 reject_bits,
		u8 route_flags, u64 frame_id)
	{
		if (!is_successful_binding(request.outcome))
		{
			return;
		}

		if (!request.copy_serial)
		{
			if (is_actual_copy(request.outcome))
			{
				m_frame.copy_map_missing_serials++;
			}
			return;
		}

		for (usz index = 0; index < m_copy_map_count; ++index)
		{
			if (m_copy_map[index].copy_serial == request.copy_serial)
			{
				if (is_actual_copy(request.outcome))
				{
					m_frame.copy_map_duplicate_serials++;
					return;
				}

				// A snapshot copy is removable only if every consumer of that exact
				// snapshot version is eligible for the route. Preserve the physical
				// copy's N/R outcome and bytes while conservatively combining all
				// subsequent generation-reuse/static-hit consumers.
				m_copy_map[index].reject_bits |= reject_bits;
				m_copy_map[index].route_flags = classify_routes(m_copy_map[index].reject_bits);
				m_copy_map_frame_id = std::max(m_copy_map_frame_id, frame_id);
				return;
			}
		}

		if (!is_actual_copy(request.outcome))
		{
			// Frame-local snapshots are created before they can be reused. A
			// non-copy consumer whose serial has no base N/R record means the
			// capture can no longer provide a complete serial-to-copy join.
			m_frame.copy_map_mismatches++;
			return;
		}

		if (m_copy_map_count == m_copy_map.size())
		{
			m_frame.copy_map_drops++;
			return;
		}

		m_copy_map[m_copy_map_count++] =
		{
			.copy_serial = request.copy_serial,
			.reject_bits = reject_bits,
			.logical_bytes = request.logical_bytes,
			.route_flags = route_flags,
			.outcome = request.outcome,
		};
		m_copy_map_frame_id = std::max(m_copy_map_frame_id, frame_id);
		m_frame.copy_map_records++;
	}

	void oracle::observe_group(pending_request& request, const subdraw_observation* subdraw)
	{
		const bool request_credit = !request.attributed;
		const bool consumer_credit = subdraw && request.value.has_bound_view &&
			is_successful_binding(request.value.outcome);

		group_key key{};
		key.source_identity = request.value.source_identity;
		key.fp_hash = m_draw.fp_hash;
		key.vp_hash = m_draw.vp_hash;
		key.pipeline_hash = m_draw.pipeline_hash;
		key.sampler_signature = request.value.sampler_signature;
		key.texcoord_xform_signature = request.value.texcoord_xform_signature;
		key.draw_signature = make_draw_signature(m_draw, subdraw);
		key.request_address = request.value.request_address;
		key.source_address = request.value.source_address;
		key.pitch = request.value.pitch;
		key.gcm_format = request.value.gcm_format;
		key.remap = request.value.remap;

		u32 feedback_alias_mask = 0;
		for (usz index = 0; index < m_pending_count; ++index)
		{
			const auto& alias = m_pending[index].value;
			if (alias.has_bound_view && is_successful_binding(alias.outcome))
			{
				const u32 stage_offset = alias.stage == texture_stage::fragment ? 0 : 16;
				feedback_alias_mask |= 1u << (stage_offset + alias.texture_unit);
			}
		}
		const u8 feedback_alias_count = static_cast<u8>(std::popcount(feedback_alias_mask));

		key.reject_bits = m_draw.pipeline_reject_bits | request.value.semantic_reject_bits;
		if (feedback_alias_count != 1)
		{
			key.reject_bits |= reject_feedback_alias_count;
		}
		if (!consumer_credit)
		{
			key.reject_bits |= reject_no_consumer;
		}
		key.source_vk_format = request.value.source_vk_format;
		key.view_vk_format = request.value.view_vk_format;
		key.source_component_map = request.value.source_component_map;
		key.view_component_map = request.value.view_component_map;
		key.texcoord_scale_x = request.value.texcoord_scale_x;
		key.texcoord_scale_y = request.value.texcoord_scale_y;
		key.texcoord_bias_x = request.value.texcoord_bias_x;
		key.texcoord_bias_y = request.value.texcoord_bias_y;
		key.texcoord_clamp_min_x = request.value.texcoord_clamp_min_x;
		key.texcoord_clamp_min_y = request.value.texcoord_clamp_min_y;
		key.texcoord_clamp_max_x = request.value.texcoord_clamp_max_x;
		key.texcoord_clamp_max_y = request.value.texcoord_clamp_max_y;
		key.resolution_scale = request.value.resolution_scale;
		key.wpos_scale = request.value.wpos_scale;
		key.wpos_bias_x = request.value.wpos_bias_x;
		key.wpos_bias_y = request.value.wpos_bias_y;
		key.scissor_x = m_draw.scissor_x;
		key.scissor_y = m_draw.scissor_y;
		key.scissor_width = m_draw.scissor_width;
		key.scissor_height = m_draw.scissor_height;
		key.target_width = m_draw.target_width;
		key.target_height = m_draw.target_height;
		key.shader_window_height = request.value.shader_window_height;
		key.source_width = request.value.source_width;
		key.source_height = request.value.source_height;
		key.texture_instruction_count = request.value.texture_instruction_count;
		key.texture_opcode_class_mask = request.value.texture_opcode_class_mask;
		key.x = request.value.x;
		key.y = request.value.y;
		key.width = request.value.width;
		key.height = request.value.height;
		key.depth = request.value.depth;
		key.bpp = request.value.bpp;
		key.texture_unit = request.value.texture_unit;
		key.source_attachment = request.value.source_attachment;
		key.feedback_alias_count = feedback_alias_count;
		key.coordinate_source_mask = request.value.coordinate_source_mask;
		key.texture_dimension = request.value.texture_dimension;
		key.mipmap_count = request.value.mipmap_count;
		key.sample_count = request.value.sample_count;
		key.shader_window_origin = request.value.shader_window_origin;
		key.pixel_center = request.value.pixel_center;
		key.unnormalized_coordinates = request.value.unnormalized_coordinates;
		key.texcoord_clamp = request.value.texcoord_clamp;
		key.indexed_coordinate_source = request.value.indexed_coordinate_source;
		key.mixed_coordinate_source = request.value.mixed_coordinate_source;
		key.coordinate_expression_known_identity = request.value.coordinate_expression_known_identity;
		key.stage = request.value.stage;
		key.reason = request.value.reason;
		key.outcome = request.value.outcome;

		const u64 logical_request_bytes = request_credit ? request.value.logical_bytes : 0;
		const u64 score = logical_request_bytes ? logical_request_bytes :
			(request_credit || consumer_credit ? 1 : 0);
		if (!score)
		{
			return;
		}

		if (request_credit)
		{
			const u8 route_flags = classify_routes(key.reject_bits);
			if (!key.reject_bits)
			{
				add_request_counter(m_frame_rejects.zero_rejects, request.value);
			}
			else
			{
				for (usz index = 0; index < reject_bit_count; ++index)
				{
					if (key.reject_bits & (1ull << index))
					{
						add_request_counter(m_frame_rejects.by_bit[index], request.value);
					}
				}
			}

			if (route_flags & route_local_read)
			{
				add_request_counter(m_frame_routes.local_read, request.value);
			}
			if (route_flags & route_local_read_potential)
			{
				add_request_counter(m_frame_routes.local_read_potential, request.value);
			}
			if (route_flags & route_ping_pong)
			{
				add_request_counter(m_frame_routes.ping_pong, request.value);
			}
			if (route_flags & route_ping_pong_potential)
			{
				add_request_counter(m_frame_routes.ping_pong_potential, request.value);
			}

			append_copy_map(request.value, key.reject_bits, route_flags, m_draw.frame_id);
		}

		group_entry* matching = nullptr;
		group_entry* empty = nullptr;
		group_entry* smallest = &m_groups.front();
		for (auto& entry : m_groups)
		{
			if (entry.occupied() && entry.key == key)
			{
				matching = &entry;
				break;
			}

			if (!entry.occupied() && !empty)
			{
				empty = &entry;
			}
			else if (entry.occupied() && entry.score_estimate < smallest->score_estimate)
			{
				smallest = &entry;
			}
		}

		auto* entry = matching ? matching : empty;
		if (!entry)
		{
			const u64 inherited_score = smallest->score_estimate;
			*smallest = {};
			entry = smallest;
			entry->score_estimate = inherited_score;
			entry->score_error = inherited_score;
			m_frame.topk_replacements++;
		}

		if (!matching)
		{
			entry->key = key;
		}

		entry->score_estimate += score;
		entry->attributed_requests += request_credit;
		entry->consumer_links += consumer_credit;
		entry->logical_request_bytes += logical_request_bytes;
		if (!entry->first_generation && request.value.source_generation)
		{
			entry->first_generation = request.value.source_generation;
		}
		if (request.value.source_generation)
		{
			entry->last_generation = request.value.source_generation;
		}
		entry->last_request_serial = request.value.request_serial;
		entry->last_snapshot_serial = request.value.snapshot_serial;
		entry->last_copy_serial = request.value.copy_serial;
		entry->last_frame = m_draw.frame_id;
		entry->last_draw = m_draw.draw_id;
		entry->last_command_buffer_identity = request.value.command_buffer_identity;
		entry->last_command_buffer_reset_id = request.value.command_buffer_reset_id;
		if (subdraw)
		{
			entry->last_rsx_subdraw = subdraw->rsx_subdraw;
			entry->last_host_subdraw = subdraw->host_subdraw;
			entry->last_topology = subdraw->topology;
			entry->last_vertex_count = subdraw->vertex_count;
			entry->last_instance_count = subdraw->instance_count;
			entry->last_host_draw_count = subdraw->host_draw_count;
			entry->last_indexed = subdraw->indexed;
		}

		request.attributed |= request_credit;
		m_frame.consumer_links += consumer_credit;
	}

	void oracle::note_subdraw(const subdraw_observation& subdraw)
	{
		if (!m_enabled || !m_draw_active || !m_pending_count)
		{
			return;
		}

		m_draw_had_subdraw = true;
		m_frame.subdraw_groups++;
		for (usz index = 0; index < m_pending_count; ++index)
		{
			observe_group(m_pending[index], &subdraw);
		}
	}

	void oracle::finish_draw()
	{
		if (!m_enabled || !m_draw_active)
		{
			return;
		}

		for (usz index = 0; index < m_pending_count; ++index)
		{
			auto& request = m_pending[index];
			if (!request.attributed)
			{
				observe_group(request, nullptr);
				m_frame.unconsumed_requests++;
			}
		}

		m_draw_active = false;
		m_draw_had_subdraw = false;
		m_pending_count = 0;
	}

	void oracle::note_submit(u64 command_buffer_identity, u64 command_buffer_reset_id)
	{
		if (!m_enabled)
		{
			return;
		}

		const u64 submit_serial = ++m_last_submit_serial;
		if (!m_command_buffer_has_feedback ||
			m_feedback_command_buffer_identity != command_buffer_identity ||
			m_feedback_command_buffer_reset_id != command_buffer_reset_id)
		{
			return;
		}

		m_frame.submits_with_feedback++;
		for (auto& entry : m_groups)
		{
			if (entry.occupied() &&
				entry.last_command_buffer_identity == command_buffer_identity &&
				entry.last_command_buffer_reset_id == command_buffer_reset_id)
			{
				entry.last_submit_serial = submit_serial;
			}
		}

		m_command_buffer_has_feedback = false;
	}

	void oracle::end_frame(const reference_statistics& reference)
	{
		if (!m_enabled)
		{
			return;
		}

		if (m_draw_active)
		{
			finish_draw();
		}

		// Request/logical-byte marginals are classified as each consumer is
		// observed. Physical-copy marginals must instead use the finalized
		// per-copy union: a snapshot is eligible only when every consumer of
		// that exact version is eligible.
		for (usz index = 0; index < m_copy_map_count; ++index)
		{
			const auto& record = m_copy_map[index];
			if (!record.reject_bits)
			{
				add_actual_copy_counter(m_frame_rejects.zero_rejects, record);
			}
			else
			{
				for (usz bit = 0; bit < reject_bit_count; ++bit)
				{
					if (record.reject_bits & (1ull << bit))
					{
						add_actual_copy_counter(m_frame_rejects.by_bit[bit], record);
					}
				}
			}

			if (record.route_flags & route_local_read)
			{
				add_actual_copy_counter(m_frame_routes.local_read, record);
			}
			if (record.route_flags & route_local_read_potential)
			{
				add_actual_copy_counter(m_frame_routes.local_read_potential, record);
			}
			if (record.route_flags & route_ping_pong)
			{
				add_actual_copy_counter(m_frame_routes.ping_pong, record);
			}
			if (record.route_flags & route_ping_pong_potential)
			{
				add_actual_copy_counter(m_frame_routes.ping_pong_potential, record);
			}
		}

		m_frame.frames = 1;
		const u64 expected_copy_map_records = m_frame.new_snapshots + m_frame.refreshes;
		if (m_frame.copy_map_records != expected_copy_map_records ||
			m_frame.copy_map_drops || m_frame.copy_map_missing_serials ||
			m_frame.copy_map_duplicate_serials)
		{
			m_frame.copy_map_mismatches++;
		}
		m_last_reference = reference;
		m_last_frame_reconciled =
			m_frame.requests == reference.requests &&
			m_frame.live_rop_requests == reference.live_rop_requests &&
			m_frame.edge_clamped_requests == reference.edge_clamped_requests &&
			m_frame.new_snapshots + m_frame.refreshes == reference.actual_copies &&
			m_frame.new_snapshots == reference.new_snapshots &&
			m_frame.refreshes == reference.refreshes &&
			m_frame.generation_reuses == reference.generation_reuses &&
			m_frame.static_hits == reference.static_hits &&
			m_frame.failures == reference.failures &&
			m_frame.uncached_copies == reference.uncached_copies &&
			m_frame.logical_copied_bytes == reference.logical_copied_bytes &&
			m_frame.logical_reused_bytes == reference.logical_reused_bytes &&
			m_frame.copy_map_mismatches == 0 &&
			m_frame.unknown_outcomes == 0;

		if (m_last_frame_reconciled)
		{
			m_frame.reconciled_frames++;
		}
		else
		{
			m_frame.reconciliation_mismatches++;
		}

		m_last_completed_frame = m_frame;
		m_last_completed_frame_rejects = m_frame_rejects;
		m_last_completed_frame_routes = m_frame_routes;
		accumulate(m_total, m_frame);
		accumulate(m_total_rejects, m_frame_rejects);
		accumulate(m_total_routes, m_frame_routes);
		report_copy_map();

		const u64 now = get_system_time();
		if (!m_last_report_time_us || now - m_last_report_time_us >= 1'000'000)
		{
			report(now);
		}

		m_frame = {};
		m_frame_rejects = {};
		m_frame_routes = {};
		m_copy_map_count = 0;
		m_copy_map_frame_id = 0;
	}

	void oracle::report_copy_map()
	{
		if (!m_copy_map_count)
		{
			return;
		}

		const usz part_count = (m_copy_map_count + copy_map_records_per_line - 1) /
			copy_map_records_per_line;
		for (usz part = 0; part < part_count; ++part)
		{
			const usz begin = part * copy_map_records_per_line;
			const usz end = std::min(m_copy_map_count, begin + copy_map_records_per_line);
			std::string records;
			for (usz index = begin; index < end; ++index)
			{
				const auto& record = m_copy_map[index];
				records += format_copy_map_record(record, !records.empty());
			}

			rsx_log.notice("FBPATH_MAP frame=%llu part=%u/%u records=%s",
				m_copy_map_frame_id, static_cast<u32>(part + 1),
				static_cast<u32>(part_count), records);
		}
	}

	void oracle::report(u64 now)
	{
		m_last_report_time_us = now;
		rsx_log.notice("FBPATH frames=%llu requests=%llu outcomes=%llu/%llu/%llu/%llu/%llu unknown=%llu copied_bytes=%llu reused_bytes=%llu consumer_links=%llu subdraw_groups=%llu unconsumed=%llu orphan=%llu pending_drops=%llu topk_replacements=%llu reconciled=%llu mismatches=%llu submits=%llu serials=%llu/%llu/%llu submit_serial=%llu map_records=%llu map_drops=%llu map_missing_serials=%llu map_duplicate_serials=%llu map_mismatches=%llu",
			m_total.frames, m_total.requests, m_total.new_snapshots, m_total.refreshes,
			m_total.generation_reuses, m_total.static_hits, m_total.failures,
			m_total.unknown_outcomes, m_total.logical_copied_bytes,
			m_total.logical_reused_bytes, m_total.consumer_links,
			m_total.subdraw_groups, m_total.unconsumed_requests,
			m_total.orphan_requests, m_total.request_record_drops,
			m_total.topk_replacements, m_total.reconciled_frames,
			m_total.reconciliation_mismatches, m_total.submits_with_feedback,
			m_last_request_serial, m_last_snapshot_serial, m_last_copy_serial,
			m_last_submit_serial, m_total.copy_map_records, m_total.copy_map_drops,
			m_total.copy_map_missing_serials, m_total.copy_map_duplicate_serials,
			m_total.copy_map_mismatches);

		rsx_log.notice("FBPATH_GATE zero_requests=%llu zero_bytes=%llu zero_actual_copies=%llu zero_actual_bytes=%llu unpartitioned_requests=%llu unpartitioned_bytes=%llu unpartitioned_actual_copies=%llu unpartitioned_actual_bytes=%llu",
			m_total_rejects.zero_rejects.requests,
			m_total_rejects.zero_rejects.logical_bytes,
			m_total_rejects.zero_rejects.actual_copies,
			m_total_rejects.zero_rejects.actual_copy_bytes,
			m_total_rejects.unpartitioned.requests,
			m_total_rejects.unpartitioned.logical_bytes,
			m_total_rejects.unpartitioned.actual_copies,
			m_total_rejects.unpartitioned.actual_copy_bytes);
		rsx_log.notice("FBPATH_ROUTES local_read=%llu/%llu/%llu/%llu local_read_potential=%llu/%llu/%llu/%llu ping_pong=%llu/%llu/%llu/%llu ping_pong_potential=%llu/%llu/%llu/%llu fields=requests/logical_bytes/actual_copies/actual_copy_bytes",
			m_total_routes.local_read.requests, m_total_routes.local_read.logical_bytes,
			m_total_routes.local_read.actual_copies, m_total_routes.local_read.actual_copy_bytes,
			m_total_routes.local_read_potential.requests, m_total_routes.local_read_potential.logical_bytes,
			m_total_routes.local_read_potential.actual_copies, m_total_routes.local_read_potential.actual_copy_bytes,
			m_total_routes.ping_pong.requests, m_total_routes.ping_pong.logical_bytes,
			m_total_routes.ping_pong.actual_copies, m_total_routes.ping_pong.actual_copy_bytes,
			m_total_routes.ping_pong_potential.requests, m_total_routes.ping_pong_potential.logical_bytes,
			m_total_routes.ping_pong_potential.actual_copies, m_total_routes.ping_pong_potential.actual_copy_bytes);

		std::string reject_counts;
		for (usz index = 0; index < reject_bit_count; ++index)
		{
			const auto& counter = m_total_rejects.by_bit[index];
			if (!counter.requests)
			{
				continue;
			}

			reject_counts += fmt::format("%s%llu:%llu/%llu/%llu/%llu",
				reject_counts.empty() ? "" : ",", static_cast<u64>(index), counter.requests,
				counter.logical_bytes, counter.actual_copies, counter.actual_copy_bytes);
		}
		rsx_log.notice("FBPATH_GATE_BITS bit=requests/logical_bytes/actual_copies/actual_copy_bytes values=%s", reject_counts);

		std::array<const group_entry*, group_capacity> sorted{};
		usz count = 0;
		for (const auto& entry : m_groups)
		{
			if (entry.occupied())
			{
				sorted[count++] = &entry;
			}
		}

		std::sort(sorted.begin(), sorted.begin() + count,
			[](const group_entry* lhs, const group_entry* rhs)
			{
				return lhs->score_estimate > rhs->score_estimate;
			});

		for (usz rank = 0; rank < std::min(count, report_group_count); ++rank)
		{
			const auto& entry = *sorted[rank];
			const auto& key = entry.key;
			rsx_log.notice("FBPATH_GROUP rank=%u reason=%s outcome=%s stage=%s unit=%u attachment=%u aliases=%u source_id=0x%llx source=0x%08x request=0x%08x region=%u,%u,%ux%ux%u source_dims=%ux%u pitch=%u bpp=%u gcm_format=0x%x source_vk=%u view_vk=%u source_map=0x%x view_map=0x%x remap=0x%08x tex_meta=%u/0x%x/0x%x tex_shape=%u/%u/%u src0=%u/%u/%u xform=0x%llx/%08x,%08x,%08x,%08x/clamp=%u:%08x,%08x,%08x,%08x/unnorm=%u wpos=%u/%u/%u/%08x/%08x,%08x coverage=%d,%d,%ux%u/%ux%u fp=0x%llx vp=0x%llx pipeline=0x%llx draw=0x%llx sampler=0x%llx rejects=0x%016llx score_est=%llu score_error=%llu attributed_requests=%llu consumer_links=%llu logical_request_bytes=%llu generation_first=%llu generation_last=%llu request_serial=%llu snapshot_serial=%llu copy_serial=%llu last_frame=%llu last_draw=%llu last_subdraw=%u/%u topology=%u vertices=%u instances=%u host_draws=%u indexed=%u cb=0x%llx/%llu submit=%llu",
				static_cast<u32>(rank + 1), get_reason_name(key.reason),
				get_outcome_name(key.outcome), key.stage == texture_stage::fragment ? "fp" : "vp",
				key.texture_unit, key.source_attachment, key.feedback_alias_count, key.source_identity,
				key.source_address, key.request_address, key.x, key.y,
				key.width, key.height, key.depth, key.source_width, key.source_height,
				key.pitch, key.bpp, key.gcm_format,
				key.source_vk_format, key.view_vk_format, key.source_component_map,
				key.view_component_map, key.remap,
				key.texture_instruction_count, key.texture_opcode_class_mask,
				key.coordinate_source_mask, key.texture_dimension, key.mipmap_count,
				key.sample_count, key.coordinate_expression_known_identity,
				key.indexed_coordinate_source, key.mixed_coordinate_source,
				key.texcoord_xform_signature, key.texcoord_scale_x, key.texcoord_scale_y,
				key.texcoord_bias_x, key.texcoord_bias_y, key.texcoord_clamp,
				key.texcoord_clamp_min_x, key.texcoord_clamp_min_y,
				key.texcoord_clamp_max_x, key.texcoord_clamp_max_y,
				key.unnormalized_coordinates, key.shader_window_origin, key.pixel_center,
				key.shader_window_height, key.resolution_scale, key.wpos_scale,
				key.wpos_bias_x, key.wpos_bias_y,
				key.scissor_x, key.scissor_y, key.scissor_width, key.scissor_height,
				key.target_width, key.target_height,
				key.fp_hash, key.vp_hash, key.pipeline_hash, key.draw_signature,
				key.sampler_signature, key.reject_bits, entry.score_estimate,
				entry.score_error, entry.attributed_requests, entry.consumer_links,
				entry.logical_request_bytes, entry.first_generation,
				entry.last_generation, entry.last_request_serial,
				entry.last_snapshot_serial, entry.last_copy_serial, entry.last_frame,
				entry.last_draw, entry.last_rsx_subdraw, entry.last_host_subdraw,
				entry.last_topology,
				entry.last_vertex_count, entry.last_instance_count,
				entry.last_host_draw_count, entry.last_indexed,
				entry.last_command_buffer_identity, entry.last_command_buffer_reset_id,
				entry.last_submit_serial);
		}
	}

	summary oracle::get_summary() const noexcept
	{
		return
		{
			.frame = m_frame.requests || m_frame.frames ? m_frame : m_last_completed_frame,
			.total = m_total,
			.last_reference = m_last_reference,
			.last_frame_reconciled = m_last_frame_reconciled,
			.last_submit_serial = m_last_submit_serial,
			.last_request_serial = m_last_request_serial,
			.last_snapshot_serial = m_last_snapshot_serial,
			.last_copy_serial = m_last_copy_serial,
			.frame_rejects = m_frame.requests || m_frame.frames ? m_frame_rejects : m_last_completed_frame_rejects,
			.total_rejects = m_total_rejects,
			.frame_routes = m_frame.requests || m_frame.frames ? m_frame_routes : m_last_completed_frame_routes,
			.total_routes = m_total_routes,
		};
	}

	const std::array<group_entry, oracle::group_capacity>& oracle::groups() const noexcept
	{
		return m_groups;
	}

	std::span<const copy_map_record> oracle::copy_map() const noexcept
	{
		return {m_copy_map.data(), m_copy_map_count};
	}
}
