#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "Emu/RSX/Common/cell_access_coherence.h"
#include "Emu/RSX/Common/texture_cache_utils.h"
#include "Emu/RSX/Host/MM.h"
#include "Emu/Cell/SPUThread.h"

namespace
{
	using namespace rsx::cell_access;

	utils::address_range32 range(u32 start, u32 length)
	{
		return utils::address_range32::start_length(start, length);
	}

	class scoped_buffered_section_vm_state
	{
		utils::address_range32 m_range;
		utils::shm m_memory;
		std::string m_error;
		bool m_mapped = false;

	public:
		explicit scoped_buffered_section_vm_state(const utils::address_range32& test_range)
			: m_range(test_range)
			, m_memory(test_range.length())
		{
			rsx::mm_flush();
			auto [mapped, error] = m_memory.map_critical(
				vm::base(m_range.start), utils::protection::no);
			m_mapped = mapped == vm::base(m_range.start);
			m_error = std::move(error);
			g_ownership_directory.begin_renderer_lifetime_quiescent();
		}

		scoped_buffered_section_vm_state(const scoped_buffered_section_vm_state&) = delete;
		scoped_buffered_section_vm_state& operator=(const scoped_buffered_section_vm_state&) = delete;

		~scoped_buffered_section_vm_state()
		{
			rsx::mm_flush();
			if (m_mapped)
			{
				// Restore the reserved guest window (or an inaccessible remnant on
				// POSIX) without ever dereferencing the test mapping.
				m_memory.unmap_critical(vm::base(m_range.start));
			}
			g_ownership_directory.begin_renderer_lifetime_quiescent();
		}

		bool mapped() const
		{
			return m_mapped;
		}

		const std::string& error() const
		{
			return m_error;
		}
	};

	void expect_texture_owner_count(const utils::address_range32& owner_range, u16 expected)
	{
		ASSERT_TRUE(owner_range.valid());
		const u32 first = owner_range.start & ~(summary_granule_size - 1);
		for (u64 address = first; address <= owner_range.end; address += summary_granule_size)
		{
			SCOPED_TRACE(static_cast<u32>(address));
			EXPECT_EQ(g_ownership_directory.count_at(static_cast<u32>(address)), expected);
		}
	}
}

TEST(RsxCellAccessDirectory, AddsRemovesAndMovesNoAccessOwners)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto first = range(0x4000, 0x4000);
	const auto second = range(0x8000, 0x4000);

	{
		auto mutation = directory->begin_mutation({}, false);
		EXPECT_EQ(directory->probe(0x4000, 1), page_probe_result::unstable);
		mutation.commit(first, true);
	}

	EXPECT_EQ(directory->probe(0x4000, 0x4000), page_probe_result::maybe_texture);
	EXPECT_EQ(directory->count_at(0x4000), 1u);

	{
		auto mutation = directory->begin_mutation(first, true);
		mutation.commit(second, true);
	}

	EXPECT_EQ(directory->probe(0x4000, 1), page_probe_result::clear);
	EXPECT_EQ(directory->probe(0x8000, 1), page_probe_result::maybe_texture);

	{
		auto mutation = directory->begin_mutation(second, true);
		mutation.commit(second, false);
	}

	EXPECT_EQ(directory->probe(0x8000, 1), page_probe_result::clear);
	EXPECT_EQ(directory->sequence() & 1, 0u);
}

TEST(RsxCellAccessDirectory, KeepsSharedGranuleOwnedUntilLastSectionLeaves)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto left = range(0x1000, 0x1000);
	const auto right = range(0x3000, 0x1000);

	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(left, true);
	}
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(right, true);
	}

	EXPECT_EQ(directory->count_at(0x0000), 2u);
	{
		auto mutation = directory->begin_mutation(left, true);
		mutation.commit(left, false);
	}
	EXPECT_EQ(directory->count_at(0x0000), 1u);
	EXPECT_EQ(directory->probe(0x2000, 1), page_probe_result::maybe_texture);

	{
		auto mutation = directory->begin_mutation(right, true);
		mutation.commit(right, false);
	}
	EXPECT_EQ(directory->probe(0x2000, 1), page_probe_result::clear);
}

TEST(RsxCellAccessDirectory, ProbesBothGranulesOfAnUnalignedMfcRange)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto second_granule = range(0x4000, 0x4000);
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(second_granule, true);
	}

	EXPECT_EQ(directory->probe(0x3ff0, 0x20), page_probe_result::maybe_texture);
	EXPECT_EQ(directory->probe(0x3f00, 0x100), page_probe_result::clear);
	EXPECT_EQ(directory->probe(0x3ff0, 0), page_probe_result::invalid);
	EXPECT_EQ(directory->probe(0xfffffff0, 0x10), page_probe_result::clear);
	EXPECT_EQ(directory->probe(0xfffffff0, 0x20), page_probe_result::invalid);
	EXPECT_EQ(directory->probe(0, summary_granule_size + 1), page_probe_result::invalid);
}

TEST(RsxCellAccessDirectory, PoisonsInsteadOfUnderflowing)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto owner = range(0x10000, 0x4000);

	{
		auto mutation = directory->begin_mutation(owner, true);
		mutation.commit(owner, false);
	}

	EXPECT_EQ(directory->count_at(0x10000), poisoned_owner_count);
	EXPECT_EQ(directory->probe(0x10000, 1), page_probe_result::poisoned);
	EXPECT_EQ(directory->validation_snapshot().underflows, 1u);
}

TEST(RsxCellAccessDirectory, ExpandsAcrossFourGranulesAndClearsTheTopOfEaSpace)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto one_granule = range(0x4000, 0x4000);
	const auto four_granules = range(0x4000, 0x10000);
	const auto top_granule = range(0xffffc000, 0x4000);

	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(one_granule, true);
	}
	{
		auto mutation = directory->begin_mutation(one_granule, true);
		mutation.commit(four_granules, true);
	}

	for (u32 address = 0x4000; address <= 0x10000; address += 0x4000)
	{
		EXPECT_EQ(directory->count_at(address), 1u);
	}

	{
		auto mutation = directory->begin_mutation(four_granules, true);
		mutation.commit(four_granules, false);
	}
	for (u32 address = 0x4000; address <= 0x10000; address += 0x4000)
	{
		EXPECT_EQ(directory->probe(address, 1), page_probe_result::clear);
	}

	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(top_granule, true);
	}
	EXPECT_EQ(directory->probe(0xffffffff, 1), page_probe_result::maybe_texture);
	{
		auto mutation = directory->begin_mutation(top_granule, true);
		mutation.commit(top_granule, false);
	}
	EXPECT_EQ(directory->probe(0xffffffff, 1), page_probe_result::clear);
}

TEST(RsxCellAccessDirectory, IgnoresTransitionsWithoutNoAccessOwnership)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto owner = range(0x20000, 0x4000);

	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(owner, false);
	}

	EXPECT_EQ(directory->count_at(owner.start), 0u);
	EXPECT_EQ(directory->probe(owner.start, 1), page_probe_result::clear);
}

TEST(RsxCellAccessDirectory, AbandonedMutationPublishesAnEvenPoisonedState)
{
	auto directory = std::make_unique<ownership_directory>();
	{
		auto mutation = directory->begin_mutation({}, false);
		EXPECT_EQ(directory->probe(0, 1), page_probe_result::unstable);
	}

	EXPECT_EQ(directory->sequence() & 1, 0u);
	EXPECT_EQ(directory->probe(0, 1), page_probe_result::poisoned);
	EXPECT_EQ(directory->validation_snapshot().abandoned_mutations, 1u);
	EXPECT_TRUE(directory->validation_snapshot().globally_poisoned);
}

TEST(RsxCellAccessDirectory, RecountAndFaultValidationExposeSafetyFailures)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto owner = range(0x30000, 0x4000);
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(owner, true);
	}

	auto recount_session = directory->begin_recount();
	recount_session.add(owner);
	auto recount = recount_session.finish();
	EXPECT_EQ(recount.mismatched_granules, 0u);
	EXPECT_EQ(recount.expected_refs, 1u);
	EXPECT_EQ(recount.observed_refs, 1u);

	directory->record_read_fault_probe(page_probe_result::maybe_texture, true);
	directory->record_read_fault_probe(page_probe_result::clear, true);
	directory->record_read_fault_probe(page_probe_result::maybe_texture, false);
	directory->record_recount_cache_busy();
	directory->record_recount_duration(5);
	directory->record_recount_duration(9);
	const auto validation = directory->validation_snapshot();
	EXPECT_EQ(validation.read_fault_probes, 3u);
	EXPECT_EQ(validation.read_faults_handled, 2u);
	EXPECT_EQ(validation.handled_maybe_texture, 1u);
	EXPECT_EQ(validation.handled_clear, 1u);
	EXPECT_EQ(validation.unhandled_maybe_texture, 1u);
	EXPECT_EQ(validation.recount_cache_busy, 1u);
	EXPECT_EQ(validation.recount_total_us, 14u);
	EXPECT_EQ(validation.recount_max_us, 9u);

	auto missing_recount = directory->begin_recount();
	recount = missing_recount.finish();
	EXPECT_EQ(recount.mismatched_granules, 1u);
	EXPECT_EQ(recount.excess_refs, 1u);
	EXPECT_TRUE(recount.globally_poisoned);
	EXPECT_EQ(directory->probe(owner.start, 1), page_probe_result::poisoned);
}

TEST(RsxCellAccessDirectory, QuiescentRendererLifetimeResetClearsEveryOwnershipPlane)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto texture = range(0x40000, 0x4000);
	const auto external = range(0x80000, 0x4000);
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(texture, true);
	}
	{
		auto mutation = directory->begin_nontexture_mutation({}, false);
		mutation.commit(external, true);
	}

	const u64 old_epoch = directory->lifetime_epoch();
	const u64 new_epoch = directory->begin_renderer_lifetime_quiescent();
	EXPECT_NE(new_epoch, old_epoch);
	EXPECT_EQ(directory->count_at(texture.start), 0u);
	EXPECT_EQ(directory->nontexture_count_at(external.start), 0u);
	EXPECT_EQ(directory->probe(texture.start, 1), page_probe_result::clear);

	auto stable = directory->begin_stable_session();
	const auto snapshot = stable.probe(external.start, 1);
	EXPECT_EQ(snapshot.texture, page_probe_result::clear);
	EXPECT_FALSE(snapshot.maybe_nontexture);
	EXPECT_EQ(snapshot.lifetime_epoch, new_epoch);
}

TEST(RsxCellAccessDirectory, NontextureHandoffRequiresCompleteTextureCoverage)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto prelock = range(0x10000, 0x8000);
	const auto partial_texture = range(0x10000, 0x4000);
	{
		auto mutation = directory->begin_nontexture_mutation({}, false);
		mutation.commit(prelock, true);
	}
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(partial_texture, true);
	}

	EXPECT_FALSE(directory->handoff_nontexture_to_texture(prelock, partial_texture));
	EXPECT_EQ(directory->nontexture_count_at(0x10000), 1u);
	EXPECT_EQ(directory->nontexture_count_at(0x14000), 1u);
	EXPECT_TRUE(directory->validation_snapshot().globally_poisoned);

	directory->begin_renderer_lifetime_quiescent();
	{
		auto mutation = directory->begin_nontexture_mutation({}, false);
		mutation.commit(prelock, true);
	}
	{
		auto mutation = directory->begin_mutation({}, false);
		mutation.commit(prelock, true);
	}

	EXPECT_TRUE(directory->handoff_nontexture_to_texture(prelock, prelock));
	EXPECT_EQ(directory->nontexture_count_at(0x10000), 0u);
	EXPECT_EQ(directory->nontexture_count_at(0x14000), 0u);
	EXPECT_EQ(directory->count_at(0x10000), 1u);
	EXPECT_EQ(directory->count_at(0x14000), 1u);
}

TEST(RsxCellAccessDirectory, StableSessionPinsOwnershipAndEpoch)
{
	auto directory = std::make_unique<ownership_directory>();
	const auto external = range(0x20000, 0x4000);
	const u64 epoch = directory->begin_renderer_lifetime_quiescent();
	std::atomic<bool> started = false;
	std::atomic<bool> completed = false;
	std::thread writer;
	{
		auto stable = directory->begin_stable_session();
		EXPECT_EQ(stable.probe(external.start, 1).lifetime_epoch, epoch);
		writer = std::thread([&]
		{
			started.store(true, std::memory_order_release);
			auto mutation = directory->begin_nontexture_mutation({}, false);
			mutation.commit(external, true);
			completed.store(true, std::memory_order_release);
		});

		while (!started.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		EXPECT_FALSE(completed.load(std::memory_order_acquire));
	}

	writer.join();
	EXPECT_TRUE(completed.load(std::memory_order_acquire));
	auto stable = directory->begin_stable_session();
	EXPECT_TRUE(stable.probe(external.start, 1).maybe_nontexture);
}

TEST(RsxCellAccessDirectory, CellBackingReceiptRequiresCurrentGenerationAndCoversOnlyLogicalIntersection)
{
	cell_backing_receipt receipt;
	const auto section = range(0x20000100, 0x180);
	const auto request = range(0x20000080, 0x200);
	const auto covered = request.get_intersect(section);
	constexpr u64 epoch = 9;
	constexpr u64 generation = 27;

	receipt.publish(section, generation, epoch);
	EXPECT_TRUE(receipt.matches(covered, epoch));
	EXPECT_TRUE(receipt.matches_generation(generation));
	EXPECT_FALSE(receipt.matches(request, epoch));
	EXPECT_FALSE(receipt.matches(covered, epoch + 1));
	EXPECT_FALSE(receipt.matches_generation(generation + 1));

	receipt.clear();
	EXPECT_FALSE(receipt.matches(covered, epoch));
	EXPECT_FALSE(receipt.matches_generation(generation));
}

TEST(RsxCellAccessDirectory, ReadyGetRangeEligibilityRejectsUnpinnableGuestRanges)
{
	EXPECT_TRUE(spu_is_ready_cell_backing_range_eligible(0x10020, 0x200));
	EXPECT_TRUE(spu_is_ready_cell_backing_range_eligible(0x1c000, 0x4000));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(0x1ff00, 0x200));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(rsx::constants::local_mem_base, 0x80));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(RAW_SPU_BASE_ADDR, 0x80));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(0xfffffff0, 0x20));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(0x10000, 0));
	EXPECT_FALSE(spu_is_ready_cell_backing_range_eligible(0x10000, 0x4001));
}

TEST(RsxCellAccessDirectory, RendererLifetimeSessionPinsPublishedBackendAgainstTeardown)
{
	auto directory = std::make_unique<ownership_directory>();
	directory->begin_renderer_lifetime_quiescent();
	auto* renderer = reinterpret_cast<rsx::thread*>(static_cast<uintptr_t>(1));
	directory->publish_renderer_lifetime(renderer);

	std::atomic<bool> started = false;
	std::atomic<bool> completed = false;
	std::thread teardown;
	{
		auto lifetime = directory->begin_renderer_lifetime_session();
		EXPECT_EQ(lifetime.renderer(), renderer);
		EXPECT_EQ(lifetime.epoch(), directory->lifetime_epoch());
		teardown = std::thread([&]
		{
			started.store(true, std::memory_order_release);
			directory->unpublish_renderer_lifetime(renderer);
			completed.store(true, std::memory_order_release);
		});

		while (!started.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		EXPECT_FALSE(completed.load(std::memory_order_acquire));
	}

	teardown.join();
	EXPECT_TRUE(completed.load(std::memory_order_acquire));
	auto lifetime = directory->begin_renderer_lifetime_session();
	EXPECT_EQ(lifetime.renderer(), nullptr);
}

TEST(RsxCellAccessDirectory, IntrusiveReceiptPinBlocksRetirementUntilExplicitClear)
{
	class test_resource final : public rsx::ref_counted
	{
	};

	test_resource resource;
	resource.add_ref(); // Existing NO/locked section ref.
	intrusive_lifetime_pin<test_resource> receipt_pin;
	receipt_pin.reset(&resource);
	resource.release(); // Simulate discard releasing the section ref.

	std::atomic<u32> phase = 0;
	bool retained_during_retirement = false;
	bool reusable_after_clear = false;
	std::thread retirement([&]
	{
		while (phase.load(std::memory_order_acquire) < 1)
		{
			std::this_thread::yield();
		}
		retained_during_retirement = resource.has_refs();
		phase.store(2, std::memory_order_release);

		while (phase.load(std::memory_order_acquire) < 3)
		{
			std::this_thread::yield();
		}
		reusable_after_clear = !resource.has_refs();
	});

	phase.store(1, std::memory_order_release);
	while (phase.load(std::memory_order_acquire) < 2)
	{
		std::this_thread::yield();
	}
	receipt_pin.reset();
	phase.store(3, std::memory_order_release);
	retirement.join();

	EXPECT_TRUE(retained_during_retirement);
	EXPECT_TRUE(reusable_after_clear);
}

TEST(RsxCellAccessDirectory, BufferedSectionConfirmedRangeExpansionTracksRealLifecycle)
{
	constexpr u32 test_address = 0x60000000;
	const auto full_range = range(test_address, 0x40000);
	scoped_buffered_section_vm_state vm_state(full_range);
	ASSERT_TRUE(vm_state.mapped()) << vm_state.error();

	rsx::buffered_section section;
	section.reset(full_range);
	EXPECT_EQ(section.get_protection(), utils::protection::rw);
	EXPECT_FALSE(section.is_locked());
	EXPECT_EQ(g_ownership_directory.count_at(full_range.start), 0u);

	section.protect(utils::protection::no, {0x1000, 0x1000});
	const auto initial_locked = section.get_locked_range();
	EXPECT_EQ(section.get_protection(), utils::protection::no);
	EXPECT_TRUE(section.is_locked());
	expect_texture_owner_count(initial_locked, 1);
	EXPECT_EQ(g_ownership_directory.count_at(full_range.end), 0u);

	section.protect(utils::protection::no, {0x21000, 0x1000});
	const auto expanded_locked = section.get_locked_range();
	EXPECT_TRUE(initial_locked.inside(expanded_locked));
	EXPECT_LT(initial_locked.length(), expanded_locked.length());
	expect_texture_owner_count(expanded_locked, 1);
	EXPECT_EQ(g_ownership_directory.count_at(full_range.end), 0u);

	section.unprotect();
	EXPECT_EQ(section.get_protection(), utils::protection::rw);
	EXPECT_FALSE(section.is_locked());
	expect_texture_owner_count(expanded_locked, 0);
}

TEST(RsxCellAccessDirectory, BufferedSectionDiscardClearsOwnershipAfterPhysicalUnlock)
{
	constexpr u32 test_address = 0x60040000;
	const auto full_range = range(test_address, 0x10000);
	scoped_buffered_section_vm_state vm_state(full_range);
	ASSERT_TRUE(vm_state.mapped()) << vm_state.error();

	rsx::buffered_section section;
	section.reset(full_range);
	section.protect(utils::protection::no);
	const auto locked_range = section.get_locked_range();
	expect_texture_owner_count(locked_range, 1);

	// Texture-cache discard callers first make the host pages writable, then
	// retire the section's logical ownership without issuing another mprotect.
	rsx::memory_protect(locked_range, utils::protection::rw);
	EXPECT_EQ(section.get_protection(), utils::protection::no);
	expect_texture_owner_count(locked_range, 1);

	section.discard();
	EXPECT_EQ(section.get_protection(), utils::protection::rw);
	EXPECT_FALSE(section.is_locked());
	expect_texture_owner_count(locked_range, 0);
}
