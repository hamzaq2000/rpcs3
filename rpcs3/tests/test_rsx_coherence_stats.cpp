#include <gtest/gtest.h>

#include <memory>

#include "Emu/RSX/RSXCoherenceStats.h"

namespace
{
	using namespace rsx::coherence_stats;

	fault_event make_event(u32 address)
	{
		fault_event event{};
		event.fault_address = address;
		return event;
	}
}

TEST(RsxCoherenceFaultRing, PreservesFifoOrder)
{
	auto ring = std::make_unique<fault_event_ring>();

	ASSERT_TRUE(ring->try_push(make_event(0x1000)));
	ASSERT_TRUE(ring->try_push(make_event(0x2000)));
	ASSERT_TRUE(ring->try_push(make_event(0x3000)));

	fault_event event{};
	ASSERT_TRUE(ring->try_pop(event));
	EXPECT_EQ(event.sequence, 1u);
	EXPECT_EQ(event.fault_address, 0x1000u);
	ASSERT_TRUE(ring->try_pop(event));
	EXPECT_EQ(event.sequence, 2u);
	EXPECT_EQ(event.fault_address, 0x2000u);
	ASSERT_TRUE(ring->try_pop(event));
	EXPECT_EQ(event.sequence, 3u);
	EXPECT_EQ(event.fault_address, 0x3000u);
	EXPECT_FALSE(ring->try_pop(event));
}

TEST(RsxCoherenceFaultRing, FullQueueDropsWithoutOverwrite)
{
	auto ring = std::make_unique<fault_event_ring>();

	for (u32 i = 0; i < fault_ring_capacity; i++)
	{
		ASSERT_TRUE(ring->try_push(make_event(i)));
	}

	EXPECT_FALSE(ring->try_push(make_event(0xdeadbeef)));
	EXPECT_EQ(ring->dropped(), 1u);

	fault_event event{};
	for (u32 i = 0; i < fault_ring_capacity; i++)
	{
		ASSERT_TRUE(ring->try_pop(event));
		EXPECT_EQ(event.fault_address, i);
	}

	EXPECT_FALSE(ring->try_pop(event));
}

TEST(RsxCoherenceFaultRing, RecyclesSlotsAfterWrap)
{
	auto ring = std::make_unique<fault_event_ring>();
	fault_event event{};

	for (u32 pass = 0; pass < 2; pass++)
	{
		for (u32 i = 0; i < fault_ring_capacity; i++)
		{
			ASSERT_TRUE(ring->try_push(make_event(pass * fault_ring_capacity + i)));
		}

		for (u32 i = 0; i < fault_ring_capacity; i++)
		{
			ASSERT_TRUE(ring->try_pop(event));
			EXPECT_EQ(event.sequence, static_cast<u64>(pass) * fault_ring_capacity + i + 1);
			EXPECT_EQ(event.fault_address, pass * fault_ring_capacity + i);
		}
	}

	EXPECT_EQ(ring->dropped(), 0u);
	EXPECT_FALSE(ring->try_pop(event));
}

TEST(RsxCoherenceMfcContext, NestsRestoresAndClears)
{
	clear_active_mfc_context();

	{
		scoped_mfc_context outer(true, 1, 0x10000, 128, 0x40, 3, mfc_context_get, mfc_context_source::normal_dma);
		ASSERT_TRUE(g_active_mfc_context.valid);
		EXPECT_EQ(g_active_mfc_context.ea, 0x10000u);
		EXPECT_EQ(g_active_mfc_context.source, mfc_context_source::normal_dma);

		{
			scoped_mfc_context inner(true, 2, 0x20000, 256, 0x24, 4,
				mfc_context_put | mfc_context_list, mfc_context_source::list_fastpath);
			ASSERT_TRUE(g_active_mfc_context.valid);
			EXPECT_EQ(g_active_mfc_context.ea, 0x20000u);
			EXPECT_EQ(g_active_mfc_context.flags, mfc_context_put | mfc_context_list);
			EXPECT_EQ(g_active_mfc_context.source, mfc_context_source::list_fastpath);
		}

		ASSERT_TRUE(g_active_mfc_context.valid);
		EXPECT_EQ(g_active_mfc_context.ea, 0x10000u);
		EXPECT_EQ(g_active_mfc_context.flags, mfc_context_get);
		EXPECT_EQ(g_active_mfc_context.source, mfc_context_source::normal_dma);
	}

	EXPECT_FALSE(g_active_mfc_context.valid);

	{
		scoped_mfc_context context(true, 3, 0x30000, 64, 0x20, 5,
			mfc_context_put, mfc_context_source::raw_spu_proxy);
		ASSERT_TRUE(g_active_mfc_context.valid);
		clear_active_mfc_context();
		EXPECT_FALSE(g_active_mfc_context.valid);
	}

	EXPECT_FALSE(g_active_mfc_context.valid);
}

TEST(RsxCoherenceFaultClassification, DistinguishesConfirmedPaddingCollateralAndChain)
{
	const auto classify = [](u32 fault, u32 ea, u16 size, u32 full_start, u32 full_end,
		u32 confirmed_start, u32 confirmed_end, u32 locked_start, u32 locked_end)
	{
		fault_event event{};
		event.fault_address = fault;
		event.mfc = {.spu_id = 1, .ea = ea, .size = size,
			.cmd = 0x40, .tag = 0, .flags = mfc_context_get,
			.source = mfc_context_source::normal_dma, .valid = true};
		event.mfc_contains_fault = mfc_context_contains(event.mfc, fault);

		fault_event* previous = g_active_fault_event;
		g_active_fault_event = &event;
		record_readback_section(full_start, full_end, confirmed_start, confirmed_end, locked_start, locked_end,
			8, 0, 0, true, 1, 2, 3);
		g_active_fault_event = previous;
		return event;
	};

	const auto confirmed = classify(0x1100, 0x1100, 0x80, 0x1000, 0x1fff, 0x1080, 0x17ff, 0x0000, 0x3fff);
	EXPECT_TRUE(confirmed.sections[0].relation_flags & fault_section_access_confirmed);
	EXPECT_TRUE(confirmed.sections[0].relation_flags & fault_section_access_full);
	EXPECT_TRUE(confirmed.sections[0].relation_flags & fault_section_access_locked);
	EXPECT_TRUE(confirmed.mfc_contains_fault);

	const auto padding = classify(0x1800, 0x1800, 0x80, 0x1000, 0x1fff, 0x1000, 0x17ff, 0x0000, 0x3fff);
	EXPECT_FALSE(padding.sections[0].relation_flags & fault_section_access_confirmed);
	EXPECT_TRUE(padding.sections[0].relation_flags & fault_section_access_full);
	EXPECT_TRUE(padding.sections[0].relation_flags & fault_section_confirmed_padding);

	const auto collateral = classify(0x3000, 0x3000, 0x80, 0x1000, 0x1fff, 0x1000, 0x1fff, 0x0000, 0x3fff);
	EXPECT_FALSE(collateral.sections[0].relation_flags & fault_section_access_full);
	EXPECT_TRUE(collateral.sections[0].relation_flags & fault_section_access_locked);
	EXPECT_TRUE(collateral.sections[0].relation_flags & fault_section_locked_only);
	EXPECT_TRUE(collateral.sections[0].relation_flags & fault_section_native_collateral);
	EXPECT_TRUE(collateral.sections[0].relation_flags & fault_section_other_4k_lane);

	const auto chain = classify(0x5000, 0x5000, 0x80, 0x1000, 0x1fff, 0x1000, 0x1fff, 0x0000, 0x3fff);
	EXPECT_FALSE(chain.sections[0].relation_flags & fault_section_access_locked);
	EXPECT_TRUE(chain.sections[0].relation_flags & fault_section_chain_or_other);
}

TEST(RsxCoherenceFaultClassification, RetainsTwoPairedSectionsAndOverflowsThird)
{
	fault_event event{};
	event.fault_address = 0x1100;
	event.mfc = {.spu_id = 1, .ea = 0x1100, .size = 0x80,
		.cmd = 0x40, .tag = 0, .flags = mfc_context_get,
		.source = mfc_context_source::list_fastpath, .valid = true};

	fault_event* previous = g_active_fault_event;
	set_enabled(true);
	g_active_fault_event = &event;
	record_readback_section(0x1000, 0x1fff, 0x1000, 0x17ff, 0x0000, 0x3fff,
		8, 0, 0, true, 1, 2, 3);
	record_readback_range(0x1000, 0x800);
	record_readback_section(0x4000, 0x4fff, 0x4000, 0x47ff, 0x4000, 0x7fff,
		8, 0, 0, true, 4, 5, 6);
	record_readback_range(0x4000, 0x800);
	record_readback_section(0x8000, 0x8fff, 0x8000, 0x87ff, 0x8000, 0xbfff,
		16, 1, 2, false, 7, 8, 9);
	record_readback_range(0x8000, 0x800);
	g_active_fault_event = previous;
	set_enabled(false);

	EXPECT_EQ(event.readback_section_count, 3u);
	EXPECT_EQ(event.readback_section_overflow, 1u);
	EXPECT_EQ(stored_section_count(event), 2u);
	EXPECT_TRUE(event.sections[0].has_section);
	EXPECT_TRUE(event.sections[0].has_readback);
	EXPECT_EQ(event.sections[0].readback_start, 0x1000u);
	EXPECT_EQ(event.sections[0].sync_timestamp, 1u);
	EXPECT_TRUE(event.sections[1].has_section);
	EXPECT_TRUE(event.sections[1].has_readback);
	EXPECT_EQ(event.sections[1].readback_start, 0x4000u);
	EXPECT_EQ(event.sections[1].sync_timestamp, 4u);
	EXPECT_EQ(event.readback_pairing_errors, 0u);
	EXPECT_EQ(unmatched_section_count(event), 0u);
	EXPECT_EQ(unmatched_readback_count(event), 0u);
	EXPECT_EQ(readbacks_outside_locked_count(event), 0u);
}

TEST(RsxCoherenceFaultClassification, DetectsOutOfOrderReadbackPairing)
{
	fault_event event{};
	fault_event* previous = g_active_fault_event;
	set_enabled(true);
	g_active_fault_event = &event;
	record_readback_range(0x1000, 0x40);
	record_readback_section(0x1000, 0x10ff, 0x1000, 0x10ff, 0x1000, 0x1fff,
		8, 0, 0, true, 1, 2, 3);
	g_active_fault_event = previous;
	set_enabled(false);

	EXPECT_EQ(event.readback_pairing_errors, 1u);
	EXPECT_EQ(event.readback_count, 1u);
	EXPECT_EQ(event.readback_section_count, 1u);
	EXPECT_FALSE(event.sections[0].has_readback);
}

TEST(RsxCoherenceFaultClassification, ValidatesEachReadbackAgainstItsPairedLockedRange)
{
	fault_event event{};
	fault_event* previous = g_active_fault_event;
	set_enabled(true);
	g_active_fault_event = &event;
	record_readback_section(0x1000, 0x1fff, 0x1000, 0x1fff, 0x0000, 0x3fff,
		8, 0, 0, true, 1, 2, 3);
	record_readback_range(0x1000, 0x1000);
	record_readback_section(0x8000, 0x8fff, 0x8000, 0x8fff, 0x8000, 0xbfff,
		8, 0, 0, true, 4, 5, 6);
	record_readback_range(0x8000, 0x1000);
	g_active_fault_event = previous;
	set_enabled(false);

	// The aggregate envelope spans outside either locked range, but each paired
	// operation is wholly contained and therefore valid.
	EXPECT_LT(event.readback_start, event.sections[1].locked_start);
	EXPECT_GT(event.readback_end, event.sections[0].locked_end);
	EXPECT_EQ(readbacks_outside_locked_count(event), 0u);

	event.sections[1].readback_end = 0xc000;
	EXPECT_EQ(readbacks_outside_locked_count(event), 1u);
}

TEST(RsxCoherenceFaultClassification, ValidatesMfcFaultContainment)
{
	mfc_context context{.spu_id = 1, .ea = 0x1000, .size = 0x80,
		.cmd = 0x40, .tag = 0, .flags = mfc_context_get,
		.source = mfc_context_source::normal_dma, .valid = true};

	EXPECT_TRUE(mfc_context_contains(context, 0x1000));
	EXPECT_TRUE(mfc_context_contains(context, 0x107f));
	EXPECT_FALSE(mfc_context_contains(context, 0x1080));
	EXPECT_FALSE(mfc_context_contains(context, 0x2000));

	context.ea = 0xfffffff0;
	context.size = 0x40;
	EXPECT_TRUE(mfc_context_contains(context, 0xffffffff));
}

TEST(RsxCoherenceSemanticSite, CoalescesMovingAddressesButSplitsSemanticState)
{
	fault_event lhs{};
	lhs.origin = fault_origin::spu;
	lhs.origin_id = 1;
	lhs.origin_guest_pc = 0x2000;
	lhs.origin_block_hash = 0x1111;
	lhs.fault_pc = 0x100000;
	lhs.fault_instruction = 0xad4005e0;
	lhs.fault_address = 0x30001000;
	lhs.renderer_path = renderer_fault_path_texture;
	lhs.mfc = {.spu_id = 1, .ea = 0x30000000, .size = 512,
		.cmd = 0x44, .tag = 1, .flags = mfc_context_get | mfc_context_list,
		.source = mfc_context_source::list_fastpath, .valid = true};
	lhs.mfc_contains_fault = true;
	lhs.readback_count = 2;
	lhs.sections[0] = {.context = 8, .relation_flags = fault_section_access_confirmed,
		.protection = 2, .read_flags = 1, .synchronized = true, .has_section = true};
	lhs.sections[1] = {.context = 16,
		.relation_flags = fault_section_locked_only | fault_section_native_collateral,
		.protection = 3, .read_flags = 2, .synchronized = false, .has_section = true};

	fault_event rhs = lhs;
	rhs.origin_id = 9;
	rhs.origin_block_hash = 0x9999;
	rhs.fault_pc = 0x200000;
	rhs.fault_address = 0x50002000;
	rhs.mfc.spu_id = 9;
	rhs.mfc.ea = 0x50000000;
	rhs.mfc.size = 256;
	rhs.mfc.cmd = 0x46;
	rhs.mfc.tag = 31;
	std::swap(rhs.sections[0], rhs.sections[1]);
	EXPECT_TRUE(semantic_site_matches(lhs, rhs));

	rhs.mfc.source = mfc_context_source::normal_dma;
	EXPECT_FALSE(semantic_site_matches(lhs, rhs));
	rhs.mfc.source = lhs.mfc.source;
	rhs.mfc_contains_fault = false;
	EXPECT_FALSE(semantic_site_matches(lhs, rhs));
	rhs.mfc_contains_fault = true;
	rhs.sections[0].protection = 7;
	EXPECT_FALSE(semantic_site_matches(lhs, rhs));
	rhs.sections[0].protection = lhs.sections[1].protection;
	rhs.readback_section_overflow = 1;
	EXPECT_FALSE(semantic_site_matches(lhs, rhs));
}
