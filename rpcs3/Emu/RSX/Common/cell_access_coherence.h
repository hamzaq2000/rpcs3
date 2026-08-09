#pragma once

#include "Utilities/address_range.h"
#include "util/types.hpp"

#include <array>
#include <atomic>
#include <mutex>

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
		// Global lock order: renderer-lifetime shared lock, texture-cache or
		// ZCULL pages lock, then this directory lock. This session is innermost:
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
