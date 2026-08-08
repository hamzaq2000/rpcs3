#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "Emu/Cell/SPURecompiler.h"

namespace
{
void fast_stub(spu_thread&, void*, u8*)
{
}

void llvm_stub(spu_thread&, void*, u8*)
{
}

spu_item make_item()
{
	return spu_item{{.entry_point = 0, .lower_bound = 0, .data = {1}}};
}
}

TEST(SpuLlvmCompileClaim, SerializesConcurrentCompilers)
{
	constexpr u32 worker_count = 8;
	spu_item item = make_item();
	std::barrier start{worker_count};
	std::atomic<u32> compile_count = 0;
	std::atomic<u32> reuse_count = 0;
	std::atomic<u32> bad_pointer_count = 0;
	std::vector<std::thread> workers;
	workers.reserve(worker_count);

	for (u32 i = 0; i < worker_count; i++)
	{
		workers.emplace_back([&]
		{
			start.arrive_and_wait();
			spu_item::llvm_compile_claim claim{item};

			if (claim.owns_compile())
			{
				compile_count++;
				item.compiled.release(&llvm_stub);
				claim.mark_succeeded();
			}
			else
			{
				reuse_count++;

				if (item.compiled.load() != &llvm_stub)
				{
					bad_pointer_count++;
				}
			}
		});
	}

	for (auto& worker : workers)
	{
		worker.join();
	}

	EXPECT_EQ(compile_count.load(), 1u);
	EXPECT_EQ(reuse_count.load(), worker_count - 1);
	EXPECT_EQ(bad_pointer_count.load(), 0u);
	EXPECT_EQ(item.llvm_state.load(), spu_item::llvm_compile_state::ready);
}

TEST(SpuLlvmCompileClaim, FailureReturnsItemToIdle)
{
	spu_item item = make_item();

	{
		spu_item::llvm_compile_claim failed_claim{item};
		ASSERT_TRUE(failed_claim.owns_compile());
	}

	EXPECT_EQ(item.llvm_state.load(), spu_item::llvm_compile_state::idle);

	{
		spu_item::llvm_compile_claim retry_claim{item};
		ASSERT_TRUE(retry_claim.owns_compile());
		item.compiled.release(&llvm_stub);
		retry_claim.mark_succeeded();
	}

	EXPECT_EQ(item.llvm_state.load(), spu_item::llvm_compile_state::ready);

	spu_item::llvm_compile_claim ready_claim{item};
	EXPECT_FALSE(ready_claim.owns_compile());
}

TEST(SpuLlvmCompileClaim, FastPointerDoesNotSuppressLlvmUpgrade)
{
	spu_item item = make_item();
	item.compiled.release(&fast_stub);

	{
		spu_item::llvm_compile_claim llvm_claim{item};
		ASSERT_TRUE(llvm_claim.owns_compile());
		EXPECT_EQ(item.compiled.load(), &fast_stub);

		item.compiled.release(&llvm_stub);
		llvm_claim.mark_succeeded();
	}

	spu_item::llvm_compile_claim ready_claim{item};
	EXPECT_FALSE(ready_claim.owns_compile());
	EXPECT_EQ(item.compiled.load(), &llvm_stub);
}
