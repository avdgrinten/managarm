#pragma once

#include <expected>
#include <memory>
#include <mutex>
#include <vector>

#include <arch/dma_structs.hpp>
#include <async/result.hpp>

#include <blockfs.hpp>
#include "io-dispatcher.hpp"

namespace blockfs {

// Per-device scheduler in front of the driver's issue{Read,Write}() hooks.
// Enforces BlockDevice::Limits.
// The IoDispatcher implements the policy, i.e., it decides the order in which requests reach the device.
// Each hardware queue has a lock, a capacity and a dispatcher of its own, such that queues do not
// contend and requests that start on completion start on the thread that services their queue.
struct IoScheduler {
	// Takes one dispatcher per hardware queue.
	IoScheduler(BlockDevice *device, std::vector<std::unique_ptr<IoDispatcher>> dispatchers);

	IoScheduler(const IoScheduler &) = delete;
	IoScheduler &operator= (const IoScheduler &) = delete;

	// Splits the request according to the device's limits,
	// queues the parts on the calling thread's hardware queue and returns once all of them completed.
	// Fails if any part failed.
	async::result<std::expected<void, Error>> submit(bool isWrite, uint64_t sector, arch::dma_buffer_view view);

private:
	struct HwQueue {
		HwQueue(std::unique_ptr<IoDispatcher> dispatcher)
		: dispatcher{std::move(dispatcher)} { }

		std::mutex mutex;
		// The dispatcher (but not the unique_ptr access) is serialized by mutex.
		std::unique_ptr<IoDispatcher> dispatcher;
		// Sum of IoRequest::cost of the requests in flight. Protected by mutex.
		size_t occupied = 0;
	};

	// Queues a request and starts whatever the limits allow to run.
	void submit_(IoRequest *rq);

	// Moves requests from the dispatcher to started while the hardware queue has capacity.
	// Must be called under hq.mutex.
	void dispatch_(HwQueue &hq, IoRequestList &started);

	// Runs one request on the device and lets the following ones proceed once it completed.
	async::result<void> issue_(IoRequest *rq);

	BlockDevice *device_;
	// Indexed by IoRequest::queue. HwQueue is not movable since it holds a mutex.
	std::vector<std::unique_ptr<HwQueue>> queues_;
};

} // namespace blockfs
