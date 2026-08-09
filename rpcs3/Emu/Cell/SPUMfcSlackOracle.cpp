#include "stdafx.h"
#include "SPUMfcSlackOracle.h"

#include "MFC.h"
#include "SPUThread.h"
#include "Emu/RSX/RSXCoherenceStats.h"
#include "Emu/system_config.h"
#include "util/sysinfo.hpp"
#include "util/tsc.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace spu_mfc_slack
{
	tracker::tracker(result_sink sink, void* sink_context, u64 deadline_delta) noexcept
	{
		reset(sink, sink_context, deadline_delta);
	}

	void tracker::reset(result_sink sink, void* sink_context, u64 deadline_delta) noexcept
	{
		m_sink = sink;
		m_sink_context = sink_context;
		m_deadline_delta = deadline_delta;
		m_owner = 0;
		m_lifecycle_generation = 0;
		m_spu_id = 0;
		m_execution_generation = 0;
		m_query_generation = 0;
		m_publication_generation = 0;
		m_active = {};
		m_pending = {};
		m_query = {};
	}

	bool tracker::has_live_state() const noexcept
	{
		if (m_active.active || m_query.valid)
		{
			return true;
		}

		return std::any_of(m_pending.begin(), m_pending.end(), [](const pending_candidate& candidate)
		{
			return candidate.occupied;
		});
	}

	void tracker::emit(const pending_candidate& candidate, censor_reason censor, bool valid,
		u32 returned_bits, u64 return_ticks) noexcept
	{
		result out{};
		out.key = candidate.key;
		out.execution_generation = candidate.execution_generation;
		out.query_generation = m_query.query_generation;
		out.publication_generation = m_query.publication_generation;
		out.candidate_ticks = candidate.candidate_ticks;
		out.outer_complete_ticks = candidate.outer_complete_ticks;
		out.query_update_ticks = m_query.update_ticks;
		out.first_demand_ticks = m_query.first_demand_ticks;
		out.publication_ticks = m_query.publication_ticks;
		out.return_ticks = return_ticks;
		out.query_mask = m_query.requested_mask;
		out.published_bits = m_query.published_bits;
		out.returned_bits = returned_bits;
		out.spu_id = candidate.spu_id;
		out.list_eal = candidate.list_eal;
		out.list_size = candidate.list_size;
		out.tag = candidate.tag;
		out.cmd = candidate.cmd;
		out.mode = m_query.mode;
		out.censor = censor;
		out.valid = valid;

		if (m_query.update_ticks)
		{
			out.update_before_completion = m_query.update_ticks < candidate.outer_complete_ticks;
			out.completion_to_update_ticks = out.update_before_completion
				? 0 : m_query.update_ticks - candidate.outer_complete_ticks;
		}

		if (m_query.first_demand_ticks)
		{
			out.demand_before_completion = m_query.first_demand_ticks < candidate.outer_complete_ticks;
			out.completion_to_demand_ticks = out.demand_before_completion
				? 0 : m_query.first_demand_ticks - candidate.outer_complete_ticks;
		}

		if (m_sink)
		{
			m_sink(m_sink_context, out);
		}
	}

	void tracker::emit_active(const active_candidate& candidate, censor_reason censor, u64 now) noexcept
	{
		pending_candidate pending{};
		pending.key = candidate.key;
		pending.execution_generation = m_active.execution_generation;
		pending.candidate_ticks = candidate.ticks;
		pending.outer_complete_ticks = now;
		pending.spu_id = m_active.observation.spu_id;
		pending.list_eal = m_active.observation.list_eal;
		pending.list_size = m_active.observation.list_size;
		pending.tag = m_active.observation.tag;
		pending.cmd = m_active.observation.cmd;
		emit(pending, censor, false, 0, now);
	}

	void tracker::censor_mask(u32 mask, censor_reason censor, u64 now) noexcept
	{
		if (!mask)
		{
			return;
		}

		for (auto& candidate : m_pending)
		{
			if (candidate.occupied && (mask & std::rotl<u32>(1, candidate.tag)))
			{
				emit(candidate, censor, false, 0, now);
				candidate.occupied = false;
			}
		}
	}

	void tracker::censor_all(censor_reason censor, u64 now) noexcept
	{
		for (auto& candidate : m_pending)
		{
			if (candidate.occupied)
			{
				emit(candidate, censor, false, 0, now);
				candidate.occupied = false;
			}
		}
	}

	void tracker::expire(u64 now) noexcept
	{
		if (!m_deadline_delta)
		{
			return;
		}

		for (auto& candidate : m_pending)
		{
			if (candidate.occupied && now >= candidate.outer_complete_ticks &&
				now - candidate.outer_complete_ticks >= m_deadline_delta)
			{
				emit(candidate, censor_reason::deadline, false, 0, now);
				candidate.occupied = false;
			}
		}
	}

	void tracker::clear_query() noexcept
	{
		m_query = {};
	}

	void tracker::bind(u64 owner, u32 spu_id, u64 lifecycle_generation, u64 now) noexcept
	{
		if (m_owner && (m_owner != owner || m_lifecycle_generation != lifecycle_generation) && has_live_state())
		{
			invalidate(censor_reason::lifecycle_rebind, now);
		}

		m_owner = owner;
		m_lifecycle_generation = lifecycle_generation;
		m_spu_id = spu_id;
	}

	void tracker::begin_list(const list_observation& observation, u64 now) noexcept
	{
		expire(now);
		bind(observation.owner, observation.spu_id, observation.lifecycle_generation, now);

		if (m_active.active)
		{
			for (u8 index = 0; index < m_active.count; index++)
			{
				emit_active(m_active.candidates[index], censor_reason::lifecycle_rebind, now);
			}
		}

		m_active = {};
		m_active.observation = observation;
		m_active.execution_generation = ++m_execution_generation;
		m_active.active = true;
	}

	bool tracker::note_candidate(candidate_key key, u32 context_spu_id, u8 context_tag,
		bool context_is_get_list, u64 now) noexcept
	{
		expire(now);

		if (!key.serial || key.member == 0xffff)
		{
			return false;
		}

		active_candidate candidate{key, now};

		if (!m_active.active)
		{
			m_active.observation.spu_id = m_spu_id;
			emit_active(candidate, censor_reason::no_outer_list, now);
			return true;
		}

		if (!context_is_get_list || context_spu_id != m_active.observation.spu_id ||
			context_tag != m_active.observation.tag)
		{
			emit_active(candidate, censor_reason::not_get_list, now);
			return true;
		}

		for (u8 index = 0; index < m_active.count; index++)
		{
			if (m_active.candidates[index].key == key)
			{
				return false;
			}
		}

		if (m_active.count == active_candidate_capacity)
		{
			emit_active(candidate, censor_reason::active_overflow, now);
			return true;
		}

		m_active.candidates[m_active.count++] = candidate;
		return true;
	}

	void tracker::finish_list(bool completed, bool stalled, u64 now) noexcept
	{
		if (!m_active.active)
		{
			return;
		}

		censor_reason censor = censor_reason::none;

		if (stalled)
		{
			censor = censor_reason::list_stall;
		}
		else if (!completed)
		{
			censor = censor_reason::lifecycle_rebind;
		}
		else if (!m_active.observation.direct)
		{
			censor = censor_reason::queued_or_resumed;
		}
		else if (!m_active.observation.get_list)
		{
			censor = censor_reason::not_get_list;
		}
		else if (m_active.observation.ordered)
		{
			censor = censor_reason::outer_barrier_or_fence;
		}
		else if (m_active.observation.unsupported_tag_path)
		{
			censor = censor_reason::unsupported_optimized_tag_path;
		}

		for (u8 index = 0; index < m_active.count; index++)
		{
			const auto& active = m_active.candidates[index];

			if (censor != censor_reason::none)
			{
				emit_active(active, censor, now);
				continue;
			}

			auto found = std::find_if(m_pending.begin(), m_pending.end(), [](const pending_candidate& candidate)
			{
				return !candidate.occupied;
			});

			if (found == m_pending.end())
			{
				emit_active(active, censor_reason::pending_overflow, now);
				continue;
			}

			found->key = active.key;
			found->execution_generation = m_active.execution_generation;
			found->candidate_ticks = active.ticks;
			found->outer_complete_ticks = now;
			found->deadline_ticks = m_deadline_delta > std::numeric_limits<u64>::max() - now
				? std::numeric_limits<u64>::max() : now + m_deadline_delta;
			found->spu_id = m_active.observation.spu_id;
			found->list_eal = m_active.observation.list_eal;
			found->list_size = m_active.observation.list_size;
			found->tag = m_active.observation.tag;
			found->cmd = m_active.observation.cmd;
			found->occupied = true;
		}

		m_active = {};
	}

	void tracker::note_tag_mask(u32 mask, u64 now) noexcept
	{
		expire(now);

		if (m_query.valid && !m_query.published)
		{
			m_query.requested_mask = mask;
		}
	}

	void tracker::note_tag_update(query_mode mode, u32 mask, u64 now) noexcept
	{
		expire(now);

		if (m_query.valid)
		{
			censor_mask(m_query.requested_mask, censor_reason::query_overwrite, now);
			clear_query();
		}

		m_query.valid = true;
		m_query.query_generation = ++m_query_generation;
		m_query.update_ticks = now;
		m_query.requested_mask = mask;
		m_query.mode = mode;

		if (mode == query_mode::immediate)
		{
			censor_mask(mask, censor_reason::immediate_query, now);
		}
	}

	void tracker::note_tag_publication(query_mode mode, u32 mask, u32 bits, u64 now) noexcept
	{
		expire(now);

		if (!m_query.valid)
		{
			censor_mask(mask, censor_reason::missing_publication, now);
			m_query.valid = true;
			m_query.query_generation = ++m_query_generation;
			m_query.mode = mode;
			m_query.requested_mask = mask;
		}
		else if (m_query.mode != mode || m_query.requested_mask != mask)
		{
			censor_mask(m_query.requested_mask, censor_reason::query_overwrite, now);
			censor_mask(mask, censor_reason::missing_publication, now);
			m_query = {};
			m_query.valid = true;
			m_query.query_generation = ++m_query_generation;
			m_query.mode = mode;
			m_query.requested_mask = mask;
		}

		m_query.published = true;
		m_query.publication_generation = ++m_publication_generation;
		m_query.publication_ticks = now;
		m_query.published_bits = bits;
	}

	void tracker::note_rdtag_demand(u64 now) noexcept
	{
		expire(now);

		if (m_query.valid && !m_query.first_demand_ticks)
		{
			m_query.first_demand_ticks = now;
		}
	}

	void tracker::note_rdtag_return(u32 bits, u64 now) noexcept
	{
		expire(now);

		if (!m_query.valid)
		{
			censor_all(censor_reason::missing_publication, now);
			return;
		}

		if (!m_query.first_demand_ticks)
		{
			m_query.first_demand_ticks = now;
		}

		if (!m_query.published)
		{
			censor_mask(m_query.requested_mask, censor_reason::missing_publication, now);
			clear_query();
			return;
		}

		if (m_query.published_bits != bits)
		{
			censor_mask(m_query.requested_mask, censor_reason::query_overwrite, now);
			clear_query();
			return;
		}

		if (m_query.mode == query_mode::immediate)
		{
			censor_mask(m_query.requested_mask, censor_reason::immediate_query, now);
		}
		else if (m_query.mode == query_mode::any && !std::has_single_bit(m_query.requested_mask))
		{
			censor_mask(m_query.requested_mask, censor_reason::ambiguous_any, now);
		}
		else if (m_query.requested_mask)
		{
			for (auto& candidate : m_pending)
			{
				if (candidate.occupied && (m_query.requested_mask & std::rotl<u32>(1, candidate.tag)))
				{
					if (m_query.update_ticks < candidate.outer_complete_ticks)
					{
						emit(candidate, censor_reason::query_precedes_completion, false, bits, now);
					}
					else
					{
						emit(candidate, censor_reason::none, true, bits, now);
					}
					candidate.occupied = false;
				}
			}
		}

		clear_query();
	}

	void tracker::note_ordering_edge(bool standalone_barrier, bool ordered, u8 tag, u64 now) noexcept
	{
		expire(now);

		if (standalone_barrier)
		{
			censor_all(censor_reason::later_barrier_or_fence, now);
		}
		else if (ordered)
		{
			censor_mask(std::rotl<u32>(1, tag), censor_reason::later_barrier_or_fence, now);
		}
		else
		{
			censor_mask(std::rotl<u32>(1, tag), censor_reason::later_same_tag_work, now);
		}
	}

	void tracker::note_unsupported_tag_path(u64 now) noexcept
	{
		expire(now);
		censor_all(censor_reason::unsupported_optimized_tag_path, now);
		clear_query();
	}

	void tracker::invalidate(censor_reason censor, u64 now) noexcept
	{
		for (u8 index = 0; index < m_active.count; index++)
		{
			emit_active(m_active.candidates[index], censor, now);
		}

		m_active = {};
		censor_all(censor, now);
		clear_query();
	}

	namespace
	{
		constexpr usz censor_count = static_cast<usz>(censor_reason::count);

		struct alignas(128) result_slot
		{
			std::atomic<u64> sequence{0};
			result value{};
		};

		class result_ring
		{
			std::array<result_slot, result_ring_capacity> m_slots{};
			alignas(128) std::atomic<u64> m_enqueue{0};
			alignas(128) u64 m_dequeue = 0;
			alignas(128) std::atomic<u64> m_dropped{0};

		public:
			result_ring() noexcept
			{
				reset();
			}

			void reset() noexcept
			{
				m_enqueue.store(0, std::memory_order_relaxed);
				m_dequeue = 0;
				m_dropped.store(0, std::memory_order_relaxed);

				for (usz index = 0; index < result_ring_capacity; index++)
				{
					m_slots[index].sequence.store(index, std::memory_order_relaxed);
				}
			}

			bool try_push(const result& value) noexcept
			{
				u64 position = m_enqueue.load(std::memory_order_relaxed);
				result_slot* slot = nullptr;

				for (;;)
				{
					slot = &m_slots[position & (result_ring_capacity - 1)];
					const u64 sequence = slot->sequence.load(std::memory_order_acquire);
					const s64 difference = static_cast<s64>(sequence - position);

					if (!difference)
					{
						if (m_enqueue.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
						{
							break;
						}
					}
					else if (difference < 0)
					{
						m_dropped.fetch_add(1, std::memory_order_relaxed);
						return false;
					}
					else
					{
						position = m_enqueue.load(std::memory_order_relaxed);
					}
				}

				slot->value = value;
				slot->sequence.store(position + 1, std::memory_order_release);
				return true;
			}

			bool try_pop(result& value) noexcept
			{
				const u64 position = m_dequeue;
				auto& slot = m_slots[position & (result_ring_capacity - 1)];

				if (slot.sequence.load(std::memory_order_acquire) != position + 1)
				{
					return false;
				}

				value = slot.value;
				slot.sequence.store(position + result_ring_capacity, std::memory_order_release);
				m_dequeue = position + 1;
				return true;
			}

			u64 dropped() const noexcept
			{
				return m_dropped.load(std::memory_order_relaxed);
			}
		};

		struct counters
		{
			std::atomic<u64> candidates{0};
			std::atomic<u64> valid{0};
			std::atomic<u64> valid_all{0};
			std::atomic<u64> valid_any_one{0};
			std::atomic<u64> demand_before_completion{0};
			std::atomic<u64> update_before_completion{0};
			std::array<std::atomic<u64>, censor_count> censored{};
			std::atomic<u64> completion_to_demand_ticks{0};
			std::atomic<u64> completion_to_demand_max_ticks{0};
			std::atomic<u64> completion_to_update_ticks{0};
			std::atomic<u64> completion_to_update_max_ticks{0};
		};

		result_ring g_results;
		counters g_counters;
		std::atomic<u64> g_epoch{1};

		void update_max(std::atomic<u64>& target, u64 value) noexcept
		{
			u64 prior = target.load(std::memory_order_relaxed);
			while (prior < value && !target.compare_exchange_weak(prior, value, std::memory_order_relaxed))
			{
			}
		}

		void publish_result(void*, const result& value) noexcept
		{
			g_counters.demand_before_completion.fetch_add(value.demand_before_completion, std::memory_order_relaxed);
			g_counters.update_before_completion.fetch_add(value.update_before_completion, std::memory_order_relaxed);

			if (value.valid)
			{
				g_counters.valid.fetch_add(1, std::memory_order_relaxed);
				g_counters.valid_all.fetch_add(value.mode == query_mode::all, std::memory_order_relaxed);
				g_counters.valid_any_one.fetch_add(value.mode == query_mode::any, std::memory_order_relaxed);
				g_counters.completion_to_demand_ticks.fetch_add(value.completion_to_demand_ticks, std::memory_order_relaxed);
				update_max(g_counters.completion_to_demand_max_ticks, value.completion_to_demand_ticks);
				g_counters.completion_to_update_ticks.fetch_add(value.completion_to_update_ticks, std::memory_order_relaxed);
				update_max(g_counters.completion_to_update_max_ticks, value.completion_to_update_ticks);
			}
			else if (value.censor != censor_reason::none)
			{
				g_counters.censored[static_cast<usz>(value.censor)].fetch_add(1, std::memory_order_relaxed);
			}

			g_results.try_push(value);
		}

		u64 runtime_deadline_ticks() noexcept
		{
			const u64 frequency = utils::get_tsc_freq();
			return frequency && frequency <= std::numeric_limits<u64>::max() / 2 ? frequency * 2 : 0;
		}

		struct runtime_tls
		{
			tracker state{publish_result, nullptr, runtime_deadline_ticks()};
			u64 epoch = g_epoch.load(std::memory_order_relaxed);
		};

		thread_local runtime_tls g_tls;

		bool runtime_enabled() noexcept
		{
			return rsx::coherence_stats::is_enabled();
		}

		tracker& runtime_tracker(spu_thread* spu, u64 now) noexcept
		{
			const u64 epoch = g_epoch.load(std::memory_order_relaxed);

			if (g_tls.epoch != epoch)
			{
				g_tls.state.invalidate(censor_reason::lifecycle_rebind, now);
				g_tls.state.reset(publish_result, nullptr, runtime_deadline_ticks());
				g_tls.epoch = epoch;
			}

			if (spu)
			{
				const u64 lifecycle_generation = spu->mfc_slack_lifecycle_generation.load(std::memory_order_relaxed);
				g_tls.state.bind(reinterpret_cast<u64>(spu), spu->id, lifecycle_generation, now);
			}

			return g_tls.state;
		}

		query_mode convert_mode(u32 mode) noexcept
		{
			return mode == MFC_TAG_UPDATE_ANY ? query_mode::any :
				mode == MFC_TAG_UPDATE_ALL ? query_mode::all : query_mode::immediate;
		}
	}

	void note_candidate(u64 serial, u16 member) noexcept
	{
		if (!runtime_enabled())
		{
			return;
		}

		const u64 now = utils::get_tsc();
		const auto& context = rsx::coherence_stats::g_active_mfc_context;
		const bool is_get_list = context.valid &&
			(context.flags & (rsx::coherence_stats::mfc_context_get | rsx::coherence_stats::mfc_context_list)) ==
				(rsx::coherence_stats::mfc_context_get | rsx::coherence_stats::mfc_context_list) &&
			!(context.flags & rsx::coherence_stats::mfc_context_put);

		if (runtime_tracker(nullptr, now).note_candidate({serial, member}, context.spu_id,
			context.tag, is_get_list, now))
		{
			g_counters.candidates.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void note_tag_mask(spu_thread& spu, u32 mask) noexcept
	{
		if (runtime_enabled())
		{
			const u64 now = utils::get_tsc();
			runtime_tracker(&spu, now).note_tag_mask(mask, now);
		}
	}

	void note_tag_update(spu_thread& spu, u32 mode, u32 mask) noexcept
	{
		if (runtime_enabled())
		{
			const u64 now = utils::get_tsc();
			runtime_tracker(&spu, now).note_tag_update(convert_mode(mode), mask, now);
		}
	}

	void note_tag_publication(spu_thread& spu, u32 mode, u32 mask, u32 bits) noexcept
	{
		if (runtime_enabled())
		{
			const u64 now = utils::get_tsc();
			runtime_tracker(&spu, now).note_tag_publication(convert_mode(mode), mask, bits, now);
		}
	}

	void note_rdtag_demand(spu_thread& spu) noexcept
	{
		if (runtime_enabled())
		{
			const u64 now = utils::get_tsc();
			runtime_tracker(&spu, now).note_rdtag_demand(now);
		}
	}

	void note_rdtag_return(spu_thread& spu, u32 bits) noexcept
	{
		if (runtime_enabled())
		{
			const u64 now = utils::get_tsc();
			runtime_tracker(&spu, now).note_rdtag_return(bits, now);
		}
	}

	bool is_global_ordering_command(u8 cmd) noexcept
	{
		return cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD;
	}

	void note_mfc_ordering_edge(spu_thread& spu, u8 cmd, u8 tag) noexcept
	{
		if (runtime_enabled())
		{
			const bool standalone = is_global_ordering_command(cmd);
			const u8 base_cmd = cmd & ~(MFC_BARRIER_MASK | MFC_FENCE_MASK | MFC_RESULT_MASK);
			const bool transfer = base_cmd == MFC_PUT_CMD || base_cmd == MFC_PUTL_CMD ||
				base_cmd == MFC_GET_CMD || base_cmd == MFC_GETL_CMD || base_cmd == MFC_SNDSIG_CMD;

			if (!standalone && !transfer)
			{
				return;
			}

			const u64 now = utils::get_tsc();
			const bool ordered = !!(cmd & (MFC_BARRIER_MASK | MFC_FENCE_MASK));
			runtime_tracker(&spu, now).note_ordering_edge(standalone, ordered, tag, now);
		}
	}

	void note_enable_transition() noexcept
	{
		g_epoch.fetch_add(1, std::memory_order_release);
	}

	scoped_list_transfer::scoped_list_transfer(bool enabled, spu_thread& spu,
		const spu_mfc_cmd& args, bool direct) noexcept
		: m_spu(&spu)
		, m_args(&args)
		, m_uncaught_exceptions(std::uncaught_exceptions())
		, m_active(enabled)
	{
		if (!m_active)
		{
			return;
		}

		const u64 now = utils::get_tsc();
		list_observation observation{};
		observation.owner = reinterpret_cast<u64>(&spu);
		observation.lifecycle_generation = spu.mfc_slack_lifecycle_generation.load(std::memory_order_relaxed);
		observation.spu_id = spu.id;
		observation.list_eal = args.eal;
		observation.list_size = args.size;
		observation.tag = args.tag & 0x1f;
		observation.cmd = args.cmd;
		observation.direct = direct;
		observation.get_list = (args.cmd & ~(MFC_BARRIER_MASK | MFC_FENCE_MASK)) == MFC_GETL_CMD;
		observation.ordered = !!(args.cmd & (MFC_BARRIER_MASK | MFC_FENCE_MASK));
		observation.unsupported_tag_path = g_cfg.core.spu_decoder == spu_decoder_type::asmjit;
		runtime_tracker(&spu, now).begin_list(observation, now);
	}

	scoped_list_transfer::~scoped_list_transfer()
	{
		if (!m_active)
		{
			return;
		}

		if (!runtime_enabled())
		{
			return;
		}

		const bool stalled = !!(m_args->tag & 0x80);
		const bool completed = !stalled && std::uncaught_exceptions() == m_uncaught_exceptions;
		const u64 now = utils::get_tsc();
		runtime_tracker(m_spu, now).finish_list(completed, stalled, now);
	}

	snapshot get_snapshot() noexcept
	{
		snapshot out{};
		out.candidates = g_counters.candidates.load(std::memory_order_relaxed);
		out.valid = g_counters.valid.load(std::memory_order_relaxed);
		out.valid_all = g_counters.valid_all.load(std::memory_order_relaxed);
		out.valid_any_one = g_counters.valid_any_one.load(std::memory_order_relaxed);
		out.demand_before_completion = g_counters.demand_before_completion.load(std::memory_order_relaxed);
		out.update_before_completion = g_counters.update_before_completion.load(std::memory_order_relaxed);

		for (usz index = 0; index < censor_count; index++)
		{
			out.censored[index] = g_counters.censored[index].load(std::memory_order_relaxed);
		}

		out.completion_to_demand_ticks = g_counters.completion_to_demand_ticks.load(std::memory_order_relaxed);
		out.completion_to_demand_max_ticks = g_counters.completion_to_demand_max_ticks.load(std::memory_order_relaxed);
		out.completion_to_update_ticks = g_counters.completion_to_update_ticks.load(std::memory_order_relaxed);
		out.completion_to_update_max_ticks = g_counters.completion_to_update_max_ticks.load(std::memory_order_relaxed);
		out.ring_drops = g_results.dropped();
		return out;
	}

	bool try_pop_result(result& out) noexcept
	{
		return g_results.try_pop(out);
	}

	void reset() noexcept
	{
		g_epoch.fetch_add(1, std::memory_order_release);
		g_results.reset();
		g_counters.candidates.store(0, std::memory_order_relaxed);
		g_counters.valid.store(0, std::memory_order_relaxed);
		g_counters.valid_all.store(0, std::memory_order_relaxed);
		g_counters.valid_any_one.store(0, std::memory_order_relaxed);
		g_counters.demand_before_completion.store(0, std::memory_order_relaxed);
		g_counters.update_before_completion.store(0, std::memory_order_relaxed);
		g_counters.completion_to_demand_ticks.store(0, std::memory_order_relaxed);
		g_counters.completion_to_demand_max_ticks.store(0, std::memory_order_relaxed);
		g_counters.completion_to_update_ticks.store(0, std::memory_order_relaxed);
		g_counters.completion_to_update_max_ticks.store(0, std::memory_order_relaxed);

		for (auto& counter : g_counters.censored)
		{
			counter.store(0, std::memory_order_relaxed);
		}
	}
}

namespace rsx::coherence_stats
{
	// Declaration belongs beside the other coherence-stat entry points once the
	// CELLJOIN call site is stable. Keeping the implementation here avoids an RSX
	// dependency on Cell internals.
	void note_mfc_slack_candidate(u64 serial, u16 member) noexcept
	{
		spu_mfc_slack::note_candidate(serial, member);
	}

	void note_mfc_slack_enable_transition() noexcept
	{
		spu_mfc_slack::note_enable_transition();
	}
}
