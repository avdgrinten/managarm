
#include <async/basic.hpp>
#include <stdlib.h>
#include <iostream>

#include "block.hpp"

namespace block {
namespace virtio {

static bool logInitiateRetire = false;

namespace {

uint64_t currentNs() {
	uint64_t ns;
	HEL_CHECK(helGetClock(&ns));
	return ns;
}

void emitRequestTrace(UserRequest *request) {
	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkRequest,
		blockfs::ostAttrTime(request->completeTs - request->enqueueTs),
		blockfs::ostAttrNumBytes(request->view.size()),
		blockfs::ostAttrIsWrite(request->write),
		blockfs::ostAttrTimeQueue(request->dequeueTs - request->enqueueTs),
		blockfs::ostAttrTimeSetup(request->submitTs - request->dequeueTs),
		blockfs::ostAttrTimeObtain(request->obtainTime),
		blockfs::ostAttrTimeDevice(request->completeTs - request->submitTs)
	);
}

} // anonymous namespace

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

UserRequest::UserRequest(bool write_, uint64_t sector_, arch::dma_buffer_view view_)
: write{write_}, sector{sector_}, view{view_} { }

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id)
: blockfs::BlockDevice{512, parent_id, &transport->memoryPool_},
  _transport{std::move(transport)},
  _requestQueue{nullptr},
  _size{0} {}

async::result<void> Device::runDevice() {
	_transport->finalizeFeatures();
	_transport->claimQueues(1);
	_requestQueue = co_await _transport->setupQueue(0);

	auto size = static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[0]))
			| (static_cast<uint64_t>(_transport->space().load(spec::regs::capacity[1])) << 32);
	std::cout << "virtio: Disk size: " << size << " sectors" << std::endl;
	_size = size;

	_transport->runDevice();

	// perform device specific setup
	virtRequestBuffer = arch::dma_array<VirtRequest>{pagePool, _requestQueue->numDescriptors()};
	statusBuffer = arch::dma_array<uint8_t>{pagePool, _requestQueue->numDescriptors()};

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer.byte_data() % sizeof(VirtRequest) == 0);

	blockfs::runDevice(this);
}

async::result<void> Device::readSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	protocols::ostrace::Timer timer;

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() >> sectorShift;

	std::vector<std::unique_ptr<UserRequest>> requests;
	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    progress << sectorShift, std::min(num_sectors - progress, max_sectors) << sectorShift
		);
		auto request = std::make_unique<UserRequest>(false, sector + progress, subview);
		co_await _submitRequest(request.get());
		requests.push_back(std::move(request));
	}

	for(auto &request : requests) {
		co_await request->event.wait();
		emitRequestTrace(request.get());
	}

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkReadSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
}

async::result<void> Device::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	// Natural alignment makes sure a sector does not cross a page boundary.
	assert(!((uintptr_t)view.data() % 512));
//	printf("writeSectors(%lu, %lu)\n", sector, num_sectors);

	protocols::ostrace::Timer timer;

	// Limit to ensure that we don't monopolize the device.
	auto max_sectors = _requestQueue->numDescriptors() / 4;
	assert(max_sectors >= 1);
	auto num_sectors = view.size() >> sectorShift;

	std::vector<std::unique_ptr<UserRequest>> requests;
	for(size_t progress = 0; progress < num_sectors; progress += max_sectors) {
		auto subview = view.subview(
		    progress << sectorShift, std::min(num_sectors - progress, max_sectors) << sectorShift
		);
		auto request = std::make_unique<UserRequest>(true, sector + progress, subview);
		co_await _submitRequest(request.get());
		requests.push_back(std::move(request));
	}

	for(auto &request : requests) {
		co_await request->event.wait();
		emitRequestTrace(request.get());
	}

	blockfs::ostContext.emit(
		blockfs::ostEvtVirtioBlkWriteSectors,
		blockfs::ostAttrTime(timer.elapsed()),
		blockfs::ostAttrNumBytes(view.size())
	);
}

async::result<size_t> Device::getSize() {
	co_return _size * 512;
}

async::result<void> Device::_submitRequest(UserRequest *request) {
	assert(request->view.size() >> sectorShift);

	request->enqueueTs = currentNs();

	// Split the view at page boundaries (and not into individual sectors) since
	// setupBuffer() only requires physical contiguity within each descriptor.
	constexpr size_t pageSize = 0x1000;
	std::vector<std::pair<size_t, size_t>> chunks;
	size_t viewOffset = 0;
	while(viewOffset < request->view.size()) {
		auto address = reinterpret_cast<uintptr_t>(request->view.data()) + viewOffset;
		auto chunk = std::min(request->view.size() - viewOffset,
				pageSize - (address & (pageSize - 1)));
		chunks.push_back({viewOffset, chunk});
		viewOffset += chunk;
	}

	// Times the wait for a free descriptor, but only if we actually have to wait.
	auto obtainDescriptor = [&] () -> async::result<virtio_core::Handle> {
		if(_requestQueue->numFreeDescriptors())
			co_return co_await _requestQueue->obtainDescriptor();
		protocols::ostrace::Timer obtainTimer;
		auto handle = co_await _requestQueue->obtainDescriptor();
		request->obtainTime += obtainTimer.elapsed();
		co_return handle;
	};

	// Acquire all descriptors of the chain while holding the mutex.
	// Buffer setup does not need to be serialized; it proceeds concurrently
	// with the setup and submission of other requests.
	co_await _submitMutex.async_lock();
	request->dequeueTs = currentNs();

	virtio_core::Chain chain;
	std::vector<virtio_core::Handle> handles;
	for(size_t i = 0; i < 2 + chunks.size(); i++) {
		auto handle = co_await obtainDescriptor();
		handles.push_back(handle);
		chain.append(handle);
	}
	_submitMutex.unlock();

	// Setup the descriptor for the request header.
	auto header = virtRequestBuffer.object_view(chain.front().tableIndex());
	if(request->write) {
		header->type = VIRTIO_BLK_T_OUT;
	}else{
		header->type = VIRTIO_BLK_T_IN;
	}
	header->reserved = 0;
	header->sector = request->sector;

	co_await handles.front().setupBuffer(virtio_core::hostToDevice, header.view_buffer());

	// Setup descriptors for the transfered data.
	for(size_t i = 0; i < chunks.size(); i++) {
		auto [chunkOffset, chunkSize] = chunks[i];
		if(request->write) {
			co_await handles[1 + i].setupBuffer(virtio_core::hostToDevice,
					request->view.subview(chunkOffset, chunkSize));
		}else{
			co_await handles[1 + i].setupBuffer(virtio_core::deviceToHost,
					request->view.subview(chunkOffset, chunkSize));
		}
	}

	if(logInitiateRetire)
		std::cout << "Submitting " << chunks.size()
				<< " data descriptors" << std::endl;

	// Setup a descriptor for the status byte.
	co_await handles.back().setupBuffer(
	    virtio_core::deviceToHost,
	    statusBuffer.object_view(chain.front().tableIndex()).view_buffer()
	);

	// Submit the request to the device
	_requestQueue->postDescriptor(chain.front(), request,
			[] (virtio_core::Request *base_request) {
		auto request = static_cast<UserRequest *>(base_request);
		if(logInitiateRetire)
			std::cout << "Retiring " << (request->view.size() / 512uz)
					<< " data descriptors" << std::endl;
		request->completeTs = currentNs();
		request->event.raise();
	});
	_requestQueue->notify();
	request->submitTs = currentNs();
}

} } // namespace block::virtio
