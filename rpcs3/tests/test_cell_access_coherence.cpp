#include <gtest/gtest.h>

#include <memory>

#include "Emu/RSX/Common/cell_access_coherence.h"

namespace
{
	using namespace rsx::cell_access;

	utils::address_range32 range(u32 start, u32 length)
	{
		return utils::address_range32::start_length(start, length);
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
