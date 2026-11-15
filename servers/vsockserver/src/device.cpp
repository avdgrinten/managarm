#include <algorithm>
#include <bit>
#include <print>
#include <string.h>

#include "device.hpp"
#include "stack.hpp"

// virtio is little endian; we pass the packet header fields through without conversion.
static_assert(std::endian::native == std::endian::little);

namespace vsock {

namespace {

// Size of each receive buffer, including the packet header.
constexpr size_t rxBufferSize = 4096;

constexpr size_t maxEventBuffers = 8;

// Bounds the DMA memory of the data pool to 1 MiB.
constexpr size_t maxTxDataBuffers = 64;

template<typename Direction>
async::result<void> postBuffer(virtio_core::Queue *queue, Direction direction,
		arch::dma_buffer_view view, virtio_core::Request *request,
		void (*complete)(virtio_core::Request *)) {
	co_await queue->dmaSpace().ensure_mapped(view);
	auto chunks = queue->splitContiguous(view);

	// Acquire all descriptors of the chain at once to avoid potential deadlocks.
	std::vector<virtio_core::Handle> handles(chunks.size());
	co_await queue->obtainDescriptors(handles);

	for(size_t i = 1; i < handles.size(); i++)
		handles[i - 1].setupLink(handles[i]);
	for(size_t i = 0; i < chunks.size(); i++)
		handles[i].setupBuffer(direction, chunks[i]);

	queue->postDescriptor(handles.front(), request, complete);
	queue->notify();
}

// Condition for async_wait_if() that takes the first element off the list instead of waiting.
template<typename List, typename T>
auto takeFirst(std::mutex &mutex, List &list, T *&element) {
	return [&] {
		std::lock_guard lock{mutex};
		if(list.empty())
			return true;
		element = list.pop_front();
		return false;
	};
}

} // anonymous namespace

TxBuffer::~TxBuffer() {
	if(packet_)
		packet_->device->releaseTx_(packet_);
}

std::span<char> TxBuffer::payload() {
	assert(packet_);
	auto p = reinterpret_cast<char *>(packet_->buffer.data());
	return {p + sizeof(virtio_vsock_hdr), packet_->buffer.size() - sizeof(virtio_vsock_hdr)};
}

Device::Device(std::unique_ptr<virtio_core::Transport> transport)
: transport_{std::move(transport)} { }

async::result<void> Device::initialize(VsockStack *stack) {
	stack_ = stack;

	transport_->finalizeFeatures();
	transport_->claimQueues(3);
	rxQueue_ = co_await transport_->setupQueue(0);
	txQueue_ = co_await transport_->setupQueue(1);
	eventQueue_ = co_await transport_->setupQueue(2);

	// All queues share one interrupt that is serviced on the thread that binds the device.
	async::detach(rxQueue_->serviceQueue());
	async::detach(txQueue_->serviceQueue());
	async::detach(eventQueue_->serviceQueue());

	transport_->runDevice();
	loadGuestCid_();

	// Each packet takes one descriptor; more buffers would only let packets pile up in txPending_.
	auto numTxBuffers = txQueue_->numDescriptors();
	auto numTxData = std::clamp<size_t>(numTxBuffers / 2, 1, maxTxDataBuffers);
	auto numTxHeaderOnly = std::max<size_t>(numTxBuffers - numTxData, 2);
	auto numTxReply = numTxHeaderOnly / 2;
	auto numTxControl = numTxHeaderOnly - numTxReply;

	auto addTxBuffers = [&] (TxClass txClass, size_t count, size_t size) {
		for(size_t i = 0; i < count; i++) {
			auto packet = new TxBuffer::Packet;
			packet->device = this;
			packet->txClass = txClass;
			packet->buffer = arch::dma_buffer{&transport_->contiguousPool_, size};
			releaseTx_(packet);
		}
	};
	addTxBuffers(TxClass::control, numTxControl, sizeof(virtio_vsock_hdr));
	addTxBuffers(TxClass::reply, numTxReply, sizeof(virtio_vsock_hdr));
	addTxBuffers(TxClass::data, numTxData, sizeof(virtio_vsock_hdr) + maxTxPayload);

	for(size_t i = 0; i < rxQueue_->numDescriptors(); i++) {
		auto rx = new RxBuffer;
		rx->device = this;
		rx->buffer = arch::dma_buffer{&transport_->contiguousPool_, rxBufferSize};
		co_await postRx_(rx);
	}

	for(size_t i = 0; i < std::min(eventQueue_->numDescriptors(), maxEventBuffers); i++) {
		auto ev = new EventBuffer;
		ev->device = this;
		ev->buffer = arch::dma_buffer{&transport_->contiguousPool_, sizeof(virtio_vsock_event)};
		co_await postEvent_(ev);
	}

	async::detach(processRx_());
	async::detach(processEvents_());
	async::detach(processTx_());
}

async::result<TxBuffer> Device::allocateTx(TxClass txClass) {
	auto &pool = txPools_[static_cast<size_t>(txClass)];
	co_await pool.available.async_acquire();

	TxBuffer::Packet *packet;
	{
		std::lock_guard lock{txMutex_};
		// The semaphore guarantees that a buffer is available.
		assert(!pool.free.empty());
		packet = pool.free.pop_back();
	}
	co_return TxBuffer{packet};
}

void Device::releaseTx_(TxBuffer::Packet *packet) {
	auto &pool = txPools_[static_cast<size_t>(packet->txClass)];
	{
		std::lock_guard lock{txMutex_};
		pool.free.push_back(packet);
	}
	pool.available.release();
}

void Device::send(TxBuffer buffer, const virtio_vsock_hdr &hdr) {
	assert(buffer);
	assert(hdr.len <= buffer.payload().size());

	auto packet = std::exchange(buffer.packet_, nullptr);
	memcpy(packet->buffer.data(), &hdr, sizeof(virtio_vsock_hdr));
	packet->size = sizeof(virtio_vsock_hdr) + hdr.len;

	std::lock_guard lock{txMutex_};
	txPending_.push_back(packet);
}

void Device::flushTx() {
	txEvent_.raise();
}

void Device::loadGuestCid_() {
	// Only the lower 32 bits of the CID are in use, the upper ones are reserved.
	auto cid = transport_->loadConfig32(offsetof(virtio_vsock_config, guest_cid));
	guestCid_.store(cid, std::memory_order_release);
}

async::result<void> Device::postRx_(RxBuffer *rx) {
	co_await postBuffer(rxQueue_, virtio_core::deviceToHost, rx->buffer, rx,
			[] (virtio_core::Request *base) {
		auto rx = static_cast<RxBuffer *>(base);
		auto device = rx->device;
		{
			std::lock_guard lock{device->rxMutex_};
			device->rxDone_.push_back(rx);
		}
		device->rxEvent_.raise();
	});
}

async::result<void> Device::postEvent_(EventBuffer *ev) {
	co_await postBuffer(eventQueue_, virtio_core::deviceToHost, ev->buffer, ev,
			[] (virtio_core::Request *base) {
		auto ev = static_cast<EventBuffer *>(base);
		auto device = ev->device;
		{
			std::lock_guard lock{device->rxMutex_};
			device->eventsDone_.push_back(ev);
		}
		device->eventsEvent_.raise();
	});
}

async::result<void> Device::processRx_() {
	// No packet is handled without a buffer for its reply; this bounds the replies that we queue.
	TxBuffer reply;
	while(true) {
		RxBuffer *rx = nullptr;
		co_await rxEvent_.async_wait_if(takeFirst(rxMutex_, rxDone_, rx));
		if(!rx)
			continue;
		if(!reply)
			reply = co_await allocateTx(TxClass::reply);

		virtio_vsock_hdr hdr;
		if(rx->len < sizeof(virtio_vsock_hdr)) {
			std::println("vsock: Dropping truncated packet of {} bytes", rx->len);
		}else{
			memcpy(&hdr, rx->buffer.data(), sizeof(virtio_vsock_hdr));
			if(hdr.len > rx->len - sizeof(virtio_vsock_hdr)) {
				std::println("vsock: Dropping packet with {} bytes of payload in {} bytes",
						hdr.len, rx->len);
			}else{
				auto payload = reinterpret_cast<const char *>(rx->buffer.byte_data()
						+ sizeof(virtio_vsock_hdr));
				stack_->handlePacket(hdr, {payload, hdr.len}, reply);
			}
		}

		co_await postRx_(rx);
	}
}

async::result<void> Device::processEvents_() {
	while(true) {
		EventBuffer *ev = nullptr;
		co_await eventsEvent_.async_wait_if(takeFirst(rxMutex_, eventsDone_, ev));
		if(!ev)
			continue;

		virtio_vsock_event event;
		if(ev->len < sizeof(virtio_vsock_event)) {
			std::println("vsock: Dropping truncated event of {} bytes", ev->len);
		}else{
			memcpy(&event, ev->buffer.data(), sizeof(virtio_vsock_event));
			if(event.id == VIRTIO_VSOCK_EVENT_TRANSPORT_RESET) {
				loadGuestCid_();
				std::println("vsock: Transport reset, guest CID is now {}", guestCid());
				stack_->handleTransportReset();
			}else{
				std::println("vsock: Ignoring unknown event {}", event.id);
			}
		}

		co_await postEvent_(ev);
	}
}

async::result<void> Device::processTx_() {
	while(true) {
		TxBuffer::Packet *packet = nullptr;
		co_await txEvent_.async_wait_if(takeFirst(txMutex_, txPending_, packet));
		if(!packet)
			continue;

		co_await postBuffer(txQueue_, virtio_core::hostToDevice,
				packet->buffer.subview(0, packet->size), packet,
				[] (virtio_core::Request *base) {
			auto packet = static_cast<TxBuffer::Packet *>(base);
			packet->device->releaseTx_(packet);
		});
	}
}

} // namespace vsock
