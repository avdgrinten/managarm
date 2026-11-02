#include <algorithm>
#include <cassert>
#include <list>

#include <async/basic.hpp>

#include "io-scheduler.hpp"

namespace blockfs {

IoScheduler::IoScheduler(BlockDevice *device, std::vector<std::unique_ptr<IoDispatcher>> dispatchers)
: device_{device} {
	assert(dispatchers.size() == device_->limits.numQueues);
	// A limit of zero would never let a request through; unlimited is expressed by its absence.
	assert(device_->limits.maxSectorsPerRequest != 0 && device_->limits.queueCapacity != 0);
	for(auto &dispatcher : dispatchers)
		queues_.push_back(std::make_unique<HwQueue>(std::move(dispatcher)));
}

async::result<std::expected<void, Error>> IoScheduler::submit(bool isWrite, uint64_t sector, arch::dma_buffer_view view) {
	assert(view.size());
	auto maxBytes = device_->limits.maxSectorsPerRequest
			? *device_->limits.maxSectorsPerRequest << device_->sectorShift
			: view.size();
	auto queue = device_->pickQueue();
	co_await device_->prepareIo(queue, view);

	if(view.size() <= maxBytes) {
		IoRequest rq{queue, isWrite, sector, view, device_->issueCost(queue, view)};
		submit_(&rq);
		co_await rq.completion.wait();
		co_return rq.result;
	}

	// Queue all parts before waiting so that deep queues pipeline them.
	std::list<IoRequest> parts;
	for(size_t offset = 0; offset < view.size(); offset += maxBytes) {
		auto part = view.subview(offset, std::min(maxBytes, view.size() - offset));
		parts.emplace_back(queue, isWrite, sector + (offset >> device_->sectorShift),
				part, device_->issueCost(queue, part));
	}
	for(auto &rq : parts)
		submit_(&rq);
	// Wait for all parts even after an error since the submitter owns them.
	std::expected<void, Error> result;
	for(auto &rq : parts) {
		co_await rq.completion.wait();
		if(result && !rq.result)
			result = rq.result;
	}
	co_return result;
}

void IoScheduler::submit_(IoRequest *rq) {
	// Otherwise, the request could never be dispatched.
	assert(!device_->limits.queueCapacity || rq->cost <= *device_->limits.queueCapacity);
	auto &hq = *queues_[rq->queue];

	IoRequestList started;
	{
		std::lock_guard lock{hq.mutex};
		hq.dispatcher->submit(rq);
		dispatch_(hq, started);
	}
	// Hooks run inline until their first suspension, so start them outside of the lock.
	// Unlink each request first since it is gone once its hook completes inline.
	while(!started.empty())
		async::detach(issue_(started.pop_front()));
}

void IoScheduler::dispatch_(HwQueue &hq, IoRequestList &started) {
	auto capacity = device_->limits.queueCapacity;
	while(auto rq = hq.dispatcher->peek()) {
		// Do not let later requests overtake one that does not fit since it would starve.
		// Something is in flight in this case, so the next completion comes back here.
		if(capacity && hq.occupied + rq->cost > *capacity)
			break;
		hq.dispatcher->dispatch();
		hq.occupied += rq->cost;
		started.push_back(rq);
	}
}

async::result<void> IoScheduler::issue_(IoRequest *rq) {
	if(rq->isWrite) {
		rq->result = co_await device_->issueWrite(rq->queue, rq->sector, rq->view);
	}else{
		rq->result = co_await device_->issueRead(rq->queue, rq->sector, rq->view);
	}

	auto &hq = *queues_[rq->queue];
	IoRequestList started;
	{
		std::lock_guard lock{hq.mutex};
		hq.occupied -= rq->cost;
		// The completion has freed up queue space, so dispatch and issue more requests.
		dispatch_(hq, started);
	}
	// Hooks run inline until their first suspension, so start them outside of the lock.
	// Unlink each request first since it is gone once its hook completes inline.
	while(!started.empty())
		async::detach(issue_(started.pop_front()));

	// The submitter owns rq and may destroy it once it resumes.
	rq->completion.raise();
}

} // namespace blockfs
