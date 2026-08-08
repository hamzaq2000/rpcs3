#pragma once

#include "util/tsc.hpp"
#include "util/types.hpp"

#include <array>
#include <atomic>

namespace rsx::coherence_stats
{
	// Apple silicon reports 128-byte data cache lines. Keep the hot enable flag
	// and per-SPU MFC writers on distinct physical lines during this diagnostic.
	constexpr usz coherence_cache_line_size = 128;

	struct timed_counter
	{
		std::atomic<u64> count{0};
		std::atomic<u64> ticks{0};
	};

	struct alignas(coherence_cache_line_size) enabled_flag
	{
		std::atomic<bool> value{false};
	};

	static_assert(sizeof(enabled_flag) == coherence_cache_line_size);

	struct alignas(coherence_cache_line_size) timed_counter_shard
	{
		timed_counter value;
	};

	static_assert(sizeof(timed_counter_shard) == coherence_cache_line_size);

	constexpr usz spu_mfc_shard_count = 64;

	struct ledger
	{
		enabled_flag enabled;

		timed_counter renderer_fault_ppu;
		timed_counter renderer_fault_spu;
		timed_counter renderer_fault_other;
		timed_counter vk_fault_probe;
		timed_counter vk_flush_wait;
		timed_counter gpu_event_wait;
		timed_counter gpu_readback_wait;
		std::atomic<u64> gpu_readback_bytes{0};
		timed_counter spu_channel_wait;
		std::array<timed_counter_shard, spu_mfc_shard_count> spu_mfc_process;
	};

	static_assert(std::atomic<bool>::is_always_lock_free);
	static_assert(std::atomic<u64>::is_always_lock_free);

	inline ledger g_ledger;

	inline bool is_enabled() noexcept
	{
		return g_ledger.enabled.value.load(std::memory_order_relaxed);
	}

	inline void set_enabled(bool enabled) noexcept
	{
		if (is_enabled() != enabled)
		{
			g_ledger.enabled.value.store(enabled, std::memory_order_relaxed);
		}
	}

	inline void reset(timed_counter& counter) noexcept
	{
		counter.count.store(0, std::memory_order_relaxed);
		counter.ticks.store(0, std::memory_order_relaxed);
	}

	inline void reset() noexcept
	{
		set_enabled(false);
		reset(g_ledger.renderer_fault_ppu);
		reset(g_ledger.renderer_fault_spu);
		reset(g_ledger.renderer_fault_other);
		reset(g_ledger.vk_fault_probe);
		reset(g_ledger.vk_flush_wait);
		reset(g_ledger.gpu_event_wait);
		reset(g_ledger.gpu_readback_wait);
		g_ledger.gpu_readback_bytes.store(0, std::memory_order_relaxed);
		reset(g_ledger.spu_channel_wait);

		for (auto& shard : g_ledger.spu_mfc_process)
		{
			reset(shard.value);
		}
	}

	inline timed_counter& mfc_process_counter(u32 spu_id) noexcept
	{
		return g_ledger.spu_mfc_process[spu_id & (spu_mfc_shard_count - 1)].value;
	}

	inline void record(timed_counter& counter, u64 ticks) noexcept
	{
		counter.count.fetch_add(1, std::memory_order_relaxed);
		counter.ticks.fetch_add(ticks, std::memory_order_relaxed);
	}

	class scoped_timer
	{
		timed_counter* m_primary = nullptr;
		timed_counter* m_secondary = nullptr;
		u64 m_start = 0;

	public:
		explicit scoped_timer(timed_counter& primary, timed_counter* secondary = nullptr) noexcept
		{
			if (is_enabled())
			{
				m_primary = &primary;
				m_secondary = secondary;
				m_start = utils::get_tsc();
			}
		}

		scoped_timer(const scoped_timer&) = delete;
		scoped_timer& operator=(const scoped_timer&) = delete;

		~scoped_timer()
		{
			if (!m_primary)
			{
				return;
			}

			const u64 elapsed = utils::get_tsc() - m_start;
			record(*m_primary, elapsed);

			if (m_secondary)
			{
				record(*m_secondary, elapsed);
			}
		}

		void cancel() noexcept
		{
			m_primary = nullptr;
			m_secondary = nullptr;
		}
	};

	struct active_mfc_timer
	{
		timed_counter* counter = nullptr;
		u64 start = 0;
	};

	inline thread_local active_mfc_timer g_active_mfc_timer;

	inline void finish_active_mfc_process() noexcept
	{
		timed_counter* counter = g_active_mfc_timer.counter;

		if (!counter)
		{
			return;
		}

		const u64 start = g_active_mfc_timer.start;
		g_active_mfc_timer = {};
		record(*counter, utils::get_tsc() - start);
	}

	class scoped_mfc_timer
	{
	public:
		explicit scoped_mfc_timer(u32 spu_id) noexcept
		{
			if (is_enabled())
			{
				g_active_mfc_timer.counter = &mfc_process_counter(spu_id);
				g_active_mfc_timer.start = utils::get_tsc();
			}
		}

		scoped_mfc_timer(const scoped_mfc_timer&) = delete;
		scoped_mfc_timer& operator=(const scoped_mfc_timer&) = delete;

		~scoped_mfc_timer()
		{
			finish_active_mfc_process();
		}
	};

	inline void record_readback_bytes(u64 bytes) noexcept
	{
		if (is_enabled())
		{
			g_ledger.gpu_readback_bytes.fetch_add(bytes, std::memory_order_relaxed);
		}
	}

	struct timed_snapshot
	{
		u64 count;
		u64 ticks;
	};

	struct ledger_snapshot
	{
		timed_snapshot renderer_fault_ppu;
		timed_snapshot renderer_fault_spu;
		timed_snapshot renderer_fault_other;
		timed_snapshot vk_fault_probe;
		timed_snapshot vk_flush_wait;
		timed_snapshot gpu_event_wait;
		timed_snapshot gpu_readback_wait;
		u64 gpu_readback_bytes;
		timed_snapshot spu_channel_wait;
		timed_snapshot spu_mfc_process;
	};

	inline timed_snapshot read(const timed_counter& counter) noexcept
	{
		return
		{
			counter.count.load(std::memory_order_relaxed),
			counter.ticks.load(std::memory_order_relaxed)
		};
	}

	inline timed_snapshot read_mfc_process() noexcept
	{
		timed_snapshot result{};

		for (const auto& shard : g_ledger.spu_mfc_process)
		{
			const auto value = read(shard.value);
			result.count += value.count;
			result.ticks += value.ticks;
		}

		return result;
	}

	inline ledger_snapshot snapshot() noexcept
	{
		return
		{
			read(g_ledger.renderer_fault_ppu),
			read(g_ledger.renderer_fault_spu),
			read(g_ledger.renderer_fault_other),
			read(g_ledger.vk_fault_probe),
			read(g_ledger.vk_flush_wait),
			read(g_ledger.gpu_event_wait),
			read(g_ledger.gpu_readback_wait),
			g_ledger.gpu_readback_bytes.load(std::memory_order_relaxed),
			read(g_ledger.spu_channel_wait),
			read_mfc_process()
		};
	}

	inline u64 ticks_to_us(u64 ticks, u64 frequency) noexcept
	{
		if (!frequency)
		{
			return 0;
		}

		return (ticks / frequency * 1'000'000) + (ticks % frequency * 1'000'000 / frequency);
	}
}
