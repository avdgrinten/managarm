#include <cassert>
#include <protocols/posix/procdata.hpp>
#include <cstddef>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>

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
			auto result = co_await helix_ng::writeMemory(backingMemory, 0, 0x1000, buffer);
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
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), 0x1000, 0, &backingHandle, &frontalHandle));
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

	// No pending writebacks. Fence should complete immediately.
	auto result = co_await helix_ng::writebackFence(backingMemory, 0, 0x1000);
	HEL_CHECK(result.error());
}

} // anonymous namespace

DEFINE_TEST(writebackFence, ([] {
	(void)testWritebackFence;
	// TODO: The test works but we run into a crash afterwards due to a missing implementation of ~ManagedSpace() in thor.
	//async::run(testWritebackFence(), helix::currentDispatcher);
}))

namespace {

async::result<void> testInvalidateRange() {
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), 0x1000, 0, &backingHandle, &frontalHandle));
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
	(void)testInvalidateRange;
	// TODO: The test works but we run into a crash afterwards due to a missing implementation of ~ManagedSpace() in thor.
	//async::run(testInvalidateRange(), helix::currentDispatcher);
}))
