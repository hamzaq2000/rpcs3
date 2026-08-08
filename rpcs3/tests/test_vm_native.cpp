#include <gtest/gtest.h>

#include "util/vm.hpp"

#if defined(__APPLE__) && defined(ARCH_ARM64)

#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace
{
	constexpr vm_prot_t c_access_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;

	class scoped_reservation
	{
		u8* m_ptr{};
		usz m_size{};

	public:
		scoped_reservation(usz size, bool is_memory_mapping)
			: m_ptr(static_cast<u8*>(utils::memory_reserve(size, nullptr, is_memory_mapping, false)))
			, m_size(size)
		{
		}

		scoped_reservation(const scoped_reservation&) = delete;
		scoped_reservation& operator=(const scoped_reservation&) = delete;

		~scoped_reservation()
		{
			if (m_ptr)
			{
				utils::memory_release(m_ptr, m_size);
			}
		}

		u8* get() const
		{
			return m_ptr;
		}
	};

	::testing::AssertionResult has_protection(const void* ptr, vm_prot_t expected)
	{
		mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(ptr);
		mach_vm_size_t size = 0;
		vm_region_basic_info_data_64_t info{};
		mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object_name = MACH_PORT_NULL;

		const kern_return_t result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);

		if (MACH_PORT_VALID(object_name))
		{
			mach_port_deallocate(mach_task_self(), object_name);
		}

		if (result != KERN_SUCCESS)
		{
			return ::testing::AssertionFailure() << "mach_vm_region failed with " << result;
		}

		const auto target = reinterpret_cast<mach_vm_address_t>(ptr);

		if (target < address || target - address >= size)
		{
			return ::testing::AssertionFailure()
				<< "mach_vm_region returned [0x" << std::hex << address << ", 0x" << address + size
				<< ") for target 0x" << target;
		}

		const vm_prot_t actual = info.protection & c_access_protection;

		if (actual != expected)
		{
			return ::testing::AssertionFailure()
				<< "protection at " << ptr << " is 0x" << std::hex << actual << ", expected 0x" << expected;
		}

		return ::testing::AssertionSuccess();
	}
}

TEST(VmNative, MappingReservationStartsInaccessible)
{
	constexpr usz reservation_size = 0x30000;
	scoped_reservation reservation{reservation_size, true};
	auto* const base = reservation.get();

	ASSERT_NE(base, nullptr);
	EXPECT_TRUE(has_protection(base, VM_PROT_NONE));
	EXPECT_TRUE(has_protection(base + reservation_size - 1, VM_PROT_NONE));
}

TEST(VmNative, NonMappingReservationRemainsWritable)
{
	constexpr usz reservation_size = 0x10000;
	scoped_reservation reservation{reservation_size, false};
	auto* const base = reservation.get();

	ASSERT_NE(base, nullptr);
	EXPECT_TRUE(has_protection(base, VM_PROT_READ | VM_PROT_WRITE));

	base[0] = 0xa5;
	base[reservation_size - 1] = 0x5a;
	EXPECT_EQ(base[0], 0xa5);
	EXPECT_EQ(base[reservation_size - 1], 0x5a);
}

TEST(VmNative, FileOverlayOnMappingReservation)
{
	constexpr usz segment_size = 0x10000;
	constexpr usz reservation_size = segment_size * 3;
	scoped_reservation reservation{reservation_size, true};
	auto* const base = reservation.get();

	ASSERT_NE(base, nullptr);

	utils::shm shm{segment_size};
	auto* const target = base + segment_size;
	const auto [mapped, error] = shm.map_critical(target);

	ASSERT_EQ(mapped, target) << error;
	ASSERT_TRUE(error.empty()) << error;

	target[0] = 0xa5;
	target[segment_size - 1] = 0x5a;
	EXPECT_EQ(target[0], 0xa5);
	EXPECT_EQ(target[segment_size - 1], 0x5a);

	EXPECT_TRUE(has_protection(base, VM_PROT_NONE));
	EXPECT_TRUE(has_protection(target, VM_PROT_READ | VM_PROT_WRITE));
	EXPECT_TRUE(has_protection(base + segment_size * 2, VM_PROT_NONE));

	shm.unmap_critical(target);
	EXPECT_TRUE(has_protection(target, VM_PROT_NONE));
}

#endif
