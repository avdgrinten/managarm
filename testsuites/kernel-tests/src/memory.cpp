#include <cassert>
#include <cstddef>
#include <cstring>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <core/process-data.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include "testsuite.hpp"

namespace {

// Processes an exact number of notifications.
async::result<void> handleManageRequests(helix::BorrowedDescriptor backingMemory, int count) {
	std::byte buffer[0x1000]{};

	for(int i = 0; i < count; i++) {
		helix::ManageMemory manage;
		auto submit = helix::submitManageMemory(backingMemory, &manage,
				helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		if(manage.type() == kHelManageInitialize) {
			auto result = co_await helix_ng::writeMemory(backingMemory, manage.offset(), 0x1000, buffer);
			HEL_CHECK(result.error());
			HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);
			HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::result<void> testWritebackFence() {
	constexpr size_t memorySize = size_t{1} << 40;
	constexpr size_t pageOffset = memorySize / 2;
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(core::getProcessHierarchy(), memorySize, 0,
			&backingHandle, &frontalHandle));
	helix::UniqueDescriptor backingMemory{backingHandle};
	helix::UniqueDescriptor frontalMemory{frontalHandle};

	// An entirely absent range must not scan every possible page.
	auto emptyFence = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
	HEL_CHECK(emptyFence.error());

	// Pin an initialized page so mapped stores need no synchronous page service.
	helix::LockMemoryView lockMemory;
	co_await async::when_all(
		handleManageRequests(backingMemory, 1),
		async::lambda([&]() -> async::result<void> {
			auto submit = helix::submitLockMemoryView(frontalMemory, &lockMemory,
					pageOffset, 0x1000, helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lockMemory.error());
		})()
	);
	auto lock = lockMemory.descriptor();
	helix::Mapping first{frontalMemory, pageOffset, 0x1000, kHelMapProtRead | kHelMapProtWrite};
	helix::Mapping second{frontalMemory, pageOffset, 0x1000, kHelMapProtRead | kHelMapProtWrite};

	// No writeMemory() or synchronizeSpace(): only the mapping's PTE is dirty.
	// Repeat through another alias to check that the fence collects all mappings
	// and that stores after the first shootdown are picked up again.
	for(auto mapping : {first.get(), second.get()}) {
		auto expected = mapping == first.get() ? std::byte{42} : std::byte{43};
		*static_cast<volatile std::byte *>(mapping) = expected;
		bool writtenBack = false;
		co_await async::when_all(
			async::lambda([&]() -> async::result<void> {
				helix::ManageMemory manage;
				auto submit = helix::submitManageMemory(backingMemory, &manage,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(manage.error());
				assert(manage.type() == kHelManageWriteback);
				assert(manage.offset() == pageOffset && manage.length() == 0x1000);
				std::byte data;
				auto read = co_await helix_ng::readMemory(backingMemory, pageOffset, 1, &data);
				HEL_CHECK(read.error());
				assert(data == expected);
				writtenBack = true;
				HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageWriteback,
						manage.offset(), manage.length()));
			})(),
			async::lambda([&]() -> async::result<void> {
				auto result = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
				HEL_CHECK(result.error());
				assert(writtenBack);
			})()
		);
	}

	// The existing page at the exclusive end must be outside this empty range.
	auto prefixFence = co_await helix_ng::writebackFence(backingMemory, 0, pageOffset);
	HEL_CHECK(prefixFence.error());

	// A clean range needs no further writeback notification.
	auto result = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
	HEL_CHECK(result.error());
}

} // anonymous namespace

DEFINE_TEST(writebackFence, ([] {
	async::run(testWritebackFence(), helix::currentDispatcher);
}))

namespace {

async::result<void> testInvalidateRange() {
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(core::getProcessHierarchy(), 0x1000, 0, &backingHandle, &frontalHandle));
	helix::UniqueDescriptor backingMemory{backingHandle};
	helix::UniqueDescriptor frontalMemory{frontalHandle};

	std::byte buffer[0x1000]{};

	// Trigger initialization, then writeback.
	co_await async::when_all(
		handleManageRequests(backingMemory, 2),
		async::lambda([&]() -> async::result<void> {
			auto result = co_await helix_ng::writeMemory(frontalMemory, 0, 0x1000, buffer);
			HEL_CHECK(result.error());
			co_return;
		})()
	);

	auto invalidateResult = co_await helix_ng::invalidateMemory(backingMemory, 0, 0x1000);
	HEL_CHECK(invalidateResult.error());

	// A subsequent write to the frontal memory must trigger initialization again.
	co_await async::when_all(
		handleManageRequests(backingMemory, 2),
		async::lambda([&]() -> async::result<void> {
			auto result = co_await helix_ng::writeMemory(frontalMemory, 0, 0x1000, buffer);
			HEL_CHECK(result.error());
			co_return;
		})()
	);
}

} // anonymous namespace

DEFINE_TEST(invalidateRange, ([] {
	async::run(testInvalidateRange(), helix::currentDispatcher);
}))

namespace {

async::result<void> fillPage(helix::BorrowedDescriptor memory, uintptr_t offset, char c) {
	char buffer[0x1000];
	memset(buffer, c, 0x1000);
	auto result = co_await helix_ng::writeMemory(memory, offset, 0x1000, buffer);
	HEL_CHECK(result.error());
}

async::result<void> expectPage(helix::BorrowedDescriptor memory, uintptr_t offset, char c) {
	char buffer[0x1000]{};
	auto result = co_await helix_ng::readMemory(memory, offset, 0x1000, buffer);
	HEL_CHECK(result.error());
	for(size_t i = 0; i < 0x1000; i++)
		assert(buffer[i] == c);
}

async::result<void> expectFault(helix::BorrowedDescriptor memory, uintptr_t offset) {
	char buffer[0x1000];
	auto result = co_await helix_ng::readMemory(memory, offset, 0x1000, buffer);
	assert(result.error() == kHelErrFault);
	(void)result;
}

async::result<HelError> install(helix::BorrowedDescriptor indirect, uintptr_t offset,
		helix::BorrowedDescriptor memory, uintptr_t memoryOffset, size_t size) {
	auto result = co_await helix_ng::installMemoryIndirection(indirect, offset,
			memory, memoryOffset, size);
	co_return result.error();
}

async::result<HelError> remove(helix::BorrowedDescriptor indirect, uintptr_t offset, size_t size) {
	auto result = co_await helix_ng::removeMemoryIndirection(indirect, offset, size);
	co_return result.error();
}

async::result<void> testIndirectMemory() {
	HelHandle firstHandle, secondHandle, indirectHandle, backingHandle, frontalHandle;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x2000, 0, nullptr, &firstHandle));
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x2000, 0, nullptr, &secondHandle));
	HEL_CHECK(helCreateIndirectMemory(0x10000, &indirectHandle));
	HEL_CHECK(helCreateManagedMemory(core::getProcessHierarchy(), 0x1000, 0,
			&backingHandle, &frontalHandle));
	helix::UniqueDescriptor first{firstHandle};
	helix::UniqueDescriptor second{secondHandle};
	helix::UniqueDescriptor indirect{indirectHandle};
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor frontal{frontalHandle};

	co_await fillPage(first, 0, 'a');
	co_await fillPage(first, 0x1000, 'b');
	co_await fillPage(second, 0, 'c');
	co_await fillPage(second, 0x1000, 'd');

	// Adjacent indirections at offsets that are not aligned to their sizes.
	HEL_CHECK(co_await install(indirect, 0x1000, first, 0, 0x2000));
	HEL_CHECK(co_await install(indirect, 0x3000, second, 0x1000, 0x1000));
	co_await expectPage(indirect, 0x1000, 'a');
	co_await expectPage(indirect, 0x2000, 'b');
	co_await expectPage(indirect, 0x3000, 'd');
	co_await expectFault(indirect, 0);
	co_await expectFault(indirect, 0x4000);

	// Writes through the indirection reach the target memory.
	co_await fillPage(indirect, 0x3000, 'e');
	co_await expectPage(second, 0x1000, 'e');

	assert(co_await install(indirect, 0x2000, second, 0, 0x2000) == kHelErrAlreadyExists);
	assert(co_await install(indirect, 0x4800, second, 0, 0x1000) == kHelErrIllegalArgs);
	assert(co_await install(indirect, 0x4000, second, 0, 0) == kHelErrIllegalArgs);
	assert(co_await install(indirect, 0xF000, second, 0, 0x2000) == kHelErrOutOfBounds);
	assert(co_await install(indirect, 0x4000, second, 0x1000, 0x2000) == kHelErrOutOfBounds);
	assert(co_await install(indirect, 0x4000, frontal, 0, 0x1000) == kHelErrIllegalObject);
	assert(co_await remove(indirect, 0x1000, 0x1000) == kHelErrIllegalArgs);

	// Slices delegate to their memory and bound the indirection by their size.
	HelHandle sliceHandle;
	HEL_CHECK(helCreateSliceView(firstHandle, 0x1000, 0x1000, 0, &sliceHandle));
	helix::UniqueDescriptor slice{sliceHandle};
	assert(co_await install(indirect, 0x5000, slice, 0, 0x2000) == kHelErrOutOfBounds);
	HEL_CHECK(co_await install(indirect, 0x5000, slice, 0, 0x1000));
	co_await expectPage(indirect, 0x5000, 'b');

	// Removal unmaps the indirection: after reinstalling, the mapping sees the new memory.
	helix::Mapping mapping{indirect, 0x1000, 0x2000, kHelMapProtRead | kHelMapProtWrite};
	auto window = static_cast<volatile char *>(mapping.get());
	assert(window[0] == 'a');
	HEL_CHECK(co_await remove(indirect, 0x1000, 0x2000));
	co_await expectFault(indirect, 0x1000);
	HEL_CHECK(co_await install(indirect, 0x1000, second, 0, 0x2000));
	assert(window[0] == 'c');
	assert(window[0x1000] == 'e');

	// Dropping the handles destroys the indirect memory while indirections are installed.
}

} // anonymous namespace

DEFINE_TEST(indirectMemory, ([] {
	async::run(testIndirectMemory(), helix::currentDispatcher);
}))
