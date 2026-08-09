#pragma once

#include "Utilities/address_range.h"
#include "util/types.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace rsx
{
	class thread;
}

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

	enum class ready_get_result : u8
	{
		hit_backing_receipt,
		fallback_no_renderer,
		fallback_epoch,
		fallback_nontexture,
		fallback_no_native_sibling,
		fallback_exact_owner,
		fallback_no_receipt,
		fallback_stale_generation,
		fallback_ambiguous_receipt,
		fallback_directory,
		count,
	};

	struct cell_backing_receipt
	{
		utils::address_range32 range{};
		u64 content_generation = 0;
		u64 lifetime_epoch = 0;

		void clear() noexcept
		{
			range.invalidate();
			content_generation = 0;
			lifetime_epoch = 0;
		}

		void publish(const utils::address_range32& copied_range, u64 generation, u64 epoch) noexcept
		{
			if (!copied_range.valid() || !generation || !epoch)
			{
				clear();
				return;
			}

			range = copied_range;
			content_generation = generation;
			lifetime_epoch = epoch;
		}

		bool matches(const utils::address_range32& requested_range, u64 epoch) const noexcept
		{
			return range.valid() && content_generation && lifetime_epoch == epoch &&
				requested_range.valid() && requested_range.inside(range);
		}

		bool matches_generation(u64 current_generation) const noexcept
		{
			return content_generation && content_generation == current_generation;
		}
	};

	// Pins intrusive RSX resources across ownership handoffs. Acquiring the new
	// ref before releasing the old one prevents a same-resource lifetime gap.
	template <typename Resource>
	class intrusive_lifetime_pin
	{
		Resource* m_resource = nullptr;

	public:
		intrusive_lifetime_pin() = default;
		intrusive_lifetime_pin(const intrusive_lifetime_pin&) = delete;
		intrusive_lifetime_pin& operator=(const intrusive_lifetime_pin&) = delete;

		~intrusive_lifetime_pin()
		{
			reset();
		}

		void reset(Resource* resource = nullptr) noexcept
		{
			if (resource == m_resource)
			{
				return;
			}
			if (resource)
			{
				resource->add_ref();
			}

			auto* previous = std::exchange(m_resource, resource);
			if (previous)
			{
				previous->release();
			}
		}

		Resource* get() const noexcept
		{
			return m_resource;
		}
	};

	class ownership_directory
	{
	public:
		class renderer_lifetime_session
		{
			ownership_directory* m_owner = nullptr;
			std::shared_lock<std::shared_mutex> m_lock;
			rsx::thread* m_renderer = nullptr;
			u64 m_epoch = 0;

			explicit renderer_lifetime_session(ownership_directory& owner) noexcept;
			friend class ownership_directory;

		public:
			renderer_lifetime_session(const renderer_lifetime_session&) = delete;
			renderer_lifetime_session& operator=(const renderer_lifetime_session&) = delete;
			renderer_lifetime_session(renderer_lifetime_session&&) = delete;
			renderer_lifetime_session& operator=(renderer_lifetime_session&&) = delete;

			rsx::thread* renderer() const noexcept { return m_renderer; }
			u64 epoch() const noexcept { return m_epoch; }
		};

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
		renderer_lifetime_session begin_renderer_lifetime_session() noexcept;
		void publish_renderer_lifetime(rsx::thread* renderer) noexcept;
		void unpublish_renderer_lifetime(rsx::thread* renderer) noexcept;

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
		void record_ready_get_result(ready_get_result result) noexcept;
		u64 ready_get_result_count(ready_get_result result) const noexcept;

	private:
		std::array<std::atomic<u16>, summary_granule_count> m_no_access_owner_counts{};
		std::array<std::atomic<u16>, summary_granule_count> m_nontexture_no_access_owner_counts{};
		std::array<u16, summary_granule_count> m_recount_scratch{};
		std::atomic<u64> m_sequence{0};
		std::atomic<u64> m_lifetime_epoch{0};
		std::atomic<bool> m_globally_poisoned{false};
		std::mutex m_writer_mutex;
		std::shared_mutex m_renderer_lifetime_mutex;
		rsx::thread* m_renderer = nullptr;
		std::array<std::atomic<u64>, static_cast<usz>(ready_get_result::count)> m_ready_get_results{};

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
