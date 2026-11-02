#pragma once

#include <expected>

#include <arch/dma_structs.hpp>
#include <async/oneshot-event.hpp>
#include <frg/list.hpp>

#include <blockfs.hpp>

namespace blockfs {

// One request as IoScheduler hands it to the driver, i.e., after splitting.
struct IoRequest {
	IoRequest(unsigned int queue, bool isWrite, uint64_t sector, arch::dma_buffer_view view, size_t cost)
	: queue{queue}, isWrite{isWrite}, sector{sector}, view{view}, cost{cost} { }

	IoRequest(const IoRequest &) = delete;
	IoRequest &operator= (const IoRequest &) = delete;

	// Hardware queue that the request is bound to.
	unsigned int queue;
	bool isWrite;
	uint64_t sector;
	arch::dma_buffer_view view;
	// Units of Limits::queueCapacity that the request occupies while it is in flight.
	size_t cost;
	std::expected<void, Error> result;
	async::oneshot_event completion;
	// Links the request into a dispatcher's queue and then into IoScheduler's requests to start.
	frg::default_list_hook<IoRequest> hook;
};

using IoRequestList = frg::intrusive_list<
	IoRequest,
	frg::locate_member<IoRequest, frg::default_list_hook<IoRequest>, &IoRequest::hook>
>;

// Decides the order in which the queued requests of one hardware queue reach the device.
// Every method runs under the hardware queue's mutex in IoScheduler,
// so implementations are single-threaded and do not lock.
struct IoDispatcher {
	virtual ~IoDispatcher() = default;

	virtual const char *name() const = 0;

	virtual void submit(IoRequest *rq) = 0;

	// Returns the request that dispatch() would return next, or null if nothing is queued.
	virtual IoRequest *peek() = 0;

	// Removes and returns the next request, or null if nothing is queued.
	virtual IoRequest *dispatch() = 0;
};

// Requests reach the device in submission order.
struct FifoDispatcher final : IoDispatcher {
	const char *name() const override {
		return "none";
	}

	void submit(IoRequest *rq) override;
	IoRequest *peek() override;
	IoRequest *dispatch() override;

private:
	IoRequestList fifo_;
};

} // namespace blockfs
