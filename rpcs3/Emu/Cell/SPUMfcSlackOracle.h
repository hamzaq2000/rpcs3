#pragma once

#include "util/types.hpp"

#include <array>
#include <atomic>
#include <exception>
#include <type_traits>

class spu_thread;
struct spu_mfc_cmd;

namespace spu_mfc_slack
{
	constexpr usz active_candidate_capacity = 16;
	constexpr usz pending_candidate_capacity = 64;
	constexpr usz result_ring_capacity = 4096;

	struct candidate_key
	{
		u64 serial = 0;
		u16 member = 0xffff;

		bool operator ==(const candidate_key&) const = default;
	};

	enum class censor_reason : u8
	{
		none,
		no_outer_list,
		not_get_list,
		queued_or_resumed,
		list_stall,
		outer_barrier_or_fence,
		later_barrier_or_fence,
		later_same_tag_work,
		immediate_query,
		ambiguous_any,
		query_overwrite,
		missing_publication,
		query_precedes_completion,
		unsupported_optimized_tag_path,
		active_overflow,
		pending_overflow,
		lifecycle_rebind,
		deadline,
		count,
	};

	enum class query_mode : u8
	{
		immediate,
		any,
		all,
	};

	struct list_observation
	{
		u64 owner = 0;
		u64 lifecycle_generation = 0;
		u32 spu_id = 0;
		u32 list_eal = 0;
		u16 list_size = 0;
		u8 tag = 0;
		u8 cmd = 0;
		bool direct = false;
		bool get_list = false;
		bool ordered = false;
		bool unsupported_tag_path = false;
	};

	struct result
	{
		candidate_key key{};
		u64 execution_generation = 0;
		u64 query_generation = 0;
		u64 publication_generation = 0;
		u64 candidate_ticks = 0;
		u64 outer_complete_ticks = 0;
		u64 query_update_ticks = 0;
		u64 first_demand_ticks = 0;
		u64 publication_ticks = 0;
		u64 return_ticks = 0;
		u64 completion_to_demand_ticks = 0;
		u64 completion_to_update_ticks = 0;
		u32 query_mask = 0;
		u32 published_bits = 0;
		u32 returned_bits = 0;
		u32 spu_id = 0;
		u32 list_eal = 0;
		u16 list_size = 0;
		u8 tag = 0;
		u8 cmd = 0;
		query_mode mode = query_mode::immediate;
		censor_reason censor = censor_reason::none;
		bool valid = false;
		bool update_before_completion = false;
		bool demand_before_completion = false;
	};

	static_assert(std::is_trivially_copyable_v<result>);

	using result_sink = void(*)(void*, const result&) noexcept;

	// Backend-free state machine used by the runtime TLS wrapper and unit tests.
	// It owns only fixed storage and never allocates.
	class tracker
	{
		struct active_candidate
		{
			candidate_key key{};
			u64 ticks = 0;
		};

		struct pending_candidate
		{
			candidate_key key{};
			u64 execution_generation = 0;
			u64 candidate_ticks = 0;
			u64 outer_complete_ticks = 0;
			u64 deadline_ticks = 0;
			u32 spu_id = 0;
			u32 list_eal = 0;
			u16 list_size = 0;
			u8 tag = 0;
			u8 cmd = 0;
			bool occupied = false;
		};

		struct active_list
		{
			list_observation observation{};
			std::array<active_candidate, active_candidate_capacity> candidates{};
			u64 execution_generation = 0;
			u8 count = 0;
			bool active = false;
		};

		struct tag_query
		{
			u64 query_generation = 0;
			u64 publication_generation = 0;
			u64 update_ticks = 0;
			u64 publication_ticks = 0;
			u64 first_demand_ticks = 0;
			u32 requested_mask = 0;
			u32 published_bits = 0;
			query_mode mode = query_mode::immediate;
			bool valid = false;
			bool published = false;
		};

		result_sink m_sink = nullptr;
		void* m_sink_context = nullptr;
		u64 m_deadline_delta = 0;
		u64 m_owner = 0;
		u64 m_lifecycle_generation = 0;
		u32 m_spu_id = 0;
		u64 m_execution_generation = 0;
		u64 m_query_generation = 0;
		u64 m_publication_generation = 0;
		active_list m_active{};
		std::array<pending_candidate, pending_candidate_capacity> m_pending{};
		tag_query m_query{};

		void emit(const pending_candidate& candidate, censor_reason censor, bool valid,
			u32 returned_bits, u64 return_ticks) noexcept;
		void emit_active(const active_candidate& candidate, censor_reason censor, u64 now) noexcept;
		void censor_mask(u32 mask, censor_reason censor, u64 now) noexcept;
		void censor_all(censor_reason censor, u64 now) noexcept;
		void expire(u64 now) noexcept;
		void clear_query() noexcept;
		bool has_live_state() const noexcept;

	public:
		explicit tracker(result_sink sink = nullptr, void* sink_context = nullptr,
			u64 deadline_delta = 0) noexcept;

		void reset(result_sink sink = nullptr, void* sink_context = nullptr,
			u64 deadline_delta = 0) noexcept;
		void bind(u64 owner, u32 spu_id, u64 lifecycle_generation, u64 now) noexcept;
		void begin_list(const list_observation& observation, u64 now) noexcept;
		bool note_candidate(candidate_key key, u32 context_spu_id, u8 context_tag,
			bool context_is_get_list, u64 now) noexcept;
		void finish_list(bool completed, bool stalled, u64 now) noexcept;
		void note_tag_mask(u32 mask, u64 now) noexcept;
		void note_tag_update(query_mode mode, u32 mask, u64 now) noexcept;
		void note_tag_publication(query_mode mode, u32 mask, u32 bits, u64 now) noexcept;
		void note_rdtag_demand(u64 now) noexcept;
		void note_rdtag_return(u32 bits, u64 now) noexcept;
		void note_ordering_edge(bool standalone_barrier, bool ordered, u8 tag, u64 now) noexcept;
		void note_unsupported_tag_path(u64 now) noexcept;
		void invalidate(censor_reason censor, u64 now) noexcept;
	};

	struct snapshot
	{
		u64 candidates = 0;
		u64 valid = 0;
		u64 valid_all = 0;
		u64 valid_any_one = 0;
		u64 demand_before_completion = 0;
		u64 update_before_completion = 0;
		std::array<u64, static_cast<usz>(censor_reason::count)> censored{};
		u64 completion_to_demand_ticks = 0;
		u64 completion_to_demand_max_ticks = 0;
		u64 completion_to_update_ticks = 0;
		u64 completion_to_update_max_ticks = 0;
		u64 ring_drops = 0;
	};

	// Runtime API. Hooks are active only while RSX coherence diagnostics are enabled.
	void note_candidate(u64 serial, u16 member) noexcept;
	void note_tag_mask(spu_thread& spu, u32 mask) noexcept;
	void note_tag_update(spu_thread& spu, u32 mode, u32 mask) noexcept;
	void note_tag_publication(spu_thread& spu, u32 mode, u32 mask, u32 bits) noexcept;
	void note_rdtag_demand(spu_thread& spu) noexcept;
	void note_rdtag_return(spu_thread& spu, u32 bits) noexcept;
	bool is_global_ordering_command(u8 cmd) noexcept;
	void note_mfc_ordering_edge(spu_thread& spu, u8 cmd, u8 tag) noexcept;
	void note_enable_transition() noexcept;

	class scoped_list_transfer
	{
		spu_thread* m_spu = nullptr;
		const spu_mfc_cmd* m_args = nullptr;
		int m_uncaught_exceptions = 0;
		bool m_active = false;

	public:
		scoped_list_transfer(bool enabled, spu_thread& spu, const spu_mfc_cmd& args,
			bool direct) noexcept;
		scoped_list_transfer(const scoped_list_transfer&) = delete;
		scoped_list_transfer& operator=(const scoped_list_transfer&) = delete;
		~scoped_list_transfer();
	};

	snapshot get_snapshot() noexcept;
	bool try_pop_result(result& out) noexcept;
	void reset() noexcept;
}
