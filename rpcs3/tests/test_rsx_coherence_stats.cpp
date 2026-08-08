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
		scoped_mfc_context outer(true, 1, 0x10000, 128, 0x40, 3, mfc_context_get);
		ASSERT_TRUE(g_active_mfc_context.valid);
		EXPECT_EQ(g_active_mfc_context.ea, 0x10000u);

		{
			scoped_mfc_context inner(true, 2, 0x20000, 256, 0x24, 4, mfc_context_put | mfc_context_list);
			ASSERT_TRUE(g_active_mfc_context.valid);
			EXPECT_EQ(g_active_mfc_context.ea, 0x20000u);
			EXPECT_EQ(g_active_mfc_context.flags, mfc_context_put | mfc_context_list);
		}

		ASSERT_TRUE(g_active_mfc_context.valid);
		EXPECT_EQ(g_active_mfc_context.ea, 0x10000u);
		EXPECT_EQ(g_active_mfc_context.flags, mfc_context_get);
	}

	EXPECT_FALSE(g_active_mfc_context.valid);

	{
		scoped_mfc_context context(true, 3, 0x30000, 64, 0x20, 5, mfc_context_put);
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
			.cmd = 0x40, .tag = 0, .flags = mfc_context_get, .valid = true};
		event.mfc_contains_fault = mfc_context_contains(event.mfc, fault);

		fault_event* previous = g_active_fault_event;
		g_active_fault_event = &event;
		record_readback_section(full_start, full_end, confirmed_start, confirmed_end, locked_start, locked_end,
			8, 0, 0, true, 1, 2, 3);
		g_active_fault_event = previous;
		return event;
	};

	const auto confirmed = classify(0x1100, 0x1100, 0x80, 0x1000, 0x1fff, 0x1080, 0x17ff, 0x0000, 0x3fff);
	EXPECT_TRUE(confirmed.section_relation_flags & fault_section_access_confirmed);
	EXPECT_TRUE(confirmed.section_relation_flags & fault_section_access_full);
	EXPECT_TRUE(confirmed.section_relation_flags & fault_section_access_locked);
	EXPECT_TRUE(confirmed.mfc_contains_fault);

	const auto padding = classify(0x1800, 0x1800, 0x80, 0x1000, 0x1fff, 0x1000, 0x17ff, 0x0000, 0x3fff);
	EXPECT_FALSE(padding.section_relation_flags & fault_section_access_confirmed);
	EXPECT_TRUE(padding.section_relation_flags & fault_section_access_full);
	EXPECT_TRUE(padding.section_relation_flags & fault_section_confirmed_padding);

	const auto collateral = classify(0x3000, 0x3000, 0x80, 0x1000, 0x1fff, 0x1000, 0x1fff, 0x0000, 0x3fff);
	EXPECT_FALSE(collateral.section_relation_flags & fault_section_access_full);
	EXPECT_TRUE(collateral.section_relation_flags & fault_section_access_locked);
	EXPECT_TRUE(collateral.section_relation_flags & fault_section_locked_only);
	EXPECT_TRUE(collateral.section_relation_flags & fault_section_native_collateral);
	EXPECT_TRUE(collateral.section_relation_flags & fault_section_other_4k_lane);

	const auto chain = classify(0x5000, 0x5000, 0x80, 0x1000, 0x1fff, 0x1000, 0x1fff, 0x0000, 0x3fff);
	EXPECT_FALSE(chain.section_relation_flags & fault_section_access_locked);
	EXPECT_TRUE(chain.section_relation_flags & fault_section_chain_or_other);
}

TEST(RsxCoherenceFaultClassification, MarksMultipleReadbackSectionsUnknown)
{
	fault_event event{};
	event.fault_address = 0x1100;
	event.mfc = {.spu_id = 1, .ea = 0x1100, .size = 0x80,
		.cmd = 0x40, .tag = 0, .flags = mfc_context_get, .valid = true};

	fault_event* previous = g_active_fault_event;
	g_active_fault_event = &event;
	record_readback_section(0x1000, 0x1fff, 0x1000, 0x17ff, 0x0000, 0x3fff,
		8, 0, 0, true, 1, 2, 3);
	record_readback_section(0x4000, 0x4fff, 0x4000, 0x47ff, 0x4000, 0x7fff,
		8, 0, 0, true, 4, 5, 6);
	g_active_fault_event = previous;

	EXPECT_EQ(event.readback_section_count, 2u);
	EXPECT_EQ(event.readback_section_overflow, 1u);
	EXPECT_TRUE(event.section_relation_flags & fault_section_multi_unknown);
	EXPECT_FALSE(event.section_relation_flags & fault_section_access_confirmed);
	EXPECT_FALSE(event.section_relation_flags & fault_section_native_collateral);
}

TEST(RsxCoherenceFaultClassification, ValidatesMfcFaultContainment)
{
	mfc_context context{.spu_id = 1, .ea = 0x1000, .size = 0x80,
		.cmd = 0x40, .tag = 0, .flags = mfc_context_get, .valid = true};

	EXPECT_TRUE(mfc_context_contains(context, 0x1000));
	EXPECT_TRUE(mfc_context_contains(context, 0x107f));
	EXPECT_FALSE(mfc_context_contains(context, 0x1080));
	EXPECT_FALSE(mfc_context_contains(context, 0x2000));

	context.ea = 0xfffffff0;
	context.size = 0x40;
	EXPECT_TRUE(mfc_context_contains(context, 0xffffffff));
}
