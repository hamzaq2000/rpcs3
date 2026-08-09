#pragma once

#include "util/tsc.hpp"
#include "util/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <type_traits>

namespace rsx::coherence_stats
{
	// Neutral CELLJOIN-to-MFC observer bridge. The implementation lives in the
	// Cell observer so texture-cache code does not depend on SPU internals.
	void note_mfc_slack_candidate(u64 serial, u16 member) noexcept;
	void note_mfc_slack_enable_transition() noexcept;

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

	struct ledger
	{
		enabled_flag enabled;

		timed_counter renderer_fault_ppu;
		timed_counter renderer_fault_spu;
		timed_counter renderer_fault_other;
		timed_counter vk_fault_probe;
		timed_counter vk_flush_wait;
		timed_counter vk_flush_consumer_wait;
		timed_counter gpu_event_wait;
		timed_counter gpu_readback_wait;
		std::atomic<u64> gpu_readback_bytes{0};
		timed_counter spu_channel_wait;
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
			note_mfc_slack_enable_transition();
			g_ledger.enabled.value.store(enabled, std::memory_order_relaxed);
		}
	}

	enum class fault_origin : u8
	{
		other,
		ppu,
		spu,
	};

	enum mfc_context_flag : u8
	{
		mfc_context_get = 1 << 0,
		mfc_context_put = 1 << 1,
		mfc_context_list = 1 << 2,
		mfc_context_atomic = 1 << 3,
	};

	enum class mfc_context_source : u8
	{
		none,
		normal_dma,
		list_fastpath,
		atomic,
		raw_spu_proxy,
	};

	enum renderer_fault_path : u8
	{
		renderer_fault_path_texture = 1 << 0,
		renderer_fault_path_zcull = 1 << 1,
		renderer_fault_path_offloader = 1 << 2,
	};

	enum fault_section_relation : u32
	{
		fault_section_access_confirmed = 1 << 0,
		fault_section_access_full = 1 << 1,
		fault_section_access_locked = 1 << 2,
		fault_section_access_contains_confirmed = 1 << 3,
		fault_section_access_contains_full = 1 << 4,
		fault_section_fault_confirmed = 1 << 5,
		fault_section_fault_full = 1 << 6,
		fault_section_fault_locked = 1 << 7,
		fault_section_confirmed_padding = 1 << 8,
		fault_section_native_collateral = 1 << 9,
		fault_section_other_4k_lane = 1 << 10,
		fault_section_chain_or_other = 1 << 11,
		fault_section_unknown_access_width = 1 << 12,
		fault_section_locked_only = 1 << 13,
	};

	enum class fault_timing : u8
	{
		none,
		vk_probe,
		flush_wait,
		gpu_event_wait,
		readback_wait,
	};

	struct mfc_context
	{
		u32 spu_id = 0;
		u32 ea = 0;
		u16 size = 0;
		u8 cmd = 0;
		u8 tag = 0;
		u8 flags = 0;
		mfc_context_source source = mfc_context_source::none;
		bool valid = false;
	};

	inline thread_local mfc_context g_active_mfc_context;

	inline void clear_active_mfc_context() noexcept
	{
		g_active_mfc_context = {};
	}

	inline bool mfc_context_contains(const mfc_context& context, u32 address) noexcept
	{
		return context.valid && context.size && context.ea <= address &&
			static_cast<u64>(address) < static_cast<u64>(context.ea) + context.size;
	}

	class scoped_mfc_context
	{
		mfc_context m_previous{};
		bool m_active = false;

	public:
		scoped_mfc_context(bool enabled, u32 spu_id, u32 ea, u16 size, u8 cmd, u8 tag, u8 flags, mfc_context_source source) noexcept
		{
			if (enabled)
			{
				m_previous = g_active_mfc_context;
				g_active_mfc_context = {spu_id, ea, size, cmd, tag, flags, source, true};
				m_active = true;

				// The access-violation callback observes this TLS state asynchronously
				// on the faulting thread. Keep the protected access between publication
				// and restoration without imposing a hardware fence.
				std::atomic_signal_fence(std::memory_order_seq_cst);
			}
		}

		scoped_mfc_context(const scoped_mfc_context&) = delete;
		scoped_mfc_context& operator=(const scoped_mfc_context&) = delete;

		~scoped_mfc_context()
		{
			if (m_active)
			{
				std::atomic_signal_fence(std::memory_order_seq_cst);
				g_active_mfc_context = m_previous;
			}
		}
	};

	constexpr usz fault_section_capacity = 2;

	struct fault_section_observation
	{
		u64 readback_bytes = 0;
		u64 sync_timestamp = 0;
		u64 last_write_tag = 0;
		u64 rop_timestamp = 0;
		u32 readback_start = umax;
		u32 readback_end = 0;
		u32 full_start = umax;
		u32 full_end = 0;
		u32 confirmed_start = umax;
		u32 confirmed_end = 0;
		u32 locked_start = umax;
		u32 locked_end = 0;
		u32 context = 0;
		u32 relation_flags = 0;
		u8 protection = 0;
		u8 read_flags = 0;
		bool synchronized = false;
		bool has_section = false;
		bool has_readback = false;
	};

	struct fault_event
	{
		u64 sequence = 0;
		u64 frame = 0;
		u64 timestamp_ticks = 0;
		u64 duration_ticks = 0;
		u64 fault_pc = 0;
		u64 fault_esr = 0;
		u64 origin_lr = 0;
		u64 origin_block_hash = 0;
		u32 fault_instruction = 0;
		u32 fault_address = 0;
		u32 origin_id = 0;
		u32 origin_guest_pc = 0;
		fault_origin origin = fault_origin::other;
		u8 fault_access_size = 0;
		bool is_writing = false;
		u8 renderer_path = 0;
		mfc_context mfc{};
		std::array<fault_section_observation, fault_section_capacity> sections{};
		u64 vk_probe_ticks = 0;
		u64 vk_probe_start = 0;
		u64 vk_probe_end = 0;
		u64 flush_wait_ticks = 0;
		u64 flush_wait_start = 0;
		u64 flush_wait_end = 0;
		u64 gpu_event_wait_ticks = 0;
		u64 gpu_event_wait_start = 0;
		u64 gpu_event_wait_end = 0;
		u64 readback_wait_ticks = 0;
		u64 readback_wait_start = 0;
		u64 readback_wait_end = 0;
		u64 readback_bytes = 0;
		u32 readback_start = umax;
		u32 readback_end = 0;
		u32 readback_section_count = 0;
		u32 readback_section_overflow = 0;
		u32 readback_pairing_errors = 0;
		u32 access_start = umax;
		u32 access_end = 0;
		bool mfc_contains_fault = false;
		u32 vk_probe_count = 0;
		u32 flush_wait_count = 0;
		u32 gpu_event_wait_count = 0;
		u32 readback_wait_count = 0;
		u32 readback_count = 0;
		u32 transfer_count = 0;
	};

	static_assert(std::is_trivially_copyable_v<mfc_context>);
	static_assert(std::is_trivially_copyable_v<fault_section_observation>);
	static_assert(std::is_trivially_copyable_v<fault_event>);

	enum class fault_section_class : u8
	{
		none,
		confirmed,
		confirmed_padding,
		locked_only,
		chain_or_other,
	};

	inline fault_section_class classify_section(const fault_section_observation& section) noexcept
	{
		if (!section.has_section)
		{
			return fault_section_class::none;
		}

		if (section.relation_flags & fault_section_access_confirmed)
		{
			return fault_section_class::confirmed;
		}

		if (section.relation_flags & fault_section_confirmed_padding)
		{
			return fault_section_class::confirmed_padding;
		}

		if (section.relation_flags & fault_section_locked_only)
		{
			return fault_section_class::locked_only;
		}

		return fault_section_class::chain_or_other;
	}

	inline u8 section_semantic_key(const fault_section_observation& section) noexcept
	{
		u8 result = static_cast<u8>(classify_section(section));
		result |= section.relation_flags & fault_section_native_collateral ? 1 << 4 : 0;
		result |= section.relation_flags & fault_section_other_4k_lane ? 1 << 5 : 0;
		result |= section.relation_flags & fault_section_unknown_access_width ? 1 << 6 : 0;
		return result;
	}

	struct fault_section_site_key
	{
		u32 context = 0;
		u8 relation = 0;
		u8 protection = 0;
		u8 read_flags = 0;
		bool synchronized = false;

		bool operator==(const fault_section_site_key&) const = default;
	};

	inline bool section_site_key_less(const fault_section_site_key& lhs, const fault_section_site_key& rhs) noexcept
	{
		if (lhs.relation != rhs.relation) return lhs.relation < rhs.relation;
		if (lhs.context != rhs.context) return lhs.context < rhs.context;
		if (lhs.protection != rhs.protection) return lhs.protection < rhs.protection;
		if (lhs.read_flags != rhs.read_flags) return lhs.read_flags < rhs.read_flags;
		return lhs.synchronized < rhs.synchronized;
	}

	inline std::array<fault_section_site_key, fault_section_capacity> sorted_section_semantic_keys(const fault_event& event) noexcept
	{
		std::array<fault_section_site_key, fault_section_capacity> result{};

		for (usz i = 0; i < fault_section_capacity; i++)
		{
			const auto& section = event.sections[i];
			result[i] = {section.context, section_semantic_key(section), section.protection, section.read_flags, section.synchronized};
		}

		std::sort(result.begin(), result.end(), section_site_key_less);
		return result;
	}

	inline u8 mfc_command_family(u8 command) noexcept
	{
		return command & 0xf0;
	}

	inline u8 readback_count_bucket(u32 count) noexcept
	{
		return static_cast<u8>(std::min<u32>(count, 3));
	}

	inline bool semantic_site_matches(const fault_event& lhs, const fault_event& rhs) noexcept
	{
		return lhs.origin == rhs.origin &&
			lhs.origin_guest_pc == rhs.origin_guest_pc &&
			lhs.fault_instruction == rhs.fault_instruction &&
			lhs.is_writing == rhs.is_writing &&
			lhs.renderer_path == rhs.renderer_path &&
			lhs.mfc.valid == rhs.mfc.valid &&
			lhs.mfc.source == rhs.mfc.source &&
			lhs.mfc_contains_fault == rhs.mfc_contains_fault &&
			mfc_command_family(lhs.mfc.cmd) == mfc_command_family(rhs.mfc.cmd) &&
			lhs.mfc.flags == rhs.mfc.flags &&
			readback_count_bucket(lhs.readback_count) == readback_count_bucket(rhs.readback_count) &&
			(lhs.readback_section_overflow != 0) == (rhs.readback_section_overflow != 0) &&
			sorted_section_semantic_keys(lhs) == sorted_section_semantic_keys(rhs);
	}

	inline u32 stored_section_count(const fault_event& event) noexcept
	{
		u32 result = 0;

		for (const auto& section : event.sections)
		{
			result += section.has_section;
		}

		return result;
	}

	inline u32 unmatched_section_count(const fault_event& event) noexcept
	{
		return event.readback_section_count > event.readback_count ?
			event.readback_section_count - event.readback_count : 0;
	}

	inline u32 unmatched_readback_count(const fault_event& event) noexcept
	{
		return event.readback_count > event.readback_section_count ?
			event.readback_count - event.readback_section_count : 0;
	}

	inline u32 readbacks_outside_locked_count(const fault_event& event) noexcept
	{
		u32 result = 0;

		for (const auto& section : event.sections)
		{
			result += section.has_section && section.has_readback && section.readback_bytes &&
				(section.readback_start < section.locked_start || section.readback_end > section.locked_end);
		}

		return result;
	}

	constexpr usz fault_ring_capacity = 4096;
	static_assert(std::has_single_bit(fault_ring_capacity));

	struct alignas(coherence_cache_line_size) fault_event_slot
	{
		std::atomic<u64> sequence{0};
		fault_event event{};
	};

	// Bounded MPSC queue. Producers own a slot before touching its plain POD
	// payload, publish with release, and the sole RSX consumer recycles it only
	// after an acquire read. A full queue drops the event instead of overwriting
	// storage that the consumer may be reading.
	class fault_event_ring
	{
		std::array<fault_event_slot, fault_ring_capacity> m_slots{};
		alignas(coherence_cache_line_size) std::atomic<u64> m_enqueue_pos{0};
		alignas(coherence_cache_line_size) u64 m_dequeue_pos = 0;
		alignas(coherence_cache_line_size) std::atomic<u64> m_dropped{0};

	public:
		fault_event_ring() noexcept
		{
			reset();
		}

		void reset() noexcept
		{
			m_enqueue_pos.store(0, std::memory_order_relaxed);
			m_dequeue_pos = 0;
			m_dropped.store(0, std::memory_order_relaxed);

			for (usz i = 0; i < fault_ring_capacity; i++)
			{
				m_slots[i].sequence.store(i, std::memory_order_relaxed);
			}
		}

		bool try_push(const fault_event& event) noexcept
		{
			u64 pos = m_enqueue_pos.load(std::memory_order_relaxed);
			fault_event_slot* slot = nullptr;

			for (;;)
			{
				slot = &m_slots[pos & (fault_ring_capacity - 1)];
				const u64 sequence = slot->sequence.load(std::memory_order_acquire);
				const s64 difference = static_cast<s64>(sequence - pos);

				if (!difference)
				{
					if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
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
					pos = m_enqueue_pos.load(std::memory_order_relaxed);
				}
			}

			slot->event = event;
			slot->event.sequence = pos + 1;
			slot->sequence.store(pos + 1, std::memory_order_release);
			return true;
		}

		bool try_pop(fault_event& event) noexcept
		{
			const u64 pos = m_dequeue_pos;
			auto& slot = m_slots[pos & (fault_ring_capacity - 1)];

			if (slot.sequence.load(std::memory_order_acquire) != pos + 1)
			{
				return false;
			}

			event = slot.event;
			slot.sequence.store(pos + fault_ring_capacity, std::memory_order_release);
			m_dequeue_pos = pos + 1;
			return true;
		}

		u64 dropped() const noexcept
		{
			return m_dropped.load(std::memory_order_relaxed);
		}
	};

	inline fault_event_ring g_fault_events;
	inline std::atomic<u64> g_fault_frame{0};
	inline std::atomic<u64> g_fault_epoch{0};
	inline thread_local fault_event* g_active_fault_event = nullptr;

	class scoped_fault_event
	{
		fault_event m_event{};
		fault_event* m_previous = nullptr;
		bool m_active = false;

	public:
		scoped_fault_event(u32 address, bool is_writing, u64 fault_pc, u32 fault_instruction, u64 fault_esr, u8 fault_access_size,
			fault_origin origin, u32 origin_id, u32 origin_guest_pc, u64 origin_lr, u64 origin_block_hash) noexcept
		{
			if (is_enabled())
			{
				m_event.frame = g_fault_frame.load(std::memory_order_relaxed);
				m_event.timestamp_ticks = utils::get_tsc();
				m_event.fault_pc = fault_pc;
				m_event.fault_instruction = fault_instruction;
				m_event.fault_esr = fault_esr;
				m_event.fault_access_size = fault_access_size;
				m_event.fault_address = address;
				m_event.is_writing = is_writing;
				m_event.origin = origin;
				m_event.origin_id = origin_id;
				m_event.origin_guest_pc = origin_guest_pc;
				m_event.origin_lr = origin_lr;
				m_event.origin_block_hash = origin_block_hash;
				m_event.mfc = g_active_mfc_context;
				m_event.mfc_contains_fault = mfc_context_contains(m_event.mfc, address);
				m_previous = g_active_fault_event;
				g_active_fault_event = &m_event;
				m_active = true;
			}
		}

		scoped_fault_event(const scoped_fault_event&) = delete;
		scoped_fault_event& operator=(const scoped_fault_event&) = delete;

		~scoped_fault_event()
		{
			if (m_active)
			{
				g_active_fault_event = m_previous;
			}
		}

		void finish(bool handled) noexcept
		{
			if (!m_active)
			{
				return;
			}

			m_event.duration_ticks = utils::get_tsc() - m_event.timestamp_ticks;

			if (handled)
			{
				g_fault_events.try_push(m_event);
			}

			g_active_fault_event = m_previous;
			m_active = false;
		}
	};

	inline void mark_renderer_fault_path(renderer_fault_path path) noexcept
	{
		if (g_active_fault_event)
		{
			g_active_fault_event->renderer_path |= path;
		}
	}

	inline void record_fault_timing(fault_timing kind, u64 start, u64 end) noexcept
	{
		if (!g_active_fault_event)
		{
			return;
		}

		switch (kind)
		{
		case fault_timing::vk_probe:
			g_active_fault_event->vk_probe_ticks += end - start;
			g_active_fault_event->vk_probe_start = g_active_fault_event->vk_probe_start ? std::min(g_active_fault_event->vk_probe_start, start) : start;
			g_active_fault_event->vk_probe_end = std::max(g_active_fault_event->vk_probe_end, end);
			g_active_fault_event->vk_probe_count++;
			break;
		case fault_timing::flush_wait:
			g_active_fault_event->flush_wait_ticks += end - start;
			g_active_fault_event->flush_wait_start = g_active_fault_event->flush_wait_start ? std::min(g_active_fault_event->flush_wait_start, start) : start;
			g_active_fault_event->flush_wait_end = std::max(g_active_fault_event->flush_wait_end, end);
			g_active_fault_event->flush_wait_count++;
			break;
		case fault_timing::gpu_event_wait:
			g_active_fault_event->gpu_event_wait_ticks += end - start;
			g_active_fault_event->gpu_event_wait_start = g_active_fault_event->gpu_event_wait_start ? std::min(g_active_fault_event->gpu_event_wait_start, start) : start;
			g_active_fault_event->gpu_event_wait_end = std::max(g_active_fault_event->gpu_event_wait_end, end);
			g_active_fault_event->gpu_event_wait_count++;
			break;
		case fault_timing::readback_wait:
			g_active_fault_event->readback_wait_ticks += end - start;
			g_active_fault_event->readback_wait_start = g_active_fault_event->readback_wait_start ? std::min(g_active_fault_event->readback_wait_start, start) : start;
			g_active_fault_event->readback_wait_end = std::max(g_active_fault_event->readback_wait_end, end);
			g_active_fault_event->readback_wait_count++;
			break;
		case fault_timing::none:
			break;
		}
	}

	inline void advance_fault_frame() noexcept
	{
		g_fault_frame.fetch_add(1, std::memory_order_relaxed);
	}

	inline void reset(timed_counter& counter) noexcept
	{
		counter.count.store(0, std::memory_order_relaxed);
		counter.ticks.store(0, std::memory_order_relaxed);
	}

	inline void reset() noexcept
	{
		set_enabled(false);
		g_fault_epoch.fetch_add(1, std::memory_order_relaxed);
		g_fault_frame.store(0, std::memory_order_relaxed);
		g_fault_events.reset();
		reset(g_ledger.renderer_fault_ppu);
		reset(g_ledger.renderer_fault_spu);
		reset(g_ledger.renderer_fault_other);
		reset(g_ledger.vk_fault_probe);
		reset(g_ledger.vk_flush_wait);
		reset(g_ledger.vk_flush_consumer_wait);
		reset(g_ledger.gpu_event_wait);
		reset(g_ledger.gpu_readback_wait);
		g_ledger.gpu_readback_bytes.store(0, std::memory_order_relaxed);
		reset(g_ledger.spu_channel_wait);

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
		fault_timing m_fault_timing = fault_timing::none;
		u64 m_start = 0;

	public:
		explicit scoped_timer(timed_counter& primary, timed_counter* secondary = nullptr, fault_timing fault_timing = fault_timing::none) noexcept
		{
			if (is_enabled())
			{
				m_primary = &primary;
				m_secondary = secondary;
				m_fault_timing = fault_timing;
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

			record_fault_timing(m_fault_timing, m_start, m_start + elapsed);
		}

		void cancel() noexcept
		{
			m_primary = nullptr;
			m_secondary = nullptr;
			m_fault_timing = fault_timing::none;
		}
	};

	inline void record_readback_range(u32 start, u64 bytes) noexcept
	{
		if (is_enabled())
		{
			g_ledger.gpu_readback_bytes.fetch_add(bytes, std::memory_order_relaxed);

			if (g_active_fault_event)
			{
				auto& event = *g_active_fault_event;
				const u32 ordinal = event.readback_count++;
				event.readback_pairing_errors += ordinal >= event.readback_section_count;
				event.readback_bytes += bytes;

				if (bytes)
				{
					const u32 end = static_cast<u32>(std::min<u64>(umax, static_cast<u64>(start) + bytes - 1));
					event.readback_start = std::min(event.readback_start, start);
					event.readback_end = std::max(event.readback_end, end);

					if (ordinal < fault_section_capacity && event.sections[ordinal].has_section)
					{
						auto& observation = event.sections[ordinal];
						observation.readback_start = start;
						observation.readback_end = end;
						observation.readback_bytes = bytes;
						observation.has_readback = true;
					}
				}
				else if (ordinal < fault_section_capacity && event.sections[ordinal].has_section)
				{
					auto& observation = event.sections[ordinal];
					observation.readback_start = start;
					observation.readback_end = start;
					observation.has_readback = true;
				}
			}
		}
	}

	inline void record_transfer_sections(u32 count) noexcept
	{
		if (is_enabled() && g_active_fault_event)
		{
			g_active_fault_event->transfer_count += count;
		}
	}

	inline void record_readback_section(u32 full_start, u32 full_end, u32 confirmed_start, u32 confirmed_end,
		u32 locked_start, u32 locked_end, u32 context, u8 protection, u8 read_flags, bool synchronized,
		u64 sync_timestamp, u64 last_write_tag, u64 rop_timestamp) noexcept
	{
		if (!g_active_fault_event)
		{
			return;
		}

		auto& event = *g_active_fault_event;
		const u32 ordinal = event.readback_section_count++;

		if (ordinal >= fault_section_capacity)
		{
			event.readback_section_overflow++;
			return;
		}

		auto& section = event.sections[ordinal];
		section.has_section = true;

		const u32 access_start = event.mfc.valid && event.mfc.size ? event.mfc.ea : event.fault_address;
		const u64 access_size = event.mfc.valid && event.mfc.size ? event.mfc.size : std::max<u8>(event.fault_access_size, 1);
		const u32 access_end = static_cast<u32>(std::min<u64>(umax, static_cast<u64>(access_start) + access_size - 1));
		event.access_start = std::min(event.access_start, access_start);
		event.access_end = std::max(event.access_end, access_end);

		const auto overlaps = [](u32 lhs_start, u32 lhs_end, u32 rhs_start, u32 rhs_end) noexcept
		{
			return lhs_start <= rhs_end && rhs_start <= lhs_end;
		};
		const auto contains = [](u32 outer_start, u32 outer_end, u32 inner_start, u32 inner_end) noexcept
		{
			return outer_start <= inner_start && inner_end <= outer_end;
		};

		const bool access_confirmed = overlaps(access_start, access_end, confirmed_start, confirmed_end);
		const bool access_full = overlaps(access_start, access_end, full_start, full_end);
		const bool access_locked = overlaps(access_start, access_end, locked_start, locked_end);
		const bool fault_confirmed = confirmed_start <= event.fault_address && event.fault_address <= confirmed_end;
		const bool fault_full = full_start <= event.fault_address && event.fault_address <= full_end;
		const bool fault_locked = locked_start <= event.fault_address && event.fault_address <= locked_end;
		const u32 fault_lane_start = event.fault_address & -0x1000;
		const u32 fault_lane_end = fault_lane_start + 0xfff;

		section.relation_flags |= access_confirmed ? fault_section_access_confirmed : 0;
		section.relation_flags |= access_full ? fault_section_access_full : 0;
		section.relation_flags |= access_locked ? fault_section_access_locked : 0;
		section.relation_flags |= contains(access_start, access_end, confirmed_start, confirmed_end) ? fault_section_access_contains_confirmed : 0;
		section.relation_flags |= contains(access_start, access_end, full_start, full_end) ? fault_section_access_contains_full : 0;
		section.relation_flags |= fault_confirmed ? fault_section_fault_confirmed : 0;
		section.relation_flags |= fault_full ? fault_section_fault_full : 0;
		section.relation_flags |= fault_locked ? fault_section_fault_locked : 0;
		section.relation_flags |= !event.mfc.valid && !event.fault_access_size ? fault_section_unknown_access_width : 0;

		if (access_confirmed)
		{
			// Exact logical overlap; no collateral-class bit is needed.
		}
		else if (access_full)
		{
			section.relation_flags |= fault_section_confirmed_padding;
		}
		else if (access_locked)
		{
			// locked_range is host-page expanded (16 KiB on Apple silicon), so
			// this is native-page collateral outside the section's logical range.
			section.relation_flags |= fault_section_locked_only | fault_section_native_collateral;

			if (!overlaps(full_start, full_end, fault_lane_start, fault_lane_end))
			{
				section.relation_flags |= fault_section_other_4k_lane;
			}
		}
		else
		{
			section.relation_flags |= fault_section_chain_or_other;
		}

		section.full_start = full_start;
		section.full_end = full_end;
		section.confirmed_start = confirmed_start;
		section.confirmed_end = confirmed_end;
		section.locked_start = locked_start;
		section.locked_end = locked_end;
		section.context = context;
		section.protection = protection;
		section.read_flags = read_flags;
		section.synchronized = synchronized;
		section.sync_timestamp = sync_timestamp;
		section.last_write_tag = last_write_tag;
		section.rop_timestamp = rop_timestamp;
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
		timed_snapshot vk_flush_consumer_wait;
		timed_snapshot gpu_event_wait;
		timed_snapshot gpu_readback_wait;
		u64 gpu_readback_bytes;
		timed_snapshot spu_channel_wait;
	};

	inline timed_snapshot read(const timed_counter& counter) noexcept
	{
		return
		{
			counter.count.load(std::memory_order_relaxed),
			counter.ticks.load(std::memory_order_relaxed)
		};
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
			read(g_ledger.vk_flush_consumer_wait),
			read(g_ledger.gpu_event_wait),
			read(g_ledger.gpu_readback_wait),
			g_ledger.gpu_readback_bytes.load(std::memory_order_relaxed),
			read(g_ledger.spu_channel_wait)
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
