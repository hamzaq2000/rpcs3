#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "Emu/RSX/Common/cell_access_coherence.h"
#include "Emu/RSX/Common/texture_cache_utils.h"
#include "Emu/RSX/Host/MM.h"
#include "Emu/RSX/RSXCoherenceStats.h"
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

namespace
{
	exact_cohort_plan cohort_plan(u32 fault_address = 0x20000000, u64 cache_revision = 11)
	{
		exact_cohort_plan plan{};
		plan.renderer_epoch = 3;
		plan.directory_sequence = 8;
		plan.cache_revision = cache_revision;
		plan.fault_range = range(fault_address, 0x4000);
		plan.invalidate_range = range(0x20000000, 0x8000);
		plan.section_count = 1;
		auto& section = plan.sections[0];
		section.section_identity = 0x100;
		section.section_session_generation = 0x200;
		section.producer_identity = 0x300;
		section.content_generation = 0x400;
		section.transfer_generation = 0x500;
		section.write_generation = 0x600;
		section.full_range = range(0x20000000, 0x8000);
		section.confirmed_range = section.full_range;
		section.locked_range = section.full_range;
		section.rsx_pitch = 5120;
		section.gcm_format = 0x85;
		section.width = 1280;
		section.height = 720;
		section.depth = 1;
		section.mipmaps = 1;
		return plan;
	}

	exact_cohort_member_observation cohort_member(u64 frame, u64 semantic_ready_us,
		u64 queue_release_us, u32 readback_count = 0)
	{
		exact_cohort_member_observation observation{};
		observation.frame = frame;
		observation.fault_start_ticks = frame * 100;
		observation.fault_end_ticks = frame * 100 + 90;
		observation.flush_wait_start_ticks = frame * 100 + 10;
		observation.flush_wait_end_ticks = frame * 100 + 20;
		observation.readback_wait_start_ticks = readback_count ? frame * 100 + 30 : 0;
		observation.readback_wait_end_ticks = readback_count ? frame * 100 + 80 : 0;
		observation.readback_bytes = readback_count ? 64 : 0;
		observation.submission_ready_us = semantic_ready_us;
		observation.queue_ref_removed_us = queue_release_us;
		observation.data_ready_us = semantic_ready_us;
		observation.unprotect_done_us = semantic_ready_us;
		observation.semantic_ready_us = semantic_ready_us;
		observation.first_staged_completion_generation = 0x400;
		observation.fault_address = 0x20000040;
		observation.origin = static_cast<u8>(rsx::coherence_stats::fault_origin::spu);
		observation.mfc_spu_id = 0x1234;
		observation.mfc_ea = 0x20000040;
		observation.mfc_size = 64;
		observation.mfc_source = static_cast<u8>(rsx::coherence_stats::mfc_context_source::list_fastpath);
		observation.mfc_cmd = 0x40;
		observation.mfc_tag = 7;
		observation.mfc_flags = rsx::coherence_stats::mfc_context_get |
			rsx::coherence_stats::mfc_context_list;
		observation.mfc_valid = true;
		observation.mfc_contains_fault = true;
		observation.flush_wait_count = 1;
		observation.readback_wait_count = readback_count;
		observation.readback_count = readback_count;
		observation.transfer_count = readback_count;
		observation.copied_start = readback_count ? 0x20000000u : static_cast<u32>(umax);
		observation.copied_end = readback_count ? 0x2000003f : 0;
		observation.flush_sections = readback_count;
		observation.queue_posts = 1;
		observation.replan_empty = !readback_count;
		observation.cache_unchanged = true;
		observation.semantic_plan_verified = true;
		observation.completion_verified = readback_count;
		return observation;
	}
}

TEST(RsxCellAccessExactCohort, JoinsAcrossScanRevisionAndKeepsLeaderAndMaterializerTimelines)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto leader = oracle->begin(cohort_plan(0x20000000, 11), cohort_member(10, 90, 0));
	const auto materializer = oracle->begin(cohort_plan(0x20000000, 12), cohort_member(10, 100, 0));
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(materializer.valid());
	EXPECT_TRUE(leader.is_proposed_leader());
	EXPECT_EQ(materializer.member, 1u);
	const auto follower = oracle->begin(cohort_plan(0x20000000, 13), cohort_member(10, 120, 0));
	ASSERT_TRUE(follower.valid());
	EXPECT_EQ(follower.member, 2u);

	oracle->complete(materializer, cohort_member(10, 100, 1000, 1));
	oracle->resolve_noop(leader, cohort_member(10, 110, 1100));
	oracle->resolve_noop(follower, cohort_member(10, 120, 1500));

	const auto totals = oracle->snapshot();
	EXPECT_EQ(totals.leaders, 1u);
	EXPECT_EQ(totals.same_plan_ranges_followers, 2u);
	EXPECT_EQ(totals.cache_revision_splits, 2u);
	EXPECT_EQ(totals.materializers_published, 1u);
	EXPECT_EQ(totals.first_completion_with_readback, 1u);
	EXPECT_EQ(totals.leader_completion_without_readback, 0u);
	EXPECT_EQ(totals.resolved_noops, 2u);
	EXPECT_EQ(totals.timing_complete, 1u);
	EXPECT_EQ(totals.homogeneous_spu_get_cohorts, 1u);

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_TRUE(completion.complete);
	EXPECT_EQ(completion.registrations, 3u);
	EXPECT_EQ(completion.first_done_member, 1u);
	EXPECT_EQ(completion.successful_members, 1u);
	EXPECT_EQ(completion.resolved_noop_members, 2u);
	EXPECT_TRUE(completion.any_readback);
	EXPECT_FALSE(completion.proposed_leader_had_readback);
	EXPECT_FALSE(completion.proposed_leader_valid);
	EXPECT_EQ(completion.materializer_queue_release_us, 1000u);
	EXPECT_EQ(completion.proposed_leader_queue_release_us, 1100u);
	EXPECT_EQ(completion.last_queue_release_us, 1500u);
	EXPECT_EQ(completion.queue_tail_us, 500u);
	EXPECT_EQ(completion.proposed_leader_queue_tail_us, 0u);
	EXPECT_EQ(completion.spu_get_members, 3u);
	EXPECT_EQ(completion.follower_queue_posts, 2u);
	EXPECT_FALSE(oracle->try_pop_completion(completion));

	exact_cohort_member_completion member{};
	u32 member_records = 0;
	while (oracle->try_pop_member_completion(member))
	{
		member_records++;
		EXPECT_EQ(member.serial, leader.serial);
		EXPECT_TRUE(member.observation.mfc_valid);
		if (member.terminal == exact_cohort_terminal_kind::resolved_noop)
		{
			EXPECT_TRUE(member.after_first_successful_completion);
		}
	}
	EXPECT_EQ(member_records, 3u);
}

TEST(RsxCellAccessExactCohort, RejectsUnknownGenerationsAndOddDirectorySessions)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto odd = cohort_plan();
	odd.directory_sequence = 9;
	EXPECT_FALSE(oracle->begin(odd).valid());

	auto unknown = cohort_plan();
	unknown.sections[0].section_session_generation = 0;
	EXPECT_FALSE(oracle->begin(unknown).valid());

	auto synchronized_unknown = cohort_plan();
	synchronized_unknown.sections[0].synchronized = 1;
	EXPECT_FALSE(oracle->begin(synchronized_unknown).valid());

	// Zero sync/staged generations are a known state while unsynchronized.
	const auto valid_unsynchronized = oracle->begin(cohort_plan());
	ASSERT_TRUE(valid_unsynchronized.valid());
	oracle->abandon(valid_unsynchronized, exact_cohort_terminal_kind::other_abandon);

	const auto totals = oracle->snapshot();
	EXPECT_EQ(totals.invalid_directory_sequence, 1u);
	EXPECT_EQ(totals.unknown_generation_rejections, 2u);
}

TEST(RsxCellAccessExactCohort, RejectsFaultRangeOutsideInvalidateRange)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto malformed = cohort_plan(0x20008000);
	ASSERT_TRUE(malformed.fault_range.valid());
	ASSERT_TRUE(malformed.invalidate_range.valid());
	ASSERT_FALSE(malformed.fault_range.inside(malformed.invalidate_range));
	EXPECT_FALSE(oracle->begin(malformed).valid());

	const auto totals = oracle->snapshot();
	EXPECT_EQ(totals.invalid_range_rejections, 1u);
	EXPECT_EQ(totals.unknown_generation_rejections, 0u);
}

TEST(RsxCellAccessExactCohort, CoversOnlyZeroTransferReplansAfterWorkReady)
{
	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan(), cohort_member(15, 100, 0));
		const auto follower = oracle->begin(cohort_plan(), cohort_member(15, 120, 0));
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->complete(leader, cohort_member(15, 100, 1000, 1));
		oracle->resolve_noop(follower, cohort_member(15, 120, 1500));

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_TRUE(completion.complete);
		EXPECT_TRUE(completion.proposed_leader_valid);
		EXPECT_EQ(completion.resolved_noop_members, 1u);
		EXPECT_EQ(completion.queue_tail_us, 500u);
		EXPECT_EQ(completion.proposed_leader_queue_tail_us, 500u);
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan(), cohort_member(16, 100, 0));
		const auto follower = oracle->begin(cohort_plan(), cohort_member(16, 120, 0));
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->complete(leader, cohort_member(16, 100, 1000, 1));
		oracle->resolve_noop(follower, cohort_member(16, 120, 1500, 1));

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_FALSE(completion.complete);
		EXPECT_EQ(completion.resolved_noop_members, 0u);
		EXPECT_EQ(completion.queue_tail_us, 0u);
		EXPECT_EQ(oracle->snapshot().resolved_noops, 0u);
	}
}

TEST(RsxCellAccessExactCohort, FailedProposedLeaderClosesRegistrationAndCensorsProjection)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto leader = oracle->begin(cohort_plan(), cohort_member(20, 100, 0));
	const auto follower = oracle->begin(cohort_plan(), cohort_member(20, 110, 0));
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(follower.valid());

	oracle->abandon(leader, exact_cohort_terminal_kind::replan_mismatch,
		cohort_member(20, 100, 1000));
	const auto next_leader = oracle->begin(cohort_plan(), cohort_member(20, 120, 0));
	ASSERT_TRUE(next_leader.valid());
	EXPECT_TRUE(next_leader.is_proposed_leader());
	EXPECT_NE(next_leader.serial, leader.serial);

	oracle->complete(follower, cohort_member(20, 110, 1200, 1));
	oracle->abandon(next_leader, exact_cohort_terminal_kind::other_abandon);

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_FALSE(completion.complete);
	EXPECT_EQ(completion.projected_tail_us, 0u);
	EXPECT_EQ(completion.queue_tail_us, 0u);
}

TEST(RsxCellAccessExactCohort, CensorsUnreadySuccessAndNoopBeforeMaterializer)
{
	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan());
		const auto follower = oracle->begin(cohort_plan());
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->complete(leader); // Missing the under-lock semantic-ready milestone.
		oracle->complete(follower, cohort_member(30, 100, 1000, 1));

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_FALSE(completion.complete);
		EXPECT_EQ(completion.abandoned_members, 1u);
		EXPECT_EQ(oracle->snapshot().replan_mismatches, 1u);
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan());
		const auto follower = oracle->begin(cohort_plan());
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->resolve_noop(follower, cohort_member(31, 90, 900));
		oracle->complete(leader, cohort_member(31, 100, 1000, 1));

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_FALSE(completion.complete);
		EXPECT_EQ(completion.resolved_noop_members, 0u);
		EXPECT_EQ(completion.abandoned_members, 1u);
	}
}

TEST(RsxCellAccessExactCohort, CensorsMultipleMaterializers)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto leader = oracle->begin(cohort_plan());
	const auto follower = oracle->begin(cohort_plan());
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(follower.valid());
	oracle->complete(leader, cohort_member(32, 100, 1000, 1));
	oracle->complete(follower, cohort_member(32, 110, 1200, 1));

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_FALSE(completion.complete);
	EXPECT_EQ(completion.successful_members, 2u);
	EXPECT_EQ(oracle->snapshot().multiple_materializers, 1u);
}

TEST(RsxCellAccessExactCohort, ResetPreservesSerialAgainstLateTicketAba)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto old_ticket = oracle->begin(cohort_plan());
	ASSERT_TRUE(old_ticket.valid());
	oracle->reset();
	const auto new_ticket = oracle->begin(cohort_plan());
	ASSERT_TRUE(new_ticket.valid());
	EXPECT_NE(old_ticket.serial, new_ticket.serial);

	oracle->complete(old_ticket);
	EXPECT_EQ(oracle->snapshot().stale_completion, 1u);
	oracle->abandon(new_ticket, exact_cohort_terminal_kind::other_abandon);
}

TEST(RsxCellAccessExactCohort, TimesCrossPageSemanticMembersWithDirectionalCoverage)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto first_plan = cohort_plan(0x20000000);
	const auto second_plan = cohort_plan(0x20004000);
	const auto first = oracle->begin(first_plan);
	const auto second = oracle->begin(second_plan);
	ASSERT_TRUE(first.valid());
	ASSERT_TRUE(second.valid());
	EXPECT_EQ(first.serial, second.serial);
	EXPECT_EQ(second.member, 1u);
	EXPECT_TRUE(oracle->matches_ticket_plan(first, first_plan));
	EXPECT_TRUE(oracle->matches_ticket_plan(second, second_plan));
	EXPECT_FALSE(oracle->matches_ticket_plan(second, first_plan));
	EXPECT_EQ(oracle->snapshot().cross_page_same_plan_pairs, 1u);
	EXPECT_EQ(oracle->snapshot().cross_page_followers_covered_by_leader, 1u);
	EXPECT_EQ(oracle->snapshot().different_plan_ranges_followers, 1u);
	oracle->complete(first, cohort_member(35, 100, 1000, 1));
	oracle->resolve_noop(second, cohort_member(35, 120, 1500));

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_TRUE(completion.complete);
	EXPECT_TRUE(completion.materializer_covers_all_faults);
	EXPECT_TRUE(completion.proposed_leader_covers_all_faults);
	EXPECT_EQ(completion.cross_page_members, 1u);
	EXPECT_EQ(completion.materializer_fault_range, first_plan.fault_range);
	EXPECT_EQ(completion.queue_tail_us, 500u);
}

TEST(RsxCellAccessExactCohort, CensorsCrossPageTailOutsideMaterializerInvalidateRange)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto outside = cohort_plan(0x20008000);
	outside.invalidate_range = range(0x20008000, 0x4000);
	const auto first = oracle->begin(cohort_plan(0x20000000));
	const auto second = oracle->begin(outside);
	ASSERT_TRUE(first.valid());
	ASSERT_TRUE(second.valid());
	EXPECT_EQ(first.serial, second.serial);
	oracle->complete(first, cohort_member(36, 100, 1000, 1));
	oracle->resolve_noop(second, cohort_member(36, 120, 1500));

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_FALSE(completion.complete);
	EXPECT_FALSE(completion.materializer_covers_all_faults);
	EXPECT_EQ(completion.queue_tail_us, 0u);
}

TEST(RsxCellAccessExactCohort, CountsEveryCrossPagePairAndOnlyDirectionalLeaderCoverage)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto first_a = oracle->begin(cohort_plan(0x20000000));
	const auto b = oracle->begin(cohort_plan(0x20004000));
	const auto second_a = oracle->begin(cohort_plan(0x20000000));
	ASSERT_TRUE(first_a.valid());
	ASSERT_TRUE(b.valid());
	ASSERT_TRUE(second_a.valid());

	const auto totals = oracle->snapshot();
	EXPECT_EQ(totals.cross_page_same_plan_pairs, 2u);
	EXPECT_EQ(totals.cross_page_followers_covered_by_leader, 1u);
	EXPECT_EQ(totals.same_plan_ranges_followers, 1u);
	EXPECT_EQ(totals.different_plan_ranges_followers, 1u);

	oracle->abandon(first_a, exact_cohort_terminal_kind::other_abandon);
	oracle->abandon(b, exact_cohort_terminal_kind::other_abandon);
	oracle->abandon(second_a, exact_cohort_terminal_kind::other_abandon);
}

TEST(RsxCellAccessExactCohort, SameFaultWithDifferentInvalidateIsNotCrossPageCoverage)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto different_invalidate = cohort_plan(0x20000000);
	different_invalidate.invalidate_range = range(0x20000000, 0x4000);
	const auto leader = oracle->begin(cohort_plan(0x20000000));
	const auto follower = oracle->begin(different_invalidate);
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(follower.valid());

	const auto totals = oracle->snapshot();
	EXPECT_EQ(totals.cross_page_same_plan_pairs, 0u);
	EXPECT_EQ(totals.cross_page_followers_covered_by_leader, 0u);
	EXPECT_EQ(totals.same_plan_ranges_followers, 0u);
	EXPECT_EQ(totals.different_plan_ranges_followers, 1u);

	oracle->abandon(leader, exact_cohort_terminal_kind::other_abandon);
	oracle->abandon(follower, exact_cohort_terminal_kind::other_abandon);
}

TEST(RsxCellAccessExactCohort, SemanticResolutionClosesRegistrationBeforeRuntimeFinalization)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto leader_begin = cohort_member(40, 0, 0);
	leader_begin.mfc_ea = 0x20000100;
	leader_begin.mfc_size = 128;
	const auto leader = oracle->begin(cohort_plan(), leader_begin);
	const auto follower = oracle->begin(cohort_plan(), cohort_member(40, 0, 0));
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(follower.valid());

	auto semantic = cohort_member(40, 100, 0, 1);
	semantic.mfc_ea = 0xdead0000;
	ASSERT_TRUE(oracle->resolve_semantic(leader, exact_cohort_terminal_kind::success, semantic));

	// Publishing the materializer under the semantic lock closes registration,
	// even though its queue/fault timing is not finalized yet.
	const auto successor = oracle->begin(cohort_plan(), cohort_member(40, 130, 0));
	ASSERT_TRUE(successor.valid());
	EXPECT_TRUE(successor.is_proposed_leader());
	EXPECT_NE(successor.serial, leader.serial);

	ASSERT_TRUE(oracle->resolve_semantic(follower,
		exact_cohort_terminal_kind::resolved_noop, cohort_member(40, 120, 0)));
	exact_cohort_member_observation follower_final{};
	follower_final.fault_end_ticks = 4090;
	follower_final.queue_posts = 1;
	follower_final.queue_ref_removed_us = 1500;
	oracle->finalize_member(follower, follower_final);

	exact_cohort_member_observation leader_final{};
	leader_final.fault_end_ticks = 4080;
	leader_final.queue_posts = 1;
	leader_final.queue_ref_removed_us = 1000;
	oracle->finalize_member(leader, leader_final);
	oracle->abandon(successor, exact_cohort_terminal_kind::other_abandon);

	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_TRUE(completion.complete);
	EXPECT_EQ(completion.queue_tail_us, 500u);

	exact_cohort_member_completion member{};
	bool found_leader = false;
	while (oracle->try_pop_member_completion(member))
	{
		if (member.serial != leader.serial || member.member != leader.member)
		{
			continue;
		}
		found_leader = true;
		// Finalization merges only runtime fields; begin-time MFC identity and the
		// under-lock semantic evidence survive unchanged.
		EXPECT_EQ(member.observation.mfc_ea, 0x20000100u);
		EXPECT_EQ(member.observation.mfc_size, 128u);
		EXPECT_TRUE(member.observation.semantic_plan_verified);
		EXPECT_TRUE(member.observation.completion_verified);
		EXPECT_EQ(member.observation.semantic_ready_us, 100u);
		EXPECT_EQ(member.observation.queue_ref_removed_us, 1000u);
	}
	EXPECT_TRUE(found_leader);
}

TEST(RsxCellAccessExactCohort, MissingOrSpuriousQueueReleaseCensorsOnlyQueueMetric)
{
	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan());
		const auto follower = oracle->begin(cohort_plan());
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->complete(leader, cohort_member(41, 100, 1000, 1));
		oracle->resolve_noop(follower, cohort_member(41, 120, 0));

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_TRUE(completion.complete);
		EXPECT_EQ(completion.queue_tail_us, 0u);
		EXPECT_EQ(oracle->snapshot().incomplete_queue_timing, 1u);
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		const auto leader = oracle->begin(cohort_plan());
		const auto follower = oracle->begin(cohort_plan());
		ASSERT_TRUE(leader.valid());
		ASSERT_TRUE(follower.valid());
		oracle->complete(leader, cohort_member(42, 100, 1000, 1));
		auto spurious = cohort_member(42, 120, 1500);
		spurious.queue_posts = 0;
		oracle->resolve_noop(follower, spurious);

		exact_cohort_completion completion{};
		ASSERT_TRUE(oracle->try_pop_completion(completion));
		EXPECT_TRUE(completion.complete);
		EXPECT_EQ(completion.last_queue_release_us, 1000u);
		EXPECT_EQ(completion.queue_tail_us, 0u);
		EXPECT_EQ(oracle->snapshot().queue_timestamp_without_post, 1u);
	}
}

TEST(RsxCellAccessExactCohort, NonLeaderMismatchClosesRegistrationImmediately)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto leader = oracle->begin(cohort_plan());
	const auto follower = oracle->begin(cohort_plan());
	ASSERT_TRUE(leader.valid());
	ASSERT_TRUE(follower.valid());
	ASSERT_TRUE(oracle->resolve_semantic(follower,
		exact_cohort_terminal_kind::replan_mismatch));

	const auto successor = oracle->begin(cohort_plan());
	ASSERT_TRUE(successor.valid());
	EXPECT_TRUE(successor.is_proposed_leader());
	EXPECT_NE(successor.serial, leader.serial);

	oracle->finalize_member(follower);
	oracle->complete(leader, cohort_member(43, 100, 1000, 1));
	oracle->abandon(successor, exact_cohort_terminal_kind::other_abandon);
	exact_cohort_completion completion{};
	ASSERT_TRUE(oracle->try_pop_completion(completion));
	EXPECT_FALSE(completion.complete);
}

TEST(RsxCellAccessExactCohort, RejectsFlushExclusions)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	auto plan = cohort_plan();
	plan.sections[0].has_flush_exclusions = true;
	EXPECT_FALSE(oracle->begin(plan).valid());
	EXPECT_EQ(oracle->snapshot().flush_exclusion_rejections, 1u);
}

TEST(RsxCellAccessExactCohort, CountsDuplicateSemanticAndRuntimeTerminals)
{
	auto oracle = std::make_unique<exact_cohort_oracle>();
	const auto ticket = oracle->begin(cohort_plan());
	ASSERT_TRUE(ticket.valid());
	const auto observation = cohort_member(44, 100, 1000, 1);
	EXPECT_TRUE(oracle->resolve_semantic(ticket,
		exact_cohort_terminal_kind::success, observation));
	EXPECT_FALSE(oracle->resolve_semantic(ticket,
		exact_cohort_terminal_kind::success, observation));
	oracle->finalize_member(ticket, observation);
	oracle->finalize_member(ticket, observation);
	EXPECT_EQ(oracle->snapshot().stale_completion, 2u);
}

TEST(RsxCellAccessExactCohort, ReportsBoundedSlotMemberAndRecordExhaustion)
{
	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		std::array<exact_cohort_ticket, exact_cohort_slot_capacity> tickets{};
		for (u32 index = 0; index < tickets.size(); index++)
		{
			auto plan = cohort_plan(0x20000000 + index * 0x4000);
			plan.invalidate_range = plan.fault_range;
			plan.sections[0].section_identity += index;
			tickets[index] = oracle->begin(plan);
			ASSERT_TRUE(tickets[index].valid());
		}
		auto overflow = cohort_plan(0x21000000);
		overflow.invalidate_range = overflow.fault_range;
		overflow.sections[0].section_identity += tickets.size();
		EXPECT_FALSE(oracle->begin(overflow).valid());
		EXPECT_EQ(oracle->snapshot().slot_exhaustion, 1u);
		for (const auto ticket : tickets)
		{
			oracle->abandon(ticket, exact_cohort_terminal_kind::other_abandon);
		}
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		std::array<exact_cohort_ticket, exact_cohort_member_capacity> tickets{};
		for (auto& ticket : tickets)
		{
			ticket = oracle->begin(cohort_plan());
			ASSERT_TRUE(ticket.valid());
		}
		EXPECT_FALSE(oracle->begin(cohort_plan()).valid());
		EXPECT_EQ(oracle->snapshot().member_exhaustion, 1u);
		for (const auto ticket : tickets)
		{
			oracle->abandon(ticket, exact_cohort_terminal_kind::other_abandon);
		}
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		for (u32 index = 0; index < exact_cohort_completion_capacity + 1; index++)
		{
			const auto ticket = oracle->begin(cohort_plan());
			ASSERT_TRUE(ticket.valid());
			oracle->complete(ticket, cohort_member(50 + index, 100, 1000, 1));
		}
		EXPECT_EQ(oracle->snapshot().completion_record_drops, 1u);
	}

	{
		auto oracle = std::make_unique<exact_cohort_oracle>();
		for (u32 index = 0; index < exact_cohort_member_completion_capacity + 1; index++)
		{
			const auto ticket = oracle->begin(cohort_plan());
			ASSERT_TRUE(ticket.valid());
			oracle->abandon(ticket, exact_cohort_terminal_kind::other_abandon);
		}
		EXPECT_EQ(oracle->snapshot().member_record_drops, 1u);
	}
}
