#include "stdafx.h"
#include "cell_access_coherence.h"
#include "Emu/RSX/RSXCoherenceStats.h"
#include "Emu/Cell/timers.hpp"

namespace rsx::cell_access
{
	ownership_directory g_ownership_directory;
	exact_cohort_oracle g_exact_cohort_oracle;
	static std::atomic<u64> g_next_exact_cohort_session{1};

	u64 allocate_exact_cohort_session_generation() noexcept
	{
		u64 result = g_next_exact_cohort_session.fetch_add(1, std::memory_order_relaxed);
		if (!result)
		{
			result = g_next_exact_cohort_session.fetch_add(1, std::memory_order_relaxed);
		}
		return result;
	}

	static exact_cohort_member_observation merge_member_observation(
		const exact_cohort_member_observation& prior,
		const exact_cohort_member_observation& update) noexcept
	{
		auto result = prior;
		if (!result.fault_start_ticks && update.fault_start_ticks)
		{
			result.frame = update.frame;
			result.fault_start_ticks = update.fault_start_ticks;
			result.fault_address = update.fault_address;
			result.origin_id = update.origin_id;
			result.origin_guest_pc = update.origin_guest_pc;
			result.origin = update.origin;
			result.mfc_spu_id = update.mfc_spu_id;
			result.mfc_ea = update.mfc_ea;
			result.mfc_size = update.mfc_size;
			result.mfc_source = update.mfc_source;
			result.mfc_cmd = update.mfc_cmd;
			result.mfc_tag = update.mfc_tag;
			result.mfc_flags = update.mfc_flags;
			result.mfc_valid = update.mfc_valid;
			result.mfc_contains_fault = update.mfc_contains_fault;
		}
		result.frame = update.frame ? update.frame : result.frame;
		result.fault_end_ticks = std::max(result.fault_end_ticks, update.fault_end_ticks);
		result.flush_wait_start_ticks = update.flush_wait_start_ticks
			? update.flush_wait_start_ticks : result.flush_wait_start_ticks;
		result.flush_wait_end_ticks = std::max(result.flush_wait_end_ticks, update.flush_wait_end_ticks);
		result.readback_wait_start_ticks = update.readback_wait_start_ticks
			? update.readback_wait_start_ticks : result.readback_wait_start_ticks;
		result.readback_wait_end_ticks = std::max(result.readback_wait_end_ticks, update.readback_wait_end_ticks);
		result.readback_bytes = std::max(result.readback_bytes, update.readback_bytes);
		result.submission_ready_us = update.submission_ready_us
			? update.submission_ready_us : result.submission_ready_us;
		result.queue_ref_removed_us = update.queue_ref_removed_us
			? update.queue_ref_removed_us : result.queue_ref_removed_us;
		result.data_ready_us = update.data_ready_us ? update.data_ready_us : result.data_ready_us;
		result.unprotect_done_us = update.unprotect_done_us ? update.unprotect_done_us : result.unprotect_done_us;
		result.semantic_ready_us = update.semantic_ready_us ? update.semantic_ready_us : result.semantic_ready_us;
		result.first_staged_completion_generation = update.first_staged_completion_generation
			? update.first_staged_completion_generation : result.first_staged_completion_generation;
		result.flush_wait_count = std::max(result.flush_wait_count, update.flush_wait_count);
		result.readback_wait_count = std::max(result.readback_wait_count, update.readback_wait_count);
		result.readback_count = std::max(result.readback_count, update.readback_count);
		result.transfer_count = std::max(result.transfer_count, update.transfer_count);
		if (update.copied_start != umax)
		{
			result.copied_start = update.copied_start;
			result.copied_end = update.copied_end;
		}
		result.flush_sections = std::max(result.flush_sections, update.flush_sections);
		result.unprotect_sections = std::max(result.unprotect_sections, update.unprotect_sections);
		result.discarded_sections = std::max(result.discarded_sections, update.discarded_sections);
		result.queue_posts = std::max(result.queue_posts, update.queue_posts);
		result.replan_empty |= update.replan_empty;
		result.cache_unchanged |= update.cache_unchanged;
		result.semantic_plan_verified |= update.semantic_plan_verified;
		result.completion_verified |= update.completion_verified;
		return result;
	}

	bool exact_cohort_member_observation::is_spu_get() const noexcept
	{
		return origin == static_cast<u8>(coherence_stats::fault_origin::spu) &&
			mfc_valid && (mfc_flags & coherence_stats::mfc_context_get);
	}

	bool exact_cohort_plan::same_semantic_plan_except_cache_revision(const exact_cohort_plan& other) const noexcept
	{
		if (renderer_epoch != other.renderer_epoch ||
			directory_sequence != other.directory_sequence ||
			section_count != other.section_count)
		{
			return false;
		}

		for (u32 index = 0; index < section_count; index++)
		{
			if (!(sections[index] == other.sections[index]))
			{
				return false;
			}
		}

		return true;
	}

	bool exact_cohort_plan::same_semantic_plan(const exact_cohort_plan& other) const noexcept
	{
		// Per-scan cache revisions change as each legacy fault invalidates the
		// cache. They are retained as drift evidence, not used to split otherwise
		// identical semantic plans.
		return same_semantic_plan_except_cache_revision(other);
	}

	bool exact_cohort_plan::same_native_cohort(const exact_cohort_plan& other) const noexcept
	{
		return fault_range == other.fault_range &&
			invalidate_range == other.invalidate_range &&
			same_semantic_plan(other);
	}

	exact_cohort_ticket exact_cohort_oracle::begin(const exact_cohort_plan& plan,
		const exact_cohort_member_observation& observation) noexcept
	{
		const bool invalid_directory = !plan.directory_sequence || (plan.directory_sequence & 1);
		const bool invalid_range_coverage = plan.fault_range.valid() &&
			plan.invalidate_range.valid() && !plan.fault_range.inside(plan.invalidate_range);
		bool unknown_generation = !plan.renderer_epoch || !plan.cache_revision ||
			!plan.fault_range.valid() || !plan.invalidate_range.valid() ||
			!plan.section_count || plan.section_count > exact_cohort_plan_capacity;
		bool has_flush_exclusions = false;
		for (u32 index = 0; index < plan.section_count && !unknown_generation; index++)
		{
			const auto& section = plan.sections[index];
			has_flush_exclusions |= section.has_flush_exclusions;
			unknown_generation = !section.section_identity ||
				!section.section_session_generation || !section.producer_identity ||
				!section.content_generation || !section.transfer_generation ||
				!section.write_generation ||
				!section.rsx_pitch || !section.gcm_format || !section.width ||
				!section.height || !section.depth || !section.mipmaps ||
				(section.synchronized && !section.synchronization_generation) ||
				(section.synchronized && !section.staged_generation) ||
				!section.full_range.valid() || !section.confirmed_range.valid() ||
				!section.locked_range.valid() || section.execution_rank != index;
		}

		if (invalid_directory || invalid_range_coverage || unknown_generation || has_flush_exclusions)
		{
			std::lock_guard lock(m_mutex);
			m_totals.invalid_directory_sequence += invalid_directory;
			m_totals.invalid_range_rejections += invalid_range_coverage;
			m_totals.unknown_generation_rejections += unknown_generation;
			m_totals.flush_exclusion_rejections += has_flush_exclusions;
			return {};
		}

		const u64 now = get_system_time();
		std::lock_guard lock(m_mutex);

		for (const auto& slot : m_slots)
		{
			if (slot.active && slot.plan.cache_revision != plan.cache_revision &&
				slot.plan.same_semantic_plan_except_cache_revision(plan))
			{
				m_totals.cache_revision_splits++;
				break;
			}
		}

		for (u32 index = 0; index < m_slots.size(); index++)
		{
			auto& slot = m_slots[index];
			if (!slot.active || !slot.plan.same_semantic_plan(plan))
			{
				continue;
			}

			if (slot.registrations == exact_cohort_member_capacity)
			{
				m_totals.member_exhaustion++;
				return {};
			}

			const u16 member = static_cast<u16>(slot.registrations++);
			slot.members[member] = observation;
			slot.member_fault_ranges[member] = plan.fault_range;
			slot.member_invalidate_ranges[member] = plan.invalidate_range;
			if (observation.frame)
			{
				slot.frame_min = slot.frame_min ? std::min(slot.frame_min, observation.frame) : observation.frame;
				slot.frame_max = std::max(slot.frame_max, observation.frame);
			}

			const bool same_native = slot.plan.fault_range == plan.fault_range &&
				slot.plan.invalidate_range == plan.invalidate_range;
			m_totals.same_plan_ranges_followers += same_native;
			m_totals.different_plan_ranges_followers += !same_native;
			for (u32 prior = 0; prior < member; prior++)
			{
				m_totals.cross_page_same_plan_pairs +=
					slot.member_fault_ranges[prior] != plan.fault_range;
			}
			if (plan.fault_range != slot.member_fault_ranges[0])
			{
				m_totals.cross_page_followers_covered_by_leader +=
					plan.fault_range.inside(slot.member_invalidate_ranges[0]);
			}
			m_totals.spu_get_followers += observation.is_spu_get();
			const u64 arrival = now - slot.started_us;
			m_totals.follower_arrival_us += arrival;
			m_totals.follower_arrival_max_us = std::max(m_totals.follower_arrival_max_us, arrival);
			return {slot.serial, static_cast<u16>(index), member};
		}

		for (u32 index = 0; index < m_slots.size(); index++)
		{
			auto& slot = m_slots[index];
			if (slot.occupied)
			{
				continue;
			}

			u64 serial = m_next_serial++;
			if (!serial)
			{
				serial = m_next_serial++;
			}

			slot = {};
			slot.plan = plan;
			slot.serial = serial;
			slot.started_us = now;
			slot.frame_min = observation.frame;
			slot.frame_max = observation.frame;
			slot.registrations = 1;
			slot.members[0] = observation;
			slot.member_fault_ranges[0] = plan.fault_range;
			slot.member_invalidate_ranges[0] = plan.invalidate_range;
			slot.occupied = true;
			slot.active = true;
			m_totals.leaders++;
			m_totals.spu_get_leaders += observation.is_spu_get();
			return {serial, static_cast<u16>(index), 0};
		}

		m_totals.slot_exhaustion++;
		return {};
	}

	void exact_cohort_oracle::finish_timing(slot_state& slot) noexcept
	{
		exact_cohort_completion completion{};
		completion.serial = slot.serial;
		completion.renderer_epoch = slot.plan.renderer_epoch;
		completion.directory_sequence = slot.plan.directory_sequence;
		completion.cache_revision = slot.plan.cache_revision;
		completion.started_us = slot.started_us;
		completion.first_done_us = slot.first_done_us;
		completion.proposed_leader_done_us = slot.proposed_leader_done_us;
		completion.last_member_done_us = slot.last_member_done_us;
		completion.last_terminal_us = slot.last_terminal_us;
		completion.first_readback_data_ready_us = slot.first_readback_data_ready_us;
		completion.last_readback_data_ready_us = slot.last_readback_data_ready_us;
		completion.frame_min = slot.frame_min;
		completion.frame_max = slot.frame_max;
		completion.registrations = slot.registrations;
		completion.successful_members = slot.successful_calls;
		completion.first_done_member = slot.first_done_member;
		completion.proposed_leader_materialized =
			slot.terminals[0] == exact_cohort_terminal_kind::success;
		completion.proposed_leader_covers_all_faults = true;
		completion.materializer_covers_all_faults = slot.first_done_member < slot.registrations;
		if (slot.first_done_member < slot.registrations)
		{
			completion.materializer_data_ready_us =
				slot.members[slot.first_done_member].data_ready_us;
			completion.materializer_semantic_ready_us =
				slot.members[slot.first_done_member].semantic_ready_us;
			completion.materializer_fault_range =
				slot.member_fault_ranges[slot.first_done_member];
			completion.materializer_invalidate_range =
				slot.member_invalidate_ranges[slot.first_done_member];
		}
		for (u32 member = 0; member < slot.registrations; member++)
		{
			completion.cross_page_members +=
				slot.member_fault_ranges[member] != slot.member_fault_ranges[0];
			completion.proposed_leader_covers_all_faults &=
				slot.member_fault_ranges[member].inside(slot.member_invalidate_ranges[0]);
			completion.materializer_covers_all_faults &=
				slot.member_fault_ranges[member].inside(completion.materializer_invalidate_range);
		}
		completion.proposed_leader_valid = completion.proposed_leader_materialized &&
			completion.proposed_leader_covers_all_faults;

		completion.resolved_noop_members = slot.resolved_noop_calls;
		completion.abandoned_members = slot.abandoned_calls;
		completion.fault_range = slot.plan.fault_range;
		completion.invalidate_range = slot.plan.invalidate_range;
		completion.section_count = slot.plan.section_count;
		completion.first_section_identity = slot.plan.sections[0].section_identity;
		completion.first_section_session_generation = slot.plan.sections[0].section_session_generation;
		completion.first_producer_identity = slot.plan.sections[0].producer_identity;
		completion.first_content_generation = slot.plan.sections[0].content_generation;
		completion.first_done_had_readback = slot.first_done_had_readback;
		completion.proposed_leader_had_readback = slot.proposed_leader_had_readback;
		completion.any_readback = slot.any_readback;
		completion.complete = slot.abandoned_calls == 0 && slot.successful_calls == 1 &&
			slot.first_done_us && completion.materializer_covers_all_faults &&
			slot.successful_calls + slot.resolved_noop_calls == slot.registrations &&
			slot.terminal_calls == slot.registrations;
		m_totals.multiple_materializers += slot.successful_calls > 1;

		bool every_posted_ref_released = true;
		bool materializer_posted_ref = false;
		bool proposed_leader_posted_ref = false;
		bool queue_instrumentation_valid = true;
		for (u32 member = 0; member < slot.registrations; member++)
		{
			const auto& observation = slot.members[member];
			completion.queue_posts += observation.queue_posts;
			completion.follower_queue_posts += member != 0 ? observation.queue_posts : 0;
			completion.spu_get_members += observation.is_spu_get();
			m_totals.spu_get_queue_posts += observation.is_spu_get() ? observation.queue_posts : 0;
			m_totals.spu_get_follower_queue_posts += member && observation.is_spu_get()
				? observation.queue_posts : 0;
			if (!observation.queue_posts && observation.queue_ref_removed_us)
			{
				m_totals.queue_timestamp_without_post++;
				queue_instrumentation_valid = false;
			}
			every_posted_ref_released &= !observation.queue_posts || observation.queue_ref_removed_us;
			if (member == 0 && observation.queue_posts)
			{
				proposed_leader_posted_ref = observation.queue_posts;
				completion.proposed_leader_queue_release_us = observation.queue_ref_removed_us;
			}
			if (member == completion.first_done_member && observation.queue_posts)
			{
				materializer_posted_ref = observation.queue_posts;
				completion.materializer_queue_release_us = observation.queue_ref_removed_us;
			}
			if (observation.queue_posts)
			{
				completion.last_queue_release_us = std::max(
					completion.last_queue_release_us, observation.queue_ref_removed_us);
			}
		}

		if (completion.complete && completion.proposed_leader_valid &&
			completion.last_member_done_us > completion.proposed_leader_done_us)
		{
			completion.projected_tail_us = completion.last_member_done_us - completion.proposed_leader_done_us;
		}
		if (completion.complete && completion.last_member_done_us > completion.first_done_us)
		{
			completion.materializer_tail_us = completion.last_member_done_us - completion.first_done_us;
		}
		if (completion.complete && queue_instrumentation_valid && materializer_posted_ref && every_posted_ref_released &&
			completion.materializer_queue_release_us &&
			completion.last_queue_release_us > completion.materializer_queue_release_us)
		{
			completion.queue_tail_us = completion.last_queue_release_us - completion.materializer_queue_release_us;
		}
		if (completion.complete && queue_instrumentation_valid && completion.proposed_leader_valid &&
			proposed_leader_posted_ref && every_posted_ref_released &&
			completion.proposed_leader_queue_release_us &&
			completion.last_queue_release_us > completion.proposed_leader_queue_release_us)
		{
			completion.proposed_leader_queue_tail_us = completion.last_queue_release_us - completion.proposed_leader_queue_release_us;
		}

		m_totals.timing_complete += completion.complete;
		m_totals.timing_with_any_readback += completion.complete && completion.any_readback;
		m_totals.timing_with_leader_readback += completion.complete && completion.proposed_leader_had_readback;
		m_totals.timing_cross_page_complete += completion.complete && completion.cross_page_members;
		m_totals.projected_tail_us += completion.projected_tail_us;
		m_totals.projected_tail_max_us = std::max(m_totals.projected_tail_max_us, completion.projected_tail_us);
		m_totals.queue_tail_us += completion.queue_tail_us;
		m_totals.queue_tail_max_us = std::max(m_totals.queue_tail_max_us, completion.queue_tail_us);
		m_totals.incomplete_queue_timing += completion.complete && completion.queue_posts &&
			(!queue_instrumentation_valid || !materializer_posted_ref || !every_posted_ref_released);
		m_totals.homogeneous_spu_get_cohorts += completion.spu_get_members == completion.registrations;
		m_totals.mixed_spu_get_cohorts += completion.spu_get_members != 0 &&
			completion.spu_get_members != completion.registrations;

		if (m_completion_count == m_completions.size())
		{
			m_totals.completion_record_drops++;
			return;
		}

		m_completions[m_completion_write] = completion;
		m_completion_write = (m_completion_write + 1) % m_completions.size();
		m_completion_count++;
	}

	void exact_cohort_oracle::push_member_completion(
		const exact_cohort_member_completion& completion) noexcept
	{
		if (m_member_completion_count == m_member_completions.size())
		{
			m_totals.member_record_drops++;
			return;
		}

		m_member_completions[m_member_completion_write] = completion;
		m_member_completion_write = (m_member_completion_write + 1) % m_member_completions.size();
		m_member_completion_count++;
	}

	bool exact_cohort_oracle::resolve_semantic(exact_cohort_ticket ticket,
		exact_cohort_terminal_kind terminal_kind,
		const exact_cohort_member_observation& observation) noexcept
	{
		if (!ticket.valid())
		{
			return false;
		}

		const u64 now = get_system_time();
		std::lock_guard lock(m_mutex);
		auto& slot = m_slots[ticket.slot];
		if (!slot.occupied || slot.serial != ticket.serial)
		{
			m_totals.stale_completion++;
			return false;
		}
		if (ticket.member >= slot.registrations ||
			(slot.resolved_mask & (1ull << ticket.member)))
		{
			m_totals.stale_completion++;
			return false;
		}

		slot.resolved_mask |= 1ull << ticket.member;
		slot.members[ticket.member] = merge_member_observation(slot.members[ticket.member], observation);
		const auto& resolved_observation = slot.members[ticket.member];
		const bool success_requested = terminal_kind == exact_cohort_terminal_kind::success;
		const bool has_coverage = resolved_observation.flush_sections &&
			resolved_observation.readback_count == resolved_observation.flush_sections &&
			resolved_observation.readback_bytes;
		const bool success = success_requested && resolved_observation.semantic_plan_verified &&
			resolved_observation.completion_verified && has_coverage &&
			resolved_observation.data_ready_us && resolved_observation.unprotect_done_us &&
			resolved_observation.semantic_ready_us >= resolved_observation.unprotect_done_us &&
			resolved_observation.unprotect_done_us >= resolved_observation.data_ready_us;
		const bool noop_requested = terminal_kind == exact_cohort_terminal_kind::resolved_noop;
		const bool no_transfer = !resolved_observation.readback_count &&
			!resolved_observation.transfer_count && !resolved_observation.readback_bytes &&
			!resolved_observation.readback_wait_count &&
			!resolved_observation.readback_wait_start_ticks &&
			!resolved_observation.readback_wait_end_ticks;
		const u64 materializer_ready_us = slot.first_done_member < slot.registrations
			? slot.members[slot.first_done_member].semantic_ready_us : 0;
		const bool resolved_noop = noop_requested && slot.first_done_us && no_transfer &&
			resolved_observation.semantic_plan_verified && resolved_observation.replan_empty &&
			resolved_observation.cache_unchanged &&
			!resolved_observation.flush_sections && !resolved_observation.unprotect_sections &&
			!resolved_observation.discarded_sections && materializer_ready_us &&
			resolved_observation.semantic_ready_us >= materializer_ready_us;
		if ((success_requested && !success) || (noop_requested && !resolved_noop))
		{
			terminal_kind = exact_cohort_terminal_kind::replan_mismatch;
		}
		slot.terminals[ticket.member] = terminal_kind;
		const bool had_readback = resolved_observation.readback_count != 0;
		const bool first_success = success && !slot.first_done_us;
		if (slot.first_done_us)
		{
			slot.after_first_success_mask |= 1ull << ticket.member;
		}
		if (slot.proposed_leader_done_us)
		{
			slot.after_proposed_leader_mask |= 1ull << ticket.member;
		}

		if (success)
		{
			slot.successful_calls++;

			if (ticket.member == 0)
			{
				slot.proposed_leader_done_us = now;
				slot.proposed_leader_had_readback = had_readback;
				m_totals.leader_completion_with_readback += had_readback;
				m_totals.leader_completion_without_readback += !had_readback;
			}

			if (first_success)
			{
				slot.active = false;
				slot.first_done_us = now;
				slot.first_done_member = ticket.member;
				slot.first_done_had_readback = had_readback;
				m_totals.first_completion_with_readback += had_readback;
				m_totals.first_completion_without_readback += !had_readback;
				m_totals.materializers_published++;
				const u64 lifetime = now - slot.started_us;
				m_totals.materializer_publish_latency_us += lifetime;
				m_totals.materializer_publish_latency_max_us = std::max(m_totals.materializer_publish_latency_max_us, lifetime);
				const u64 followers = slot.registrations - 1;
				m_totals.followers_at_materializer_publish += followers;
				m_totals.max_followers_at_materializer_publish = std::max(m_totals.max_followers_at_materializer_publish, followers);
			}
			else
			{
				m_totals.late_completion_with_readback += had_readback;
				m_totals.late_completion_without_readback += !had_readback;
			}
		}
		else if (resolved_noop)
		{
			slot.resolved_noop_calls++;
			m_totals.resolved_noops++;
		}
		else
		{
			slot.abandoned_calls++;
			m_totals.abandoned++;
			m_totals.replan_mismatches += terminal_kind == exact_cohort_terminal_kind::replan_mismatch;
			m_totals.offloader_abandons += terminal_kind == exact_cohort_terminal_kind::offloader;
			slot.active = false;
		}

		slot.resolved_calls++;
		if (slot.resolved_calls == slot.registrations && !slot.first_done_us)
		{
			slot.active = false;
		}
		return true;
	}

	void exact_cohort_oracle::finalize_member(exact_cohort_ticket ticket,
		const exact_cohort_member_observation& observation) noexcept
	{
		if (!ticket.valid())
		{
			return;
		}

		const u64 now = get_system_time();
		std::lock_guard lock(m_mutex);
		auto& slot = m_slots[ticket.slot];
		if (!slot.occupied || slot.serial != ticket.serial ||
			ticket.member >= slot.registrations ||
			!(slot.resolved_mask & (1ull << ticket.member)) ||
			(slot.terminal_mask & (1ull << ticket.member)))
		{
			m_totals.stale_completion++;
			return;
		}

		slot.terminal_mask |= 1ull << ticket.member;
		slot.members[ticket.member] = merge_member_observation(slot.members[ticket.member], observation);
		const auto& final_observation = slot.members[ticket.member];
		const auto terminal_kind = slot.terminals[ticket.member];
		const bool had_readback = final_observation.readback_count != 0;
		if (had_readback)
		{
			slot.any_readback = true;
			if (final_observation.data_ready_us)
			{
				slot.first_readback_data_ready_us = slot.first_readback_data_ready_us
					? std::min(slot.first_readback_data_ready_us, final_observation.data_ready_us)
					: final_observation.data_ready_us;
				slot.last_readback_data_ready_us = std::max(
					slot.last_readback_data_ready_us, final_observation.data_ready_us);
			}
		}
		if (terminal_kind == exact_cohort_terminal_kind::success ||
			terminal_kind == exact_cohort_terminal_kind::resolved_noop)
		{
			slot.last_member_done_us = std::max(slot.last_member_done_us, now);
		}

		slot.last_terminal_us = std::max(slot.last_terminal_us, now);
		if (final_observation.frame)
		{
			slot.frame_min = slot.frame_min ? std::min(slot.frame_min, final_observation.frame) : final_observation.frame;
			slot.frame_max = std::max(slot.frame_max, final_observation.frame);
		}

		slot.terminal_calls++;
		push_member_completion({
			.serial = slot.serial,
			.done_us = now,
			.member = ticket.member,
			.terminal = terminal_kind,
			.first_successful_completion = slot.first_done_member == ticket.member,
			.after_first_successful_completion = !!(slot.after_first_success_mask & (1ull << ticket.member)),
			.after_proposed_leader_completion = !!(slot.after_proposed_leader_mask & (1ull << ticket.member)),
			.fault_range = slot.member_fault_ranges[ticket.member],
			.invalidate_range = slot.member_invalidate_ranges[ticket.member],
			.observation = final_observation,
		});

		if (slot.terminal_calls == slot.registrations)
		{
			// A cohort with no successful exact-plan completion cannot accept a
			// future member after all currently registered members have returned.
			slot.active = false;
			finish_timing(slot);
			slot.occupied = false;
		}
		else if (slot.terminal_calls > slot.registrations)
		{
			m_totals.stale_completion++;
		}
	}

	void exact_cohort_oracle::complete(exact_cohort_ticket ticket,
		const exact_cohort_member_observation& observation) noexcept
	{
		if (resolve_semantic(ticket, exact_cohort_terminal_kind::success, observation))
		{
			finalize_member(ticket, observation);
		}
	}

	void exact_cohort_oracle::resolve_noop(exact_cohort_ticket ticket,
		const exact_cohort_member_observation& observation) noexcept
	{
		if (resolve_semantic(ticket, exact_cohort_terminal_kind::resolved_noop, observation))
		{
			finalize_member(ticket, observation);
		}
	}

	void exact_cohort_oracle::abandon(exact_cohort_ticket ticket,
		exact_cohort_terminal_kind reason,
		const exact_cohort_member_observation& observation) noexcept
	{
		ensure(reason != exact_cohort_terminal_kind::success &&
			reason != exact_cohort_terminal_kind::resolved_noop);
		if (resolve_semantic(ticket, reason, observation))
		{
			finalize_member(ticket, observation);
		}
	}

	void exact_cohort_oracle::record_plan_overflow() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_totals.plan_overflow++;
	}

	void exact_cohort_oracle::record_preplan_mutation() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_totals.preplan_mutation_rejections++;
	}

	void exact_cohort_oracle::record_unsupported_backend() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_totals.unsupported_backend++;
	}

	exact_cohort_snapshot exact_cohort_oracle::snapshot() const noexcept
	{
		std::lock_guard lock(m_mutex);
		auto result = m_totals;
		for (const auto& slot : m_slots)
		{
			result.active_slots += slot.active;
			result.occupied_slots += slot.occupied;
		}
		return result;
	}

	bool exact_cohort_oracle::matches_ticket_plan(exact_cohort_ticket ticket,
		const exact_cohort_plan& plan) const noexcept
	{
		if (!ticket.valid())
		{
			return false;
		}

		std::lock_guard lock(m_mutex);
		const auto& slot = m_slots[ticket.slot];
		return slot.occupied && slot.serial == ticket.serial &&
			ticket.member < slot.registrations && slot.plan.same_semantic_plan(plan) &&
			slot.member_fault_ranges[ticket.member] == plan.fault_range &&
			slot.member_invalidate_ranges[ticket.member] == plan.invalidate_range;
	}

	bool exact_cohort_oracle::try_pop_completion(exact_cohort_completion& result) noexcept
	{
		std::lock_guard lock(m_mutex);
		if (!m_completion_count)
		{
			return false;
		}

		result = m_completions[m_completion_read];
		m_completion_read = (m_completion_read + 1) % m_completions.size();
		m_completion_count--;
		return true;
	}

	bool exact_cohort_oracle::try_pop_member_completion(
		exact_cohort_member_completion& result) noexcept
	{
		std::lock_guard lock(m_mutex);
		if (!m_member_completion_count)
		{
			return false;
		}

		result = m_member_completions[m_member_completion_read];
		m_member_completion_read = (m_member_completion_read + 1) % m_member_completions.size();
		m_member_completion_count--;
		return true;
	}

	void exact_cohort_oracle::reset() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_slots = {};
		m_completions = {};
		m_member_completions = {};
		m_completion_read = 0;
		m_completion_write = 0;
		m_completion_count = 0;
		m_member_completion_read = 0;
		m_member_completion_write = 0;
		m_member_completion_count = 0;
		// Preserve the monotonic serial so an old ticket cannot alias a slot
		// allocated after a diagnostic reset.
		m_totals = {};
	}

	ownership_directory::mutation::mutation(ownership_directory& owner,
		const utils::address_range32& old_range, bool old_no_access, bool nontexture) noexcept
		: m_owner(&owner)
		, m_lock(owner.m_writer_mutex)
		, m_old_range(old_range)
		, m_old_no_access(old_no_access)
		, m_nontexture(nontexture)
	{
		const u64 previous = m_owner->m_sequence.fetch_add(1, std::memory_order_acq_rel);
		if (previous & 1)
		{
			m_owner->m_globally_poisoned.store(true, std::memory_order_relaxed);
			m_owner->m_sequence_errors.fetch_add(1, std::memory_order_relaxed);
		}
	}

	ownership_directory::mutation::~mutation()
	{
		if (m_owner && !m_committed)
		{
			// An interrupted logical transition cannot be reconstructed safely.
			// Poisoning converts every future probe into the existing fallback.
			m_owner->m_globally_poisoned.store(true, std::memory_order_relaxed);
			m_owner->m_abandoned_mutations.fetch_add(1, std::memory_order_relaxed);
			m_owner->finish_mutation();
		}
	}

	void ownership_directory::mutation::commit(const utils::address_range32& new_range, bool new_no_access) noexcept
	{
		ensure(m_owner && !m_committed);
		m_owner->apply_transition(m_old_range, m_old_no_access, new_range, new_no_access, m_nontexture);
		m_owner->finish_mutation();
		m_committed = true;
		m_lock.unlock();
	}

	ownership_directory::stable_session::stable_session(ownership_directory& owner) noexcept
		: m_owner(&owner)
		, m_lock(owner.m_writer_mutex)
	{
	}

	stable_ownership_snapshot ownership_directory::stable_session::probe(u32 address, u32 size) const noexcept
	{
		ensure(m_owner);
		return m_owner->probe_locked(address, size);
	}

	u64 ownership_directory::stable_session::sequence() const noexcept
	{
		ensure(m_owner);
		return m_owner->m_sequence.load(std::memory_order_relaxed);
	}

	ownership_directory::recount_session::recount_session(ownership_directory& owner) noexcept
		: m_owner(&owner)
		, m_lock(owner.m_writer_mutex)
	{
		std::fill(m_owner->m_recount_scratch.begin(), m_owner->m_recount_scratch.end(), 0);
	}

	void ownership_directory::recount_session::add(const utils::address_range32& range) noexcept
	{
		ensure(m_owner && !m_finished && range.valid());
		m_sections++;
		const auto [first, last] = ownership_directory::granule_span(range);
		for (u32 granule = first; granule <= last; granule++)
		{
			m_expected_refs++;
			auto& count = m_owner->m_recount_scratch[granule];
			if (count < poisoned_owner_count - 1)
			{
				count++;
			}
			else
			{
				m_expected_overflow = true;
			}
		}
	}

	directory_recount_result ownership_directory::recount_session::finish() noexcept
	{
		ensure(m_owner && !m_finished);
		auto result = m_owner->finish_recount_locked(m_sections, m_expected_refs, m_expected_overflow);
		m_finished = true;
		m_lock.unlock();
		return result;
	}

	ownership_directory::mutation ownership_directory::begin_mutation(
		const utils::address_range32& old_range, bool old_no_access) noexcept
	{
		return mutation(*this, old_range, old_no_access, false);
	}

	ownership_directory::mutation ownership_directory::begin_nontexture_mutation(
		const utils::address_range32& old_range, bool old_no_access) noexcept
	{
		return mutation(*this, old_range, old_no_access, true);
	}

	ownership_directory::recount_session ownership_directory::begin_recount() noexcept
	{
		return recount_session(*this);
	}

	ownership_directory::stable_session ownership_directory::begin_stable_session() noexcept
	{
		return stable_session(*this);
	}

	std::pair<u32, u32> ownership_directory::granule_span(const utils::address_range32& range) noexcept
	{
		ensure(range.valid());
		return {range.start >> summary_granule_shift, range.end >> summary_granule_shift};
	}

	void ownership_directory::add_owner(u32 granule, bool nontexture) noexcept
	{
		auto& counter = nontexture
			? m_nontexture_no_access_owner_counts[granule]
			: m_no_access_owner_counts[granule];
		const u16 previous = counter.load(std::memory_order_relaxed);
		if (previous >= poisoned_owner_count - 1)
		{
			counter.store(poisoned_owner_count, std::memory_order_relaxed);
			m_overflows.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		counter.store(previous + 1, std::memory_order_relaxed);
	}

	void ownership_directory::remove_owner(u32 granule, bool nontexture) noexcept
	{
		auto& counter = nontexture
			? m_nontexture_no_access_owner_counts[granule]
			: m_no_access_owner_counts[granule];
		const u16 previous = counter.load(std::memory_order_relaxed);
		if (previous == poisoned_owner_count)
		{
			return;
		}

		if (!previous)
		{
			counter.store(poisoned_owner_count, std::memory_order_relaxed);
			m_underflows.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		counter.store(previous - 1, std::memory_order_relaxed);
	}

	void ownership_directory::apply_transition(const utils::address_range32& old_range, bool old_no_access,
		const utils::address_range32& new_range, bool new_no_access, bool nontexture) noexcept
	{
		u32 old_first = 1;
		u32 old_last = 0;
		u32 new_first = 1;
		u32 new_last = 0;

		if (old_no_access)
		{
			const auto span = granule_span(old_range);
			old_first = span.first;
			old_last = span.second;
		}

		if (new_no_access)
		{
			const auto span = granule_span(new_range);
			new_first = span.first;
			new_last = span.second;
			for (u32 granule = new_first; granule <= new_last; granule++)
			{
				if (!old_no_access || granule < old_first || granule > old_last)
				{
					add_owner(granule, nontexture);
				}
			}
		}

		if (old_no_access)
		{
			for (u32 granule = old_first; granule <= old_last; granule++)
			{
				if (!new_no_access || granule < new_first || granule > new_last)
				{
					remove_owner(granule, nontexture);
				}
			}
		}
	}

	bool ownership_directory::handoff_nontexture_to_texture(const utils::address_range32& nontexture_range,
		const utils::address_range32& exact_texture_range) noexcept
	{
		ensure(nontexture_range.valid());
		std::unique_lock lock(m_writer_mutex);
		const u64 previous = m_sequence.fetch_add(1, std::memory_order_acq_rel);
		if (previous & 1)
		{
			m_globally_poisoned.store(true, std::memory_order_relaxed);
			m_sequence_errors.fetch_add(1, std::memory_order_relaxed);
		}

		const auto [first, last] = granule_span(nontexture_range);
		bool covered = exact_texture_range.valid() && nontexture_range.inside(exact_texture_range);
		for (u32 granule = first; granule <= last; granule++)
		{
			const u16 texture = m_no_access_owner_counts[granule].load(std::memory_order_relaxed);
			const u16 external = m_nontexture_no_access_owner_counts[granule].load(std::memory_order_relaxed);
			if (!covered || !texture || texture == poisoned_owner_count || !external || external == poisoned_owner_count)
			{
				covered = false;
				break;
			}
		}

		if (covered)
		{
			for (u32 granule = first; granule <= last; granule++)
			{
				remove_owner(granule, true);
			}
		}
		else
		{
			// The physical protection has no complete logical successor. Retain the
			// conservative external owner and force all future behavioral users to fall back.
			m_globally_poisoned.store(true, std::memory_order_relaxed);
		}

		finish_mutation();
		return covered;
	}

	u64 ownership_directory::begin_renderer_lifetime_quiescent() noexcept
	{
		std::unique_lock lock(m_writer_mutex);
		const u64 previous = m_sequence.fetch_add(1, std::memory_order_acq_rel);
		if (previous & 1)
		{
			m_sequence_errors.fetch_add(1, std::memory_order_relaxed);
		}

		for (auto& count : m_no_access_owner_counts)
		{
			count.store(0, std::memory_order_relaxed);
		}
		for (auto& count : m_nontexture_no_access_owner_counts)
		{
			count.store(0, std::memory_order_relaxed);
		}

		m_globally_poisoned.store(false, std::memory_order_relaxed);
		u64 epoch = m_lifetime_epoch.load(std::memory_order_relaxed) + 1;
		if (!epoch)
		{
			// Lifetime-token reuse is not recoverable. Keep the epoch invalid and
			// permanently force future behavioral users onto the existing path.
			m_globally_poisoned.store(true, std::memory_order_relaxed);
			epoch = umax;
		}
		// Publish the cleared planes before allowing an out-of-lock epoch token
		// observer to accept the new renderer lifetime.
		m_lifetime_epoch.store(epoch, std::memory_order_release);
		reset_validation();
		finish_mutation();
		return epoch;
	}

	stable_ownership_snapshot ownership_directory::probe_locked(u32 address, u32 size) const noexcept
	{
		stable_ownership_snapshot result{};
		result.lifetime_epoch = m_lifetime_epoch.load(std::memory_order_relaxed);
		if (!size || size > summary_granule_size)
		{
			return result;
		}

		const u64 end = static_cast<u64>(address) + size - 1;
		if (end > u32{umax})
		{
			return result;
		}

		if (m_globally_poisoned.load(std::memory_order_relaxed))
		{
			result.texture = page_probe_result::poisoned;
			return result;
		}

		result.texture = page_probe_result::clear;
		const u32 first = address >> summary_granule_shift;
		const u32 last = static_cast<u32>(end) >> summary_granule_shift;
		for (u32 granule = first; granule <= last; granule++)
		{
			const u16 texture = m_no_access_owner_counts[granule].load(std::memory_order_relaxed);
			const u16 external = m_nontexture_no_access_owner_counts[granule].load(std::memory_order_relaxed);
			if (texture == poisoned_owner_count || external == poisoned_owner_count)
			{
				result.texture = page_probe_result::poisoned;
				result.maybe_nontexture = true;
				return result;
			}

			result.texture = texture ? page_probe_result::maybe_texture : result.texture;
			result.maybe_nontexture |= external != 0;
		}

		return result;
	}

	void ownership_directory::finish_mutation() noexcept
	{
		const u64 previous = m_sequence.fetch_add(1, std::memory_order_release);
		if (!(previous & 1))
		{
			m_globally_poisoned.store(true, std::memory_order_relaxed);
			m_sequence_errors.fetch_add(1, std::memory_order_relaxed);
		}
	}

	page_probe_result ownership_directory::probe(u32 address, u32 size) const noexcept
	{
		if (!size || size > summary_granule_size)
		{
			return page_probe_result::invalid;
		}

		const u64 end = static_cast<u64>(address) + size - 1;
		if (end > u32{umax})
		{
			return page_probe_result::invalid;
		}

		const u64 sequence_before = m_sequence.load(std::memory_order_acquire);
		if (sequence_before & 1)
		{
			return page_probe_result::unstable;
		}

		if (m_globally_poisoned.load(std::memory_order_relaxed))
		{
			return page_probe_result::poisoned;
		}

		const u32 first = address >> summary_granule_shift;
		const u32 last = static_cast<u32>(end) >> summary_granule_shift;
		page_probe_result result = page_probe_result::clear;
		for (u32 granule = first; granule <= last; granule++)
		{
			const u16 count = m_no_access_owner_counts[granule].load(std::memory_order_relaxed);
			if (count == poisoned_owner_count)
			{
				result = page_probe_result::poisoned;
				break;
			}

			if (count)
			{
				result = page_probe_result::maybe_texture;
			}
		}

		const u64 sequence_after = m_sequence.load(std::memory_order_acquire);
		if (sequence_before != sequence_after || (sequence_after & 1))
		{
			return page_probe_result::unstable;
		}

		if (m_globally_poisoned.load(std::memory_order_relaxed))
		{
			return page_probe_result::poisoned;
		}

		return result;
	}

	void ownership_directory::record_read_fault_probe(page_probe_result probe_result, bool texture_handled) noexcept
	{
		m_read_fault_probes.fetch_add(1, std::memory_order_relaxed);

		if (!texture_handled)
		{
			if (probe_result == page_probe_result::maybe_texture)
			{
				m_unhandled_maybe_texture.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}

		m_read_faults_handled.fetch_add(1, std::memory_order_relaxed);
		switch (probe_result)
		{
		case page_probe_result::maybe_texture:
			m_handled_maybe_texture.fetch_add(1, std::memory_order_relaxed);
			break;
		case page_probe_result::clear:
			m_handled_clear.fetch_add(1, std::memory_order_relaxed);
			break;
		case page_probe_result::unstable:
		case page_probe_result::invalid:
		case page_probe_result::poisoned:
			m_handled_inconclusive.fetch_add(1, std::memory_order_relaxed);
			break;
		}
	}

	void ownership_directory::record_recount_cache_busy() noexcept
	{
		m_recount_cache_busy.fetch_add(1, std::memory_order_relaxed);
	}

	void ownership_directory::record_recount_duration(u64 duration_us) noexcept
	{
		m_recount_total_us.fetch_add(duration_us, std::memory_order_relaxed);
		u64 previous = m_recount_max_us.load(std::memory_order_relaxed);
		while (previous < duration_us &&
			!m_recount_max_us.compare_exchange_weak(previous, duration_us, std::memory_order_relaxed))
		{
		}
	}

	directory_recount_result ownership_directory::finish_recount_locked(
		u64 sections, u64 expected_refs, bool expected_overflow) noexcept
	{
		directory_recount_result result{};
		result.sequence = m_sequence.load(std::memory_order_acquire);
		result.sections = sections;
		result.expected_refs = expected_refs;
		result.expected_overflow = expected_overflow;

		for (usz index = 0; index < m_recount_scratch.size(); index++)
		{
			const u16 expected = m_recount_scratch[index];
			const u16 observed = m_no_access_owner_counts[index].load(std::memory_order_relaxed);
			result.expected_granules += expected != 0;
			result.observed_granules += observed != 0;

			if (observed == poisoned_owner_count)
			{
				result.poisoned_granules++;
				result.mismatched_granules++;
				continue;
			}

			result.observed_refs += observed;
			if (expected != observed)
			{
				result.mismatched_granules++;
				if (expected > observed)
				{
					result.missing_refs += expected - observed;
				}
				else
				{
					result.excess_refs += observed - expected;
				}
			}
		}

		result.globally_poisoned = m_globally_poisoned.load(std::memory_order_relaxed);
		m_recounts.fetch_add(1, std::memory_order_relaxed);
		if (result.mismatched_granules || expected_overflow)
		{
			m_recounts_with_mismatch.fetch_add(1, std::memory_order_relaxed);
			m_globally_poisoned.store(true, std::memory_order_relaxed);
			result.globally_poisoned = true;
		}

		m_last_recount_sequence.store(result.sequence, std::memory_order_relaxed);
		m_last_recount_sections.store(result.sections, std::memory_order_relaxed);
		m_last_recount_expected_refs.store(result.expected_refs, std::memory_order_relaxed);
		m_last_recount_observed_refs.store(result.observed_refs, std::memory_order_relaxed);
		m_last_recount_missing_refs.store(result.missing_refs, std::memory_order_relaxed);
		m_last_recount_excess_refs.store(result.excess_refs, std::memory_order_relaxed);
		m_last_recount_expected_granules.store(result.expected_granules, std::memory_order_relaxed);
		m_last_recount_observed_granules.store(result.observed_granules, std::memory_order_relaxed);
		m_last_recount_mismatched_granules.store(result.mismatched_granules, std::memory_order_relaxed);
		m_last_recount_poisoned_granules.store(result.poisoned_granules, std::memory_order_relaxed);
		m_last_recount_expected_overflow.store(result.expected_overflow, std::memory_order_relaxed);
		return result;
	}

	directory_validation_snapshot ownership_directory::validation_snapshot() const noexcept
	{
		return
		{
			m_read_fault_probes.load(std::memory_order_relaxed),
			m_read_faults_handled.load(std::memory_order_relaxed),
			m_handled_maybe_texture.load(std::memory_order_relaxed),
			m_handled_clear.load(std::memory_order_relaxed),
			m_handled_inconclusive.load(std::memory_order_relaxed),
			m_unhandled_maybe_texture.load(std::memory_order_relaxed),
			m_recounts.load(std::memory_order_relaxed),
			m_recounts_with_mismatch.load(std::memory_order_relaxed),
			m_recount_cache_busy.load(std::memory_order_relaxed),
			m_recount_total_us.load(std::memory_order_relaxed),
			m_recount_max_us.load(std::memory_order_relaxed),
			m_underflows.load(std::memory_order_relaxed),
			m_overflows.load(std::memory_order_relaxed),
			m_abandoned_mutations.load(std::memory_order_relaxed),
			m_sequence_errors.load(std::memory_order_relaxed),
			m_globally_poisoned.load(std::memory_order_relaxed),
			{
				m_last_recount_sequence.load(std::memory_order_relaxed),
				m_last_recount_sections.load(std::memory_order_relaxed),
				m_last_recount_expected_refs.load(std::memory_order_relaxed),
				m_last_recount_observed_refs.load(std::memory_order_relaxed),
				m_last_recount_missing_refs.load(std::memory_order_relaxed),
				m_last_recount_excess_refs.load(std::memory_order_relaxed),
				m_last_recount_expected_granules.load(std::memory_order_relaxed),
				m_last_recount_observed_granules.load(std::memory_order_relaxed),
				m_last_recount_mismatched_granules.load(std::memory_order_relaxed),
				m_last_recount_poisoned_granules.load(std::memory_order_relaxed),
				m_globally_poisoned.load(std::memory_order_relaxed),
				m_last_recount_expected_overflow.load(std::memory_order_relaxed),
			}
		};
	}

	void ownership_directory::reset_validation() noexcept
	{
		m_read_fault_probes.store(0, std::memory_order_relaxed);
		m_read_faults_handled.store(0, std::memory_order_relaxed);
		m_handled_maybe_texture.store(0, std::memory_order_relaxed);
		m_handled_clear.store(0, std::memory_order_relaxed);
		m_handled_inconclusive.store(0, std::memory_order_relaxed);
		m_unhandled_maybe_texture.store(0, std::memory_order_relaxed);
		m_recounts.store(0, std::memory_order_relaxed);
		m_recounts_with_mismatch.store(0, std::memory_order_relaxed);
		m_recount_cache_busy.store(0, std::memory_order_relaxed);
		m_recount_total_us.store(0, std::memory_order_relaxed);
		m_recount_max_us.store(0, std::memory_order_relaxed);
		m_underflows.store(0, std::memory_order_relaxed);
		m_overflows.store(0, std::memory_order_relaxed);
		m_abandoned_mutations.store(0, std::memory_order_relaxed);
		m_sequence_errors.store(0, std::memory_order_relaxed);
		m_last_recount_sequence.store(0, std::memory_order_relaxed);
		m_last_recount_sections.store(0, std::memory_order_relaxed);
		m_last_recount_expected_refs.store(0, std::memory_order_relaxed);
		m_last_recount_observed_refs.store(0, std::memory_order_relaxed);
		m_last_recount_missing_refs.store(0, std::memory_order_relaxed);
		m_last_recount_excess_refs.store(0, std::memory_order_relaxed);
		m_last_recount_expected_granules.store(0, std::memory_order_relaxed);
		m_last_recount_observed_granules.store(0, std::memory_order_relaxed);
		m_last_recount_mismatched_granules.store(0, std::memory_order_relaxed);
		m_last_recount_poisoned_granules.store(0, std::memory_order_relaxed);
		m_last_recount_expected_overflow.store(false, std::memory_order_relaxed);
	}

	u16 ownership_directory::count_at(u32 address) const noexcept
	{
		return m_no_access_owner_counts[address >> summary_granule_shift].load(std::memory_order_relaxed);
	}

	u16 ownership_directory::nontexture_count_at(u32 address) const noexcept
	{
		return m_nontexture_no_access_owner_counts[address >> summary_granule_shift].load(std::memory_order_relaxed);
	}

	u64 ownership_directory::sequence() const noexcept
	{
		return m_sequence.load(std::memory_order_acquire);
	}

	u64 ownership_directory::lifetime_epoch() const noexcept
	{
		return m_lifetime_epoch.load(std::memory_order_acquire);
	}

}
