#pragma once

#include <assert.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <arch/dma_pool.hpp>
#include <async/counting-semaphore.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <core/virtio/core.hpp>
#include <frg/list.hpp>
#include <linux/virtio_vsock.h>

namespace vsock {

struct Device;
struct VsockStack;

enum class TxClass {
	// Header-only packets that sockets send on their own.
	control,
	// Header-only replies to received packets; only taken while receiving.
	reply,
	// Packets that carry up to Device::maxTxPayload bytes of payload.
	data
};

// Transmit buffer of a Device; returns to its pool unless it is passed to Device::send().
// Must not be destroyed under a lock since this resumes waiters of Device::allocateTx().
struct TxBuffer {
	friend struct Device;

	TxBuffer() = default;

	TxBuffer(TxBuffer &&other)
	: packet_{std::exchange(other.packet_, nullptr)} { }

	~TxBuffer();

	TxBuffer &operator= (TxBuffer other) {
		std::swap(packet_, other.packet_);
		return *this;
	}

	explicit operator bool () const {
		return packet_;
	}

	// Space for the payload of the packet.
	std::span<char> payload();

private:
	// A packet is either held by a TxBuffer, in Device::TxPool::free,
	// in Device::txPending_ or posted to the device.
	struct Packet : virtio_core::Request {
		Device *device;
		TxClass txClass;
		arch::dma_buffer buffer;
		// Size of the header and the payload that are sent.
		size_t size = 0;
		// Links the packet into TxPool::free or txPending_.
		frg::default_list_hook<Packet> hook;
	};

	explicit TxBuffer(Packet *packet)
	: packet_{packet} { }

	Packet *packet_ = nullptr;
};

// Driver for a virtio-vsock device.
// Thread-safe after initialize() returns.
struct Device {
	Device(std::unique_ptr<virtio_core::Transport> transport);

	Device(const Device &) = delete;

	Device &operator= (const Device &) = delete;

	// Sets up the virtqueues and starts to deliver packets and events to the stack.
	async::result<void> initialize(VsockStack *stack);

	uint64_t guestCid() {
		return guestCid_.load(std::memory_order_acquire);
	}

	// Keeps the buffers of TxClass::data in the largest size class of the contiguous DMA pool.
	static constexpr size_t maxTxPayload = 16 * 1024 - sizeof(virtio_vsock_hdr);

	// Takes a buffer out of a pool; waits (in FIFO order) for the device to complete packets
	// if the pool is exhausted. Callers must not wait for anything else while holding a buffer.
	async::result<TxBuffer> allocateTx(TxClass txClass);

	// Calls fn with an empty buffer; while fn returns false, calls it again with an allocated one.
	// fn takes its own locks, so it can decide whether to send at all before we wait for a buffer.
	template<typename F>
	async::result<void> withTx(TxClass txClass, F fn) {
		TxBuffer buffer;
		while(!fn(buffer)) {
			assert(!buffer);
			buffer = co_await allocateTx(txClass);
		}
	}

	// Queues a packet that consists of hdr and the first hdr.len bytes of buffer.payload().
	// Packets are transmitted in the order in which they are queued, so callers can order
	// the packets of a connection by queueing them under a lock.
	// Does not block; call flushTx() afterwards (outside of any lock) to start transmission.
	void send(TxBuffer buffer, const virtio_vsock_hdr &hdr);

	// Wakes up the coroutine that posts queued packets to the device.
	void flushTx();

private:
	friend struct TxBuffer;

	struct RxBuffer : virtio_core::Request {
		Device *device;
		arch::dma_buffer buffer;
		// Links the buffer into rxDone_.
		frg::default_list_hook<RxBuffer> hook;
	};

	struct EventBuffer : virtio_core::Request {
		Device *device;
		arch::dma_buffer buffer;
		// Links the buffer into eventsDone_.
		frg::default_list_hook<EventBuffer> hook;
	};

	template<typename T>
	using List = frg::intrusive_list<
		T,
		frg::locate_member<
			T,
			frg::default_list_hook<T>,
			&T::hook
		>
	>;

	void loadGuestCid_();

	void releaseTx_(TxBuffer::Packet *packet);

	async::result<void> postRx_(RxBuffer *rx);
	async::result<void> postEvent_(EventBuffer *ev);
	async::result<void> processRx_();
	async::result<void> processEvents_();
	async::result<void> processTx_();

	std::unique_ptr<virtio_core::Transport> transport_;
	virtio_core::Queue *rxQueue_ = nullptr;
	virtio_core::Queue *txQueue_ = nullptr;
	virtio_core::Queue *eventQueue_ = nullptr;
	VsockStack *stack_ = nullptr;

	std::atomic<uint64_t> guestCid_{0};

	std::mutex rxMutex_;
	// Completed buffers in the order in which the device used them.
	// Protected by rxMutex_.
	List<RxBuffer> rxDone_;
	async::recurring_event rxEvent_;
	// Protected by rxMutex_.
	List<EventBuffer> eventsDone_;
	async::recurring_event eventsEvent_;

	struct TxPool {
		// Counts the buffers in free; acquired before a buffer is taken.
		async::counting_semaphore available;
		// Protected by txMutex_.
		List<TxBuffer::Packet> free;
	};

	// Indexed by TxClass.
	std::array<TxPool, 3> txPools_;

	std::mutex txMutex_;
	// Protected by txMutex_.
	List<TxBuffer::Packet> txPending_;
	async::recurring_event txEvent_;
};

} // namespace vsock
