
#include <async/basic.hpp>
#include <frg/scope_exit.hpp>
#include <helix/clock.hpp>
#include <helix/dispatcher-pool.hpp>
#include <stdlib.h>
#include <algorithm>
#include <iostream>
#include <list>

#include "block.hpp"

namespace block {
namespace virtio {

static bool logInitiateRetire = false;

namespace {

uint64_t currentNs() {
	return helix::getClock();
}

void emitRequestTrace(UserRequest *request, unsigned int queueIndex) {
	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkRequest,
		blockfs::ostAttrTime(request->completeTs - request->enqueueTs),
		blockfs::ostAttrNumBytes(request->view.size()),
		blockfs::ostAttrIsWrite(request->write),
		blockfs::ostAttrQueueIndex(queueIndex),
		blockfs::ostAttrTimeSetup(request->setupTime),
		blockfs::ostAttrTimeObtain(request->obtainTime),
		blockfs::ostAttrTimeDevice(request->completeTs - request->submitTs)
	);
}

blockfs::Error statusToError(uint8_t status) {
	if(status == VIRTIO_BLK_S_UNSUPP)
		return blockfs::Error::notSupported;
	return blockfs::Error::ioError;
}

} // anonymous namespace

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

// Poison the status byte by initializing it to 0xFF.
UserRequest::UserRequest(bool write_, uint64_t sector_, arch::dma_buffer_view view_,
		arch::dma_pool *pool)
: write{write_}, sector{sector_}, view{view_}, header{pool}, status{pool, uint8_t{0xFF}} { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id)
: blockfs::BlockDevice{512, parent_id, &transport->memoryPool_},
  _transport{std::move(transport)},
  _size{0} {}

async::result<void> Device::runDevice() {
	// Without VIRTIO_BLK_F_FLUSH, qemu disables its write cache and fdatasync()s on every write.
	// Negotiate it to get writeback caching on the host side.
	if(_transport->checkDeviceFeature(VIRTIO_BLK_F_FLUSH)) {
		_transport->acknowledgeDriverFeature(VIRTIO_BLK_F_FLUSH);
		_hasFlush = true;
	}

	// Multi-queue lets each pool thread submit to a virtq of its own.
	auto hasMq = _transport->checkDeviceFeature(VIRTIO_BLK_F_MQ);
	if(hasMq)
		_transport->acknowledgeDriverFeature(VIRTIO_BLK_F_MQ);

	_transport->finalizeFeatures();

	unsigned int numQueues = 1;
	if(hasMq) {
		auto maxQueues = _transport->space().load(spec::regs::numQueues);
		auto maxThreads = helix::DispatcherPool::global().maxThreads();
		numQueues = std::min({
			static_cast<unsigned int>(maxQueues),
			static_cast<unsigned int>(maxThreads),
			_transport->maxIrqsAvailableToQueues()
		});
		assert(numQueues);
	}
	std::cout << "virtio: Using " << numQueues << " request queues" << std::endl;

	// Each virtq is submitted to by a single thread, hence they all want their own interrupt.
	std::vector<virtio_core::QueueIrq> irqs(numQueues, virtio_core::QueueIrq::own);
	_transport->claimQueues(irqs);
	for(unsigned int i = 0; i < numQueues; ++i)
		_queues.push_back(co_await _transport->setupQueue(i));
	_queueLive = std::make_unique<std::atomic_flag[]>(numQueues);

	size_t numDescriptors = _queues.front()->numDescriptors();
	for(auto queue : _queues)
		numDescriptors = std::min(numDescriptors, queue->numDescriptors());

	// Limit to ensure that we don't monopolize the device.
	size_t maxSectors = numDescriptors / 4;
	assert(maxSectors >= 1);
	limits.maxSectorsPerRequest = maxSectors;

	// The capacity of a queue is its virtq's descriptors (see issueCost()),
	// such that requests wait in the I/O scheduler and not in obtainDescriptors().
	limits.numQueues = numQueues;
	limits.queueCapacity = numDescriptors;

	// Service queue 0 from here. It is also used by threads that are not DispatcherPool members.
	_startServicer(0);

	auto size = static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[0]))
			| (static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[1])) << 32);
	std::cout << "virtio: Disk size: " << size << " sectors" << std::endl;
	_size = size;

	_transport->runDevice();

	blockfs::runDevice(this);
}

void Device::_startServicer(unsigned int index) {
	if(_queueLive[index].test_and_set(std::memory_order_acq_rel))
		return;
	async::detach(_queues[index]->serviceQueue());
}

virtio_core::Queue *Device::_useQueue(unsigned int index) {
	if(!_queueLive[index].test(std::memory_order_acquire))
		_startServicer(index);
	return _queues[index];
}

async::result<void> Device::prepareIo(unsigned int queueIndex, arch::dma_buffer_view view) {
	// The chunks of a request are only known once it is mapped, which is rarely the case yet
	// since manage requests import their memory into a new region.
	auto &space = _queues[queueIndex]->dmaSpace();
	if(!space.check_mapped(view))
		co_await space.ensure_mapped(view);
}

size_t Device::issueCost(unsigned int queueIndex, arch::dma_buffer_view view) {
	// The header and the status byte take one descriptor each.
	return _queues[queueIndex]->countContiguous(view) + 2;
}

async::result<std::expected<void, blockfs::Error>> Device::issueRead(unsigned int queueIndex, uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));

	protocols::ostrace::Timer timer;

	auto queue = _useQueue(queueIndex);

	UserRequest request{false, sector, view, pagePool};
	request.enqueueTs = currentNs();
	co_await _issueRequest(queue, &request);

	std::expected<void, blockfs::Error> result;
	co_await request.event.wait();
	if(*request.status != VIRTIO_BLK_S_OK) {
		std::cout << "virtio: Device signaled an error for request at sector "
				<< request.sector << ", status "
				<< static_cast<unsigned int>(*request.status) << std::endl;
		result = std::unexpected{statusToError(*request.status)};
	}
	emitRequestTrace(&request, queue->queueIndex());

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkReadSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
	co_return result;
}

async::result<std::expected<void, blockfs::Error>> Device::issueWrite(unsigned int queueIndex, uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));

	protocols::ostrace::Timer timer;

	auto queue = _useQueue(queueIndex);

	UserRequest request{true, sector, view, pagePool};
	request.enqueueTs = currentNs();
	co_await _issueRequest(queue, &request);

	std::expected<void, blockfs::Error> result;
	co_await request.event.wait();
	if(*request.status != VIRTIO_BLK_S_OK) {
		std::cout << "virtio: Device signaled an error for request at sector "
				<< request.sector << ", status "
				<< static_cast<unsigned int>(*request.status) << std::endl;
		result = std::unexpected{statusToError(*request.status)};
	}
	emitRequestTrace(&request, queue->queueIndex());

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkWriteSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
	co_return result;
}

async::result<std::expected<void, blockfs::Error>> Device::flush() {
	protocols::ostrace::Timer timer;
	uint64_t setupTime = 0;
	uint64_t obtainTime = 0;
	uint64_t deviceTime = 0;
	frg::scope_exit evtOnExit{[&] {
		blockfs::ostContext.emit(
			blockfs::ostEvtVirtioBlkFlush,
			blockfs::ostAttrTime(timer.elapsed()),
			blockfs::ostAttrTimeSetup(setupTime),
			blockfs::ostAttrTimeObtain(obtainTime),
			blockfs::ostAttrTimeDevice(deviceTime)
		);
	}};

	// Devices without a flush command still emit the event, with a zero device time.
	if(!_hasFlush)
		co_return {};

	UserRequest request{false, 0, {}, pagePool};

	// VIRTIO_BLK_T_FLUSH is device-wide, so any queue covers the writes of all of them.
	auto queue = _useQueue(pickQueue());

	auto &space = queue->dmaSpace();
	co_await space.ensure_mapped(request.header.view_buffer());
	co_await space.ensure_mapped(request.status.view_buffer());

	std::array<virtio_core::Handle, 2> handles;
	setupTime += timer.split();
	co_await queue->obtainDescriptors(handles);
	obtainTime = timer.split();
	handles[0].setupLink(handles[1]);

	request.header->type = VIRTIO_BLK_T_FLUSH;
	request.header->reserved = 0;
	request.header->sector = 0;

	handles[0].setupBuffer(virtio_core::hostToDevice, request.header.view_buffer());
	handles[1].setupBuffer(virtio_core::deviceToHost, request.status.view_buffer());

	setupTime += timer.split();
	request.submitTs = currentNs();

	queue->postDescriptor(handles.front(), &request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		request->completeTs = currentNs();
		request->event.raise();
	});
	queue->notify();

	co_await request.event.wait();
	deviceTime = request.completeTs - request.submitTs;
	if(*request.status != VIRTIO_BLK_S_OK) {
		std::cout << "virtio: Device signaled an error for flush, status "
				<< static_cast<unsigned int>(*request.status) << std::endl;
		// The device offered VIRTIO_BLK_F_FLUSH, so even UNSUPP means the flush failed.
		co_return std::unexpected{blockfs::Error::ioError};
	}
	co_return {};
}

async::result<size_t> Device::getSize() {
	co_return _size * 512;
}

async::result<void> Device::_issueRequest(virtio_core::Queue *queue, UserRequest *request) {
	assert(request->view.size());

	protocols::ostrace::Timer setupTimer;

	// Establish the DMA mappings of the remaining regions; prepareIo() mapped the data.
	auto &space = queue->dmaSpace();
	for(auto view : {request->header.view_buffer(), request->status.view_buffer()})
		if(!space.check_mapped(view))
			co_await space.ensure_mapped(view);

	// Split the view into DMA-contiguous chunks (and not into individual sectors)
	// since setupBuffer() only requires contiguity in DMA space per descriptor.
	auto chunks = queue->splitContiguous(request->view);

	// Acquire all descriptors of the chain at once to avoid potential deadlocks.
	std::vector<virtio_core::Handle> handles(2 + chunks.size());
	request->setupTime += setupTimer.split();
	co_await queue->obtainDescriptors(handles);
	request->obtainTime = setupTimer.split();

	for(size_t i = 1; i < handles.size(); i++)
		handles[i - 1].setupLink(handles[i]);

	// Setup the descriptor for the request header.
	if(request->write) {
		request->header->type = VIRTIO_BLK_T_OUT;
	}else{
		request->header->type = VIRTIO_BLK_T_IN;
	}
	request->header->reserved = 0;
	request->header->sector = request->sector;

	handles.front().setupBuffer(virtio_core::hostToDevice, request->header.view_buffer());

	// Setup descriptors for the transfered data.
	for(size_t i = 0; i < chunks.size(); i++) {
		if(request->write) {
			handles[1 + i].setupBuffer(virtio_core::hostToDevice, chunks[i]);
		}else{
			handles[1 + i].setupBuffer(virtio_core::deviceToHost, chunks[i]);
		}
	}

	if(logInitiateRetire)
		std::cout << "Submitting " << chunks.size() << " data descriptors" << std::endl;

	// Setup a descriptor for the status byte.
	handles.back().setupBuffer(virtio_core::deviceToHost, request->status.view_buffer());

	// Stamp the submission before posting: the completion callback may run
	// as soon as the descriptor is posted.
	request->setupTime += setupTimer.split();
	request->submitTs = currentNs();

	// Submit the request to the device
	queue->postDescriptor(handles.front(), request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		if(logInitiateRetire)
			std::cout << "Retiring request of " << request->view.size()
					<< " bytes" << std::endl;
		request->completeTs = currentNs();
		request->event.raise();
	});
	queue->notify();
}

} } // namespace block::virtio
