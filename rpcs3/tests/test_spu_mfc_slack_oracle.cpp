#include <gtest/gtest.h>

#include "Emu/Cell/MFC.h"
#include "Emu/Cell/SPUMfcSlackOracle.h"

namespace
{
	using namespace spu_mfc_slack;

	struct result_buffer
	{
		std::array<result, 256> values{};
		usz count = 0;

		static void push(void* context, const result& value) noexcept
		{
			auto& self = *static_cast<result_buffer*>(context);
			if (self.count < self.values.size())
			{
				self.values[self.count++] = value;
			}
		}
	};

	list_observation direct_get(u8 tag = 5)
	{
		list_observation observation{};
		observation.owner = 0x1234;
		observation.spu_id = 3;
		observation.list_eal = 0x5000;
		observation.list_size = 0x30;
		observation.tag = tag;
		observation.cmd = 0x44;
		observation.direct = true;
		observation.get_list = true;
		return observation;
	}

	void complete_candidate(tracker& oracle, candidate_key key = {11, 2}, u8 tag = 5,
		u64 candidate_ticks = 20, u64 completion_ticks = 100)
	{
		oracle.begin_list(direct_get(tag), 10);
		ASSERT_TRUE(oracle.note_candidate(key, 3, tag, true, candidate_ticks));
		oracle.finish_list(true, false, completion_ticks);
	}

	void expect_global_command_censors_all(u8 cmd)
	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle, {1, 0}, 5, 20, 100);
		complete_candidate(oracle, {2, 0}, 7, 30, 110);

		ASSERT_TRUE(is_global_ordering_command(cmd));
		oracle.note_ordering_edge(is_global_ordering_command(cmd), false, 0, 120);

		ASSERT_EQ(results.count, 2u);
		EXPECT_EQ(results.values[0].censor, censor_reason::later_barrier_or_fence);
		EXPECT_EQ(results.values[1].censor, censor_reason::later_barrier_or_fence);
	}
}

TEST(SpuMfcSlackOracle, AllRecordsUpdateAndDemandBounds)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle);

	const u32 mask = (1u << 5) | (1u << 7);
	oracle.note_tag_update(query_mode::all, mask, 150);
	oracle.note_tag_publication(query_mode::all, mask, mask, 160);
	oracle.note_rdtag_demand(180);
	oracle.note_rdtag_return(mask, 190);

	ASSERT_EQ(results.count, 1u);
	const auto& result = results.values[0];
	EXPECT_TRUE(result.valid);
	EXPECT_EQ(result.censor, censor_reason::none);
	EXPECT_EQ(result.query_update_ticks, 150u);
	EXPECT_EQ(result.first_demand_ticks, 180u);
	EXPECT_EQ(result.completion_to_update_ticks, 50u);
	EXPECT_EQ(result.completion_to_demand_ticks, 80u);
	EXPECT_EQ(result.query_mask, mask);
}

TEST(SpuMfcSlackOracle, AnyRequiresOneBit)
{
	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle);
		const u32 mask = 1u << 5;
		oracle.note_tag_update(query_mode::any, mask, 110);
		oracle.note_tag_publication(query_mode::any, mask, mask, 115);
		oracle.note_rdtag_demand(120);
		oracle.note_rdtag_return(mask, 125);

		ASSERT_EQ(results.count, 1u);
		EXPECT_TRUE(results.values[0].valid);
		EXPECT_EQ(results.values[0].mode, query_mode::any);
	}

	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle);
		const u32 mask = (1u << 5) | (1u << 6);
		oracle.note_tag_update(query_mode::any, mask, 110);
		oracle.note_tag_publication(query_mode::any, mask, 1u << 6, 115);
		oracle.note_rdtag_demand(120);
		oracle.note_rdtag_return(1u << 6, 125);

		ASSERT_EQ(results.count, 1u);
		EXPECT_FALSE(results.values[0].valid);
		EXPECT_EQ(results.values[0].censor, censor_reason::ambiguous_any);
	}
}

TEST(SpuMfcSlackOracle, ImmediateAndZeroMaskNeverProveDependency)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle);

	oracle.note_tag_update(query_mode::immediate, 1u << 5, 110);
	oracle.note_tag_publication(query_mode::immediate, 1u << 5, 0, 115);
	oracle.note_rdtag_demand(120);
	oracle.note_rdtag_return(0, 125);

	ASSERT_EQ(results.count, 1u);
	EXPECT_EQ(results.values[0].censor, censor_reason::immediate_query);
}

TEST(SpuMfcSlackOracle, PendingMaskChangeAndPublicationSnapshotAreExact)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle);

	oracle.note_tag_update(query_mode::all, 1u << 4, 110);
	oracle.note_tag_mask(1u << 5, 115);
	oracle.note_tag_publication(query_mode::all, 1u << 5, 1u << 5, 120);
	oracle.note_tag_mask(1u << 7, 125);
	oracle.note_rdtag_demand(130);
	oracle.note_rdtag_return(1u << 5, 135);

	ASSERT_EQ(results.count, 1u);
	EXPECT_TRUE(results.values[0].valid);
	EXPECT_EQ(results.values[0].query_mask, 1u << 5);
}

TEST(SpuMfcSlackOracle, FirstCountPollFixesDemandDeadline)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle);
	const u32 mask = 1u << 5;
	oracle.note_tag_update(query_mode::all, mask, 110);
	oracle.note_rdtag_demand(120);
	oracle.note_rdtag_demand(140);
	oracle.note_tag_publication(query_mode::all, mask, mask, 150);
	oracle.note_rdtag_return(mask, 160);

	ASSERT_EQ(results.count, 1u);
	EXPECT_EQ(results.values[0].first_demand_ticks, 120u);
	EXPECT_EQ(results.values[0].completion_to_demand_ticks, 20u);
}

TEST(SpuMfcSlackOracle, StallQueuedAndOrderingEdgesCensor)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);

	oracle.begin_list(direct_get(), 10);
	ASSERT_TRUE(oracle.note_candidate({1, 0}, 3, 5, true, 11));
	oracle.finish_list(false, true, 12);

	auto queued = direct_get();
	queued.direct = false;
	oracle.begin_list(queued, 20);
	ASSERT_TRUE(oracle.note_candidate({2, 0}, 3, 5, true, 21));
	oracle.finish_list(true, false, 22);

	complete_candidate(oracle, {3, 0});
	oracle.note_ordering_edge(false, true, 5, 105);
	complete_candidate(oracle, {4, 0}, 6, 110, 120);
	oracle.note_ordering_edge(false, false, 6, 125);

	ASSERT_EQ(results.count, 4u);
	EXPECT_EQ(results.values[0].censor, censor_reason::list_stall);
	EXPECT_EQ(results.values[1].censor, censor_reason::queued_or_resumed);
	EXPECT_EQ(results.values[2].censor, censor_reason::later_barrier_or_fence);
	EXPECT_EQ(results.values[3].censor, censor_reason::later_same_tag_work);
}

TEST(SpuMfcSlackOracle, EieioIsAGlobalOrderingEdge)
{
	expect_global_command_censors_all(MFC_EIEIO_CMD);
}

TEST(SpuMfcSlackOracle, SyncIsAGlobalOrderingEdge)
{
	expect_global_command_censors_all(MFC_SYNC_CMD);
}

TEST(SpuMfcSlackOracle, QueryOverwriteAndMissingPublicationCensor)
{
	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle);
		oracle.note_tag_update(query_mode::all, 1u << 5, 110);
		oracle.note_tag_update(query_mode::all, 1u << 5, 120);

		ASSERT_EQ(results.count, 1u);
		EXPECT_EQ(results.values[0].censor, censor_reason::query_overwrite);
	}

	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle);
		oracle.note_tag_update(query_mode::all, 1u << 5, 110);
		oracle.note_rdtag_demand(120);
		oracle.note_rdtag_return(1u << 5, 125);

		ASSERT_EQ(results.count, 1u);
		EXPECT_EQ(results.values[0].censor, censor_reason::missing_publication);
	}
}

TEST(SpuMfcSlackOracle, DeadlineOverflowAndLifecycleNeverOverwriteLiveRecords)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 10);
	complete_candidate(oracle);
	oracle.note_tag_mask(0, 111);

	ASSERT_EQ(results.count, 1u);
	EXPECT_EQ(results.values[0].censor, censor_reason::deadline);

	tracker lifecycle(result_buffer::push, &results, 1000);
	complete_candidate(lifecycle, {22, 1});
	lifecycle.bind(0x9999, 4, 0, 120);

	ASSERT_EQ(results.count, 2u);
	EXPECT_EQ(results.values[1].censor, censor_reason::lifecycle_rebind);
}

TEST(SpuMfcSlackOracle, SameOwnerLifecycleRestartCensorsPendingAndQueryState)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	auto observation = direct_get();
	observation.lifecycle_generation = 1;

	oracle.begin_list(observation, 10);
	ASSERT_TRUE(oracle.note_candidate({1, 0}, 3, 5, true, 20));
	oracle.finish_list(true, false, 100);
	const u32 mask = 1u << 5;
	oracle.note_tag_update(query_mode::all, mask, 110);
	oracle.note_tag_publication(query_mode::all, mask, mask, 115);

	oracle.bind(observation.owner, observation.spu_id, 2, 120);
	ASSERT_EQ(results.count, 1u);
	EXPECT_FALSE(results.values[0].valid);
	EXPECT_EQ(results.values[0].censor, censor_reason::lifecycle_rebind);

	// Post-restart traffic cannot complete the query from the old lifecycle.
	oracle.note_rdtag_demand(125);
	oracle.note_rdtag_return(mask, 130);
	EXPECT_EQ(results.count, 1u);

	observation.lifecycle_generation = 2;
	oracle.begin_list(observation, 140);
	ASSERT_TRUE(oracle.note_candidate({2, 0}, 3, 5, true, 150));
	oracle.finish_list(true, false, 160);
	oracle.note_tag_update(query_mode::all, mask, 170);
	oracle.note_tag_publication(query_mode::all, mask, mask, 175);
	oracle.note_rdtag_demand(180);
	oracle.note_rdtag_return(mask, 185);

	ASSERT_EQ(results.count, 2u);
	EXPECT_TRUE(results.values[1].valid);
	EXPECT_EQ(results.values[1].key.serial, 2u);
}

TEST(SpuMfcSlackOracle, DeduplicatesCandidateWithinOuterList)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	oracle.begin_list(direct_get(), 10);
	EXPECT_TRUE(oracle.note_candidate({1, 2}, 3, 5, true, 20));
	EXPECT_FALSE(oracle.note_candidate({1, 2}, 3, 5, true, 21));
	oracle.finish_list(false, true, 30);

	ASSERT_EQ(results.count, 1u);
	EXPECT_EQ(results.values[0].key.serial, 1u);
}

TEST(SpuMfcSlackOracle, DemandBeforeCompletionClampsBothBounds)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle, {1, 0}, 5, 20, 200);
	const u32 mask = 1u << 5;
	oracle.note_tag_update(query_mode::all, mask, 150);
	oracle.note_tag_publication(query_mode::all, mask, mask, 160);
	oracle.note_rdtag_demand(170);
	oracle.note_rdtag_return(mask, 180);

	ASSERT_EQ(results.count, 1u);
	EXPECT_FALSE(results.values[0].valid);
	EXPECT_EQ(results.values[0].censor, censor_reason::query_precedes_completion);
	EXPECT_TRUE(results.values[0].update_before_completion);
	EXPECT_TRUE(results.values[0].demand_before_completion);
	EXPECT_EQ(results.values[0].completion_to_update_ticks, 0u);
	EXPECT_EQ(results.values[0].completion_to_demand_ticks, 0u);
}

TEST(SpuMfcSlackOracle, ZeroAndUnrelatedMasksLeaveCandidateForMatchingQuery)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	complete_candidate(oracle);

	oracle.note_tag_update(query_mode::all, 0, 110);
	oracle.note_tag_publication(query_mode::all, 0, 0, 115);
	oracle.note_rdtag_demand(120);
	oracle.note_rdtag_return(0, 125);

	oracle.note_tag_update(query_mode::all, 1u << 7, 130);
	oracle.note_tag_publication(query_mode::all, 1u << 7, 1u << 7, 135);
	oracle.note_rdtag_demand(140);
	oracle.note_rdtag_return(1u << 7, 145);
	EXPECT_EQ(results.count, 0u);

	const u32 matching = 1u << 5;
	oracle.note_tag_update(query_mode::all, matching, 150);
	oracle.note_tag_publication(query_mode::all, matching, matching, 155);
	oracle.note_rdtag_demand(160);
	oracle.note_rdtag_return(matching, 165);

	ASSERT_EQ(results.count, 1u);
	EXPECT_TRUE(results.values[0].valid);
}

TEST(SpuMfcSlackOracle, OuterOrderingAndNonGetAreCensored)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);

	auto ordered = direct_get();
	ordered.ordered = true;
	oracle.begin_list(ordered, 10);
	ASSERT_TRUE(oracle.note_candidate({1, 0}, 3, 5, true, 11));
	oracle.finish_list(true, false, 12);

	auto put = direct_get();
	put.get_list = false;
	oracle.begin_list(put, 20);
	ASSERT_TRUE(oracle.note_candidate({2, 0}, 3, 5, false, 21));
	oracle.finish_list(true, false, 22);

	ASSERT_EQ(results.count, 2u);
	EXPECT_EQ(results.values[0].censor, censor_reason::outer_barrier_or_fence);
	EXPECT_EQ(results.values[1].censor, censor_reason::not_get_list);
}

TEST(SpuMfcSlackOracle, FixedCapacitiesCensorWithoutOverwriting)
{
	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		oracle.begin_list(direct_get(), 10);

		for (u16 index = 0; index < active_candidate_capacity + 1; index++)
		{
			ASSERT_TRUE(oracle.note_candidate({u64{index} + 1, index}, 3, 5, true, 20 + index));
		}

		oracle.finish_list(false, true, 50);
		ASSERT_EQ(results.count, active_candidate_capacity + 1);
		EXPECT_EQ(results.values[0].censor, censor_reason::active_overflow);

		for (usz index = 1; index < results.count; index++)
		{
			EXPECT_EQ(results.values[index].censor, censor_reason::list_stall);
		}
	}

	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 10000);

		for (u16 index = 0; index < pending_candidate_capacity + 1; index++)
		{
			oracle.begin_list(direct_get(), 100 + index * 3);
			ASSERT_TRUE(oracle.note_candidate({u64{index} + 1, index}, 3, 5, true, 101 + index * 3));
			oracle.finish_list(true, false, 102 + index * 3);
		}

		ASSERT_EQ(results.count, 1u);
		EXPECT_EQ(results.values[0].censor, censor_reason::pending_overflow);
	}
}

TEST(SpuMfcSlackOracle, UnsupportedOptimizedTagPathIsExplicit)
{
	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		complete_candidate(oracle);
		oracle.note_unsupported_tag_path(110);

		ASSERT_EQ(results.count, 1u);
		EXPECT_EQ(results.values[0].censor, censor_reason::unsupported_optimized_tag_path);
	}

	{
		result_buffer results;
		tracker oracle(result_buffer::push, &results, 1000);
		auto unsupported = direct_get();
		unsupported.unsupported_tag_path = true;
		oracle.begin_list(unsupported, 10);
		ASSERT_TRUE(oracle.note_candidate({1, 0}, 3, 5, true, 11));
		oracle.finish_list(true, false, 12);

		ASSERT_EQ(results.count, 1u);
		EXPECT_EQ(results.values[0].censor, censor_reason::unsupported_optimized_tag_path);
	}
}

TEST(SpuMfcSlackOracle, OneQueryResolvesEveryMatchingCandidate)
{
	result_buffer results;
	tracker oracle(result_buffer::push, &results, 1000);
	oracle.begin_list(direct_get(), 10);
	ASSERT_TRUE(oracle.note_candidate({1, 0}, 3, 5, true, 20));
	ASSERT_TRUE(oracle.note_candidate({1, 1}, 3, 5, true, 21));
	oracle.finish_list(true, false, 100);

	const u32 mask = 1u << 5;
	oracle.note_tag_update(query_mode::all, mask, 110);
	oracle.note_tag_publication(query_mode::all, mask, mask, 115);
	oracle.note_rdtag_demand(120);
	oracle.note_rdtag_return(mask, 125);

	ASSERT_EQ(results.count, 2u);
	EXPECT_TRUE(results.values[0].valid);
	EXPECT_TRUE(results.values[1].valid);
	EXPECT_NE(results.values[0].key.member, results.values[1].key.member);
}
