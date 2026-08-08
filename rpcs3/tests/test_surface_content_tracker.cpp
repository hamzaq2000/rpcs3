#include <gtest/gtest.h>

#include "Emu/RSX/Common/surface_utils.h"

namespace rsx
{
	TEST(SurfaceContentTracker, UnknownAndExactWrites)
	{
		surface_content_tracker tracker;
		const u64 baseline = tracker.mark_content_written();
		tracker.enable_content_write_journal();

		const u64 unknown_generation = tracker.mark_content_written();
		u32 visits = 0;
		u32 callbacks = 0;
		EXPECT_EQ(tracker.visit_content_writes(baseline, unknown_generation,
			[&](const surface_content_write_rect&) { callbacks++; }, &visits),
			surface_content_history_status::unknown_or_full);
		EXPECT_EQ(visits, 1u);
		EXPECT_EQ(callbacks, 0u);

		const surface_content_write_rect expected{ 3, 5, 17, 29 };
		const u64 exact_generation = tracker.mark_content_written();
		EXPECT_TRUE(tracker.refine_last_content_write(unknown_generation, exact_generation, expected));

		visits = 0;
		EXPECT_EQ(tracker.visit_content_writes(unknown_generation, exact_generation,
			[&](const surface_content_write_rect& rect)
			{
				callbacks++;
				EXPECT_EQ(rect.x1, expected.x1);
				EXPECT_EQ(rect.y1, expected.y1);
				EXPECT_EQ(rect.x2, expected.x2);
				EXPECT_EQ(rect.y2, expected.y2);
			}, &visits),
			surface_content_history_status::complete);
		EXPECT_EQ(visits, 1u);
		EXPECT_EQ(callbacks, 1u);
	}

	TEST(SurfaceContentTracker, RejectsUnknownOrStaleGenerations)
	{
		surface_content_tracker tracker;
		u32 visits = 99;
		EXPECT_EQ(tracker.visit_content_writes(0, 0,
			[](const surface_content_write_rect&) {}, &visits),
			surface_content_history_status::generation_mismatch);
		EXPECT_EQ(visits, 0u);

		const u64 baseline = tracker.mark_content_written();
		tracker.enable_content_write_journal();
		const u64 current = tracker.mark_content_written();
		EXPECT_EQ(tracker.visit_content_writes(baseline, current + 1,
			[](const surface_content_write_rect&) {}),
			surface_content_history_status::generation_mismatch);
	}

	TEST(SurfaceContentTracker, ReportsOverflowButRetainsNewestChain)
	{
		surface_content_tracker tracker;
		const u64 overflowed_baseline = tracker.mark_content_written();
		tracker.enable_content_write_journal();

		u64 oldest_retained_generation = 0;
		for (u32 index = 0; index < 65; ++index)
		{
			const u64 before = tracker.get_content_generation();
			const u64 after = tracker.mark_content_written();
			ASSERT_TRUE(tracker.refine_last_content_write(before, after,
				{ index, index, index + 1, index + 1 }));
			if (index == 0)
			{
				oldest_retained_generation = after;
			}
		}

		const u64 current = tracker.get_content_generation();
		EXPECT_EQ(tracker.visit_content_writes(overflowed_baseline, current,
			[](const surface_content_write_rect&) {}),
			surface_content_history_status::overflow);

		u32 visits = 0;
		EXPECT_EQ(tracker.visit_content_writes(oldest_retained_generation, current,
			[](const surface_content_write_rect&) {}, &visits),
			surface_content_history_status::complete);
		EXPECT_EQ(visits, 64u);
	}
}
