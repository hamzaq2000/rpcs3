#include "stdafx.h"
#include "cell_access_coherence.h"

namespace rsx::cell_access
{
	ownership_directory g_ownership_directory;

	ownership_directory::renderer_lifetime_session::renderer_lifetime_session(ownership_directory& owner) noexcept
		: m_owner(&owner)
		, m_lock(owner.m_renderer_lifetime_mutex)
		, m_renderer(owner.m_renderer)
		, m_epoch(owner.m_lifetime_epoch.load(std::memory_order_acquire))
	{
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

	ownership_directory::renderer_lifetime_session ownership_directory::begin_renderer_lifetime_session() noexcept
	{
		return renderer_lifetime_session(*this);
	}

	void ownership_directory::publish_renderer_lifetime(rsx::thread* renderer) noexcept
	{
		ensure(renderer);
		std::unique_lock lock(m_renderer_lifetime_mutex);
		ensure(!m_renderer || m_renderer == renderer);
		m_renderer = renderer;
	}

	void ownership_directory::unpublish_renderer_lifetime(rsx::thread* renderer) noexcept
	{
		std::unique_lock lock(m_renderer_lifetime_mutex);
		if (m_renderer == renderer)
		{
			m_renderer = nullptr;
		}
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
		std::unique_lock lifetime_lock(m_renderer_lifetime_mutex);
		ensure(!m_renderer);
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
		for (auto& result : m_ready_get_results)
		{
			result.store(0, std::memory_order_relaxed);
		}
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

	void ownership_directory::record_ready_get_result(ready_get_result result) noexcept
	{
		const usz index = static_cast<usz>(result);
		ensure(index < m_ready_get_results.size());
		m_ready_get_results[index].fetch_add(1, std::memory_order_relaxed);
	}

	u64 ownership_directory::ready_get_result_count(ready_get_result result) const noexcept
	{
		const usz index = static_cast<usz>(result);
		ensure(index < m_ready_get_results.size());
		return m_ready_get_results[index].load(std::memory_order_relaxed);
	}
}
