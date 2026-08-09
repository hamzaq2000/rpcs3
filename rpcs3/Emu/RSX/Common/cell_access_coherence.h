#pragma once

#include "Utilities/address_range.h"
#include "util/types.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace rsx::cell_access
{
	// A Cell DMA is at most 16 KiB. Keeping this summary independent of the
	// host page size gives every supported host the same one-or-two-load fast
	// probe while exact ownership remains a texture-cache slow-path decision.
	constexpr u32 summary_granule_shift = 14;
	constexpr u32 summary_granule_size = 1u << summary_granule_shift;
	constexpr u32 summary_granule_count = 1u << (32 - summary_granule_shift);
	constexpr u16 poisoned_owner_count = 0xffff;
	static_assert(std::atomic<u16>::is_always_lock_free);
	static_assert(std::atomic<u64>::is_always_lock_free);

	enum class page_probe_result : u8
	{
		clear,
		maybe_texture,
		unstable,
		invalid,
		poisoned,
	};

	struct directory_recount_result
	{
		u64 sequence = 0;
		u64 sections = 0;
		u64 expected_refs = 0;
		u64 observed_refs = 0;
		u64 missing_refs = 0;
		u64 excess_refs = 0;
		u32 expected_granules = 0;
		u32 observed_granules = 0;
		u32 mismatched_granules = 0;
		u32 poisoned_granules = 0;
		bool globally_poisoned = false;
		bool expected_overflow = false;
	};

	struct directory_validation_snapshot
	{
		u64 read_fault_probes = 0;
		u64 read_faults_handled = 0;
		u64 handled_maybe_texture = 0;
		u64 handled_clear = 0;
		u64 handled_inconclusive = 0;
		u64 unhandled_maybe_texture = 0;
		u64 recounts = 0;
		u64 recounts_with_mismatch = 0;
		u64 recount_cache_busy = 0;
		u64 recount_total_us = 0;
		u64 recount_max_us = 0;
		u64 underflows = 0;
		u64 overflows = 0;
		u64 abandoned_mutations = 0;
		u64 sequence_errors = 0;
		bool globally_poisoned = false;
		directory_recount_result last_recount{};
	};

	struct stable_ownership_snapshot
	{
		page_probe_result texture = page_probe_result::invalid;
		bool maybe_nontexture = false;
		u64 lifetime_epoch = 0;
	};

	// Debug-only description of one deferred read-fault invalidation plan. The
	// fixed bounds are deliberate: the oracle must not allocate in a fault
	// handler, and an over-capacity plan is simply reported as unobserved.
	constexpr u32 exact_cohort_plan_capacity = 16;
	constexpr u32 exact_cohort_slot_capacity = 64;
	constexpr u32 exact_cohort_member_capacity = 64;
	constexpr u32 exact_cohort_completion_capacity = 1024;
	constexpr u32 exact_cohort_member_completion_capacity = 1024;
	static_assert(exact_cohort_member_capacity <= 64,
		"Exact-cohort member masks are u64");

	enum class exact_cohort_section_role : u8
	{
		flush,
		unprotect,
		exclude,
	};

	struct exact_cohort_section_token
	{
		u64 section_identity = 0;
		u64 section_session_generation = 0;
		u64 producer_identity = 0;
		u64 content_generation = 0;
		u64 staged_generation = 0;
		u64 transfer_generation = 0;
		u64 synchronization_generation = 0;
		u64 write_generation = 0;
		utils::address_range32 full_range{};
		utils::address_range32 confirmed_range{};
		utils::address_range32 locked_range{};
		u32 context = 0;
		u32 rsx_pitch = 0;
		u32 gcm_format = 0;
		u16 width = 0;
		u16 height = 0;
		u16 depth = 0;
		u16 mipmaps = 0;
		u8 protection = 0;
		u8 read_flags = 0;
		u8 synchronized = 0;
		u8 raw_rank = 0;
		u8 execution_rank = 0;
		bool swizzled = false;
		bool has_flush_exclusions = false;
		exact_cohort_section_role role = exact_cohort_section_role::flush;

		bool operator ==(const exact_cohort_section_token&) const = default;
	};

	struct exact_cohort_plan
	{
		u64 renderer_epoch = 0;
		u64 directory_sequence = 0;
		u64 cache_revision = 0;
		utils::address_range32 fault_range{};
		utils::address_range32 invalidate_range{};
		std::array<exact_cohort_section_token, exact_cohort_plan_capacity> sections{};
		u8 section_count = 0;

		bool same_semantic_plan(const exact_cohort_plan& other) const noexcept;
		bool same_semantic_plan_except_cache_revision(const exact_cohort_plan& other) const noexcept;
		bool same_native_cohort(const exact_cohort_plan& other) const noexcept;
	};

	// Captured from the faulting thread. Origin/MFC metadata deliberately does
	// not participate in observational plan grouping: the records quantify
	// whether a future broker can apply a stricter SPU-GET-only eligibility gate.
	struct exact_cohort_member_observation
	{
		u64 frame = 0;
		u64 fault_start_ticks = 0;
		u64 fault_end_ticks = 0;
		u64 flush_wait_start_ticks = 0;
		u64 flush_wait_end_ticks = 0;
		u64 readback_wait_start_ticks = 0;
		u64 readback_wait_end_ticks = 0;
		u64 readback_bytes = 0;
		u64 submission_ready_us = 0;
		u64 queue_ref_removed_us = 0;
		u64 data_ready_us = 0;
		u64 unprotect_done_us = 0;
		u64 semantic_ready_us = 0;
		u64 first_staged_completion_generation = 0;
		u32 fault_address = 0;
		u32 origin_id = 0;
		u32 origin_guest_pc = 0;
		u32 mfc_spu_id = 0;
		u32 mfc_ea = 0;
		u16 mfc_size = 0;
		u32 flush_wait_count = 0;
		u32 readback_wait_count = 0;
		u32 readback_count = 0;
		u32 transfer_count = 0;
		u32 copied_start = umax;
		u32 copied_end = 0;
		u16 flush_sections = 0;
		u16 unprotect_sections = 0;
		u16 discarded_sections = 0;
		u8 origin = 0;
		u8 mfc_source = 0;
		u8 mfc_cmd = 0;
		u8 mfc_tag = 0;
		u8 mfc_flags = 0;
		u8 queue_posts = 0;
		bool mfc_valid = false;
		bool mfc_contains_fault = false;
		bool replan_empty = false;
		bool cache_unchanged = false;
		bool semantic_plan_verified = false;
		bool completion_verified = false;

		bool is_spu_get() const noexcept;
	};

	enum class exact_cohort_terminal_kind : u8
	{
		success,
		resolved_noop,
		replan_mismatch,
		offloader,
		other_abandon,
	};

	struct exact_cohort_ticket
	{
		u64 serial = 0;
		u16 slot = 0xffff;
		u16 member = 0xffff;

		bool valid() const noexcept
		{
			return serial && slot < exact_cohort_slot_capacity &&
				member < exact_cohort_member_capacity;
		}

		bool is_proposed_leader() const noexcept
		{
			return valid() && member == 0;
		}
	};

	struct exact_cohort_snapshot
	{
		u64 leaders = 0;
		u64 same_plan_ranges_followers = 0;
		u64 different_plan_ranges_followers = 0;
		u64 cross_page_same_plan_pairs = 0;
		u64 cross_page_followers_covered_by_leader = 0;
		u64 cache_revision_splits = 0;
		u64 materializers_published = 0;
		u64 first_completion_with_readback = 0;
		u64 first_completion_without_readback = 0;
		u64 leader_completion_with_readback = 0;
		u64 leader_completion_without_readback = 0;
		u64 late_completion_with_readback = 0;
		u64 late_completion_without_readback = 0;
		u64 follower_arrival_us = 0;
		u64 follower_arrival_max_us = 0;
		u64 materializer_publish_latency_us = 0;
		u64 materializer_publish_latency_max_us = 0;
		u64 followers_at_materializer_publish = 0;
		u64 max_followers_at_materializer_publish = 0;
		u64 plan_overflow = 0;
		u64 slot_exhaustion = 0;
		u64 member_exhaustion = 0;
		u64 unknown_generation_rejections = 0;
		u64 flush_exclusion_rejections = 0;
		u64 preplan_mutation_rejections = 0;
		u64 invalid_directory_sequence = 0;
		u64 invalid_range_rejections = 0;
		u64 unsupported_backend = 0;
		u64 abandoned = 0;
		u64 replan_mismatches = 0;
		u64 resolved_noops = 0;
		u64 multiple_materializers = 0;
		u64 offloader_abandons = 0;
		u64 stale_completion = 0;
		u64 timing_complete = 0;
		u64 timing_with_any_readback = 0;
		u64 timing_with_leader_readback = 0;
		u64 timing_cross_page_complete = 0;
		u64 projected_tail_us = 0;
		u64 projected_tail_max_us = 0;
		u64 queue_tail_us = 0;
		u64 queue_tail_max_us = 0;
		u64 spu_get_leaders = 0;
		u64 spu_get_followers = 0;
		u64 spu_get_queue_posts = 0;
		u64 spu_get_follower_queue_posts = 0;
		u64 homogeneous_spu_get_cohorts = 0;
		u64 mixed_spu_get_cohorts = 0;
		u64 completion_record_drops = 0;
		u64 member_record_drops = 0;
		u64 incomplete_queue_timing = 0;
		u64 queue_timestamp_without_post = 0;
		u32 active_slots = 0;
		u32 occupied_slots = 0;
	};

	// A bounded completion record exposes intervals for an offline union. Tail
	// durations must not be summed and presented as wall-time or FPS savings.
	struct exact_cohort_completion
	{
		u64 serial = 0;
		u64 renderer_epoch = 0;
		u64 directory_sequence = 0;
		u64 cache_revision = 0;
		u64 started_us = 0;
		u64 first_done_us = 0;
		u64 materializer_data_ready_us = 0;
		u64 materializer_semantic_ready_us = 0;
		u64 proposed_leader_done_us = 0;
		u64 last_member_done_us = 0;
		u64 last_terminal_us = 0;
		u64 first_readback_data_ready_us = 0;
		u64 last_readback_data_ready_us = 0;
		u64 projected_tail_us = 0;
		u64 materializer_tail_us = 0;
		u64 materializer_queue_release_us = 0;
		u64 proposed_leader_queue_release_us = 0;
		u64 last_queue_release_us = 0;
		u64 queue_tail_us = 0;
		u64 proposed_leader_queue_tail_us = 0;
		u64 frame_min = 0;
		u64 frame_max = 0;
		u32 registrations = 0;
		u32 successful_members = 0;
		u32 resolved_noop_members = 0;
		u32 abandoned_members = 0;
		u32 queue_posts = 0;
		u32 follower_queue_posts = 0;
		u16 first_done_member = 0xffff;
		u16 spu_get_members = 0;
		u16 cross_page_members = 0;
		utils::address_range32 fault_range{};
		utils::address_range32 invalidate_range{};
		utils::address_range32 materializer_fault_range{};
		utils::address_range32 materializer_invalidate_range{};
		u64 first_section_identity = 0;
		u64 first_section_session_generation = 0;
		u64 first_producer_identity = 0;
		u64 first_content_generation = 0;
		u8 section_count = 0;
		bool first_done_had_readback = false;
		bool proposed_leader_had_readback = false;
		bool any_readback = false;
		bool proposed_leader_materialized = false;
		bool proposed_leader_covers_all_faults = false;
		bool materializer_covers_all_faults = false;
		bool proposed_leader_valid = false;
		bool complete = false;
	};

	struct exact_cohort_member_completion
	{
		u64 serial = 0;
		u64 done_us = 0;
		u16 member = 0xffff;
		exact_cohort_terminal_kind terminal = exact_cohort_terminal_kind::other_abandon;
		bool first_successful_completion = false;
		bool after_first_successful_completion = false;
		bool after_proposed_leader_completion = false;
		utils::address_range32 fault_range{};
		utils::address_range32 invalidate_range{};
		exact_cohort_member_observation observation{};
	};

	class exact_cohort_oracle
	{
		struct slot_state
		{
			exact_cohort_plan plan{};
			u64 serial = 0;
			u64 started_us = 0;
			u64 first_done_us = 0;
			u64 proposed_leader_done_us = 0;
			u64 last_member_done_us = 0;
			u64 last_terminal_us = 0;
			u64 first_readback_data_ready_us = 0;
			u64 last_readback_data_ready_us = 0;
			u64 frame_min = 0;
			u64 frame_max = 0;
			u64 resolved_mask = 0;
			u64 terminal_mask = 0;
			u64 after_first_success_mask = 0;
			u64 after_proposed_leader_mask = 0;
			u32 registrations = 0;
			u32 resolved_calls = 0;
			u32 terminal_calls = 0;
			u32 successful_calls = 0;
			u32 resolved_noop_calls = 0;
			u32 abandoned_calls = 0;
			u16 first_done_member = 0xffff;
			bool first_done_had_readback = false;
			bool proposed_leader_had_readback = false;
			bool any_readback = false;
			bool occupied = false;
			bool active = false;
			std::array<exact_cohort_member_observation, exact_cohort_member_capacity> members{};
			std::array<exact_cohort_terminal_kind, exact_cohort_member_capacity> terminals{};
			std::array<utils::address_range32, exact_cohort_member_capacity> member_fault_ranges{};
			std::array<utils::address_range32, exact_cohort_member_capacity> member_invalidate_ranges{};
		};

		mutable std::mutex m_mutex;
		std::array<slot_state, exact_cohort_slot_capacity> m_slots{};
		std::array<exact_cohort_completion, exact_cohort_completion_capacity> m_completions{};
		std::array<exact_cohort_member_completion, exact_cohort_member_completion_capacity> m_member_completions{};
		u32 m_completion_read = 0;
		u32 m_completion_write = 0;
		u32 m_completion_count = 0;
		u32 m_member_completion_read = 0;
		u32 m_member_completion_write = 0;
		u32 m_member_completion_count = 0;
		u64 m_next_serial = 1;
		exact_cohort_snapshot m_totals{};

		void finish_timing(slot_state& slot) noexcept;
		void push_member_completion(const exact_cohort_member_completion& completion) noexcept;

	public:
		exact_cohort_ticket begin(const exact_cohort_plan& plan,
			const exact_cohort_member_observation& observation = {}) noexcept;
		bool resolve_semantic(exact_cohort_ticket ticket, exact_cohort_terminal_kind terminal,
			const exact_cohort_member_observation& observation = {}) noexcept;
		void finalize_member(exact_cohort_ticket ticket,
			const exact_cohort_member_observation& observation = {}) noexcept;
		void complete(exact_cohort_ticket ticket,
			const exact_cohort_member_observation& observation = {}) noexcept;
		void resolve_noop(exact_cohort_ticket ticket,
			const exact_cohort_member_observation& observation = {}) noexcept;
		void abandon(exact_cohort_ticket ticket, exact_cohort_terminal_kind reason,
			const exact_cohort_member_observation& observation = {}) noexcept;
		void record_plan_overflow() noexcept;
		void record_preplan_mutation() noexcept;
		void record_unsupported_backend() noexcept;
		exact_cohort_snapshot snapshot() const noexcept;
		bool try_pop_completion(exact_cohort_completion& result) noexcept;
		bool try_pop_member_completion(exact_cohort_member_completion& result) noexcept;
		bool matches_ticket_plan(exact_cohort_ticket ticket,
			const exact_cohort_plan& plan) const noexcept;
		void reset() noexcept;
	};

	extern exact_cohort_oracle g_exact_cohort_oracle;
	u64 allocate_exact_cohort_session_generation() noexcept;

	class ownership_directory
	{
	public:
		class mutation
		{
			ownership_directory* m_owner = nullptr;
			std::unique_lock<std::mutex> m_lock;
			utils::address_range32 m_old_range{};
			bool m_old_no_access = false;
			bool m_nontexture = false;
			bool m_committed = false;

			mutation(ownership_directory& owner, const utils::address_range32& old_range,
				bool old_no_access, bool nontexture) noexcept;
			friend class ownership_directory;

		public:
			mutation(const mutation&) = delete;
			mutation& operator=(const mutation&) = delete;
			mutation(mutation&&) = delete;
			mutation& operator=(mutation&&) = delete;
			~mutation();

			void commit(const utils::address_range32& new_range, bool new_no_access) noexcept;
		};

		class stable_session
		{
			ownership_directory* m_owner = nullptr;
			std::unique_lock<std::mutex> m_lock;

			explicit stable_session(ownership_directory& owner) noexcept;
			friend class ownership_directory;

		public:
			stable_session(const stable_session&) = delete;
			stable_session& operator=(const stable_session&) = delete;
			stable_session(stable_session&&) = delete;
			stable_session& operator=(stable_session&&) = delete;

			stable_ownership_snapshot probe(u32 address, u32 size) const noexcept;
			u64 sequence() const noexcept;
		};

		class recount_session
		{
			ownership_directory* m_owner = nullptr;
			std::unique_lock<std::mutex> m_lock;
			u64 m_sections = 0;
			u64 m_expected_refs = 0;
			bool m_expected_overflow = false;
			bool m_finished = false;

			explicit recount_session(ownership_directory& owner) noexcept;
			friend class ownership_directory;

		public:
			recount_session(const recount_session&) = delete;
			recount_session& operator=(const recount_session&) = delete;
			recount_session(recount_session&&) = delete;
			recount_session& operator=(recount_session&&) = delete;

			void add(const utils::address_range32& range) noexcept;
			directory_recount_result finish() noexcept;
		};

		ownership_directory() = default;
		ownership_directory(const ownership_directory&) = delete;
		ownership_directory& operator=(const ownership_directory&) = delete;

		mutation begin_mutation(const utils::address_range32& old_range, bool old_no_access) noexcept;
		mutation begin_nontexture_mutation(const utils::address_range32& old_range, bool old_no_access) noexcept;
		recount_session begin_recount() noexcept;
		// Global lock order: texture-cache or ZCULL pages lock, then this
		// directory lock. This session is innermost:
		// never acquire either source lock while it lives.
		stable_session begin_stable_session() noexcept;

		// Transfers a temporary physical NO-access owner to an already-published
		// texture owner without exposing a stable owner-free interval.
		bool handoff_nontexture_to_texture(const utils::address_range32& nontexture_range,
			const utils::address_range32& exact_texture_range) noexcept;

		// This is only valid before a newly constructed renderer begins backend
		// initialization and before Cell execution can resume.
		u64 begin_renderer_lifetime_quiescent() noexcept;

		// Clear means only that no texture-cache NO-access owner is summarized for
		// this range. It does not prove that VM, ZCULL, or another subsystem permits
		// direct access, and it is not a lifetime/protection pin.
		page_probe_result probe(u32 address, u32 size) const noexcept;
		void record_read_fault_probe(page_probe_result probe, bool texture_handled) noexcept;
		void record_recount_cache_busy() noexcept;
		void record_recount_duration(u64 duration_us) noexcept;
		directory_validation_snapshot validation_snapshot() const noexcept;
		void reset_validation() noexcept;

		// Test/validation accessors. They never control emulation behavior.
		u16 count_at(u32 address) const noexcept;
		u16 nontexture_count_at(u32 address) const noexcept;
		u64 sequence() const noexcept;
		u64 lifetime_epoch() const noexcept;

	private:
		std::array<std::atomic<u16>, summary_granule_count> m_no_access_owner_counts{};
		std::array<std::atomic<u16>, summary_granule_count> m_nontexture_no_access_owner_counts{};
		std::array<u16, summary_granule_count> m_recount_scratch{};
		std::atomic<u64> m_sequence{0};
		std::atomic<u64> m_lifetime_epoch{0};
		std::atomic<bool> m_globally_poisoned{false};
		std::mutex m_writer_mutex;

		std::atomic<u64> m_read_fault_probes{0};
		std::atomic<u64> m_read_faults_handled{0};
		std::atomic<u64> m_handled_maybe_texture{0};
		std::atomic<u64> m_handled_clear{0};
		std::atomic<u64> m_handled_inconclusive{0};
		std::atomic<u64> m_unhandled_maybe_texture{0};
		std::atomic<u64> m_recounts{0};
		std::atomic<u64> m_recounts_with_mismatch{0};
		std::atomic<u64> m_recount_cache_busy{0};
		std::atomic<u64> m_recount_total_us{0};
		std::atomic<u64> m_recount_max_us{0};
		std::atomic<u64> m_underflows{0};
		std::atomic<u64> m_overflows{0};
		std::atomic<u64> m_abandoned_mutations{0};
		std::atomic<u64> m_sequence_errors{0};
		std::atomic<u64> m_last_recount_sequence{0};
		std::atomic<u64> m_last_recount_sections{0};
		std::atomic<u64> m_last_recount_expected_refs{0};
		std::atomic<u64> m_last_recount_observed_refs{0};
		std::atomic<u64> m_last_recount_missing_refs{0};
		std::atomic<u64> m_last_recount_excess_refs{0};
		std::atomic<u32> m_last_recount_expected_granules{0};
		std::atomic<u32> m_last_recount_observed_granules{0};
		std::atomic<u32> m_last_recount_mismatched_granules{0};
		std::atomic<u32> m_last_recount_poisoned_granules{0};
		std::atomic<bool> m_last_recount_expected_overflow{false};

		static std::pair<u32, u32> granule_span(const utils::address_range32& range) noexcept;
		void apply_transition(const utils::address_range32& old_range, bool old_no_access,
			const utils::address_range32& new_range, bool new_no_access, bool nontexture) noexcept;
		void add_owner(u32 granule, bool nontexture) noexcept;
		void remove_owner(u32 granule, bool nontexture) noexcept;
		void finish_mutation() noexcept;
		stable_ownership_snapshot probe_locked(u32 address, u32 size) const noexcept;
		directory_recount_result finish_recount_locked(u64 sections, u64 expected_refs,
			bool expected_overflow) noexcept;
	};

	extern ownership_directory g_ownership_directory;
}
