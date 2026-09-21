#pragma once

#include <atomic>
#include <expected>

#include <async/recurring-event.hpp>
#include <frg/span.hpp>
#include <protocols/ringbuf/core.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/stream.hpp>

// Kernel side of the ringbuf protocol (see protocols/ringbuf/ringbuf.bragi).

namespace thor {

// Offers Kick messages on a lane. Can be shared among multiple rings.
struct RingKickSender {
	explicit RingKickSender(smarter::shared_ptr<Stream, LanePolicy> lane);

	RingKickSender(const RingKickSender &) = delete;
	RingKickSender &operator= (const RingKickSender &) = delete;

	// Never blocks; kicks that are issued while another kick is in flight are coalesced.
	void kick();

private:
	struct State {
		smarter::shared_ptr<Stream, LanePolicy> lane;
		std::atomic<bool> inFlight{false};
		std::atomic<bool> again{false};
	};

	static coroutine<void> run_(smarter::shared_ptr<State> state);

	smarter::shared_ptr<State> state_;
};

// Accepts Kick messages on a lane. Can be shared among multiple rings.
struct RingKickReceiver {
	explicit RingKickReceiver(smarter::shared_ptr<Stream, LanePolicy> lane);

	RingKickReceiver(const RingKickReceiver &) = delete;

	~RingKickReceiver();

	RingKickReceiver &operator= (const RingKickReceiver &) = delete;

	// Number of kicks that were received so far.
	uint64_t sequence() {
		return state_->sequence.load(std::memory_order_relaxed);
	}

	bool closed() {
		return state_->closed.load(std::memory_order_relaxed);
	}

	// Waits until sequence() changes or until the lane is closed.
	coroutine<void> waitPast(uint64_t sequence);

private:
	struct State {
		smarter::shared_ptr<Stream, LanePolicy> lane;
		async::recurring_event event;
		std::atomic<uint64_t> sequence{0};
		std::atomic<bool> closed{false};
		// Set when the RingKickReceiver is destructed.
		std::atomic<bool> abandoned{false};
	};

	static coroutine<void> run_(smarter::shared_ptr<State> state);

	smarter::shared_ptr<State> state_;
};

// Memory of a ring that is allocated by the kernel.
struct KernelRing {
	static std::expected<KernelRing, Error>
	create(protocols::ringbuf::RingParameters params);

	protocols::ringbuf::ProducerHeader *producerHeader() {
		return producerWindow.access<protocols::ringbuf::ProducerHeader>(0);
	}

	protocols::ringbuf::ConsumerHeader *consumerHeader() {
		return consumerWindow.access<protocols::ringbuf::ConsumerHeader>(0);
	}

	std::byte *data() {
		return producerWindow.bytes_data(protocols::ringbuf::dataOffset);
	}

	protocols::ringbuf::RingParameters params{};
	smarter::shared_ptr<ImmediateMemory> producerMemory;
	smarter::shared_ptr<ImmediateMemory> consumerMemory;
	ImmediateWindow producerWindow;
	ImmediateWindow consumerWindow;
};

struct ProvidedKernelRing {
	KernelRing *ring;
	// Role that userspace plays for this ring.
	protocols::ringbuf::Role remoteRole;
};

struct RingKickLanes {
	// Null if userspace never waits.
	smarter::shared_ptr<RingKickSender> sender;
	// Null if the kernel never waits.
	smarter::shared_ptr<RingKickReceiver> receiver;
};

// Kernel side of the ringbuf handshake: pushes the descriptors of the rings
// (and of the kick lanes that the rings share) over a conversation lane.
coroutine<frg::expected<Error, RingKickLanes>>
provideRings(smarter::shared_ptr<Stream, LanePolicy> conversation,
		frg::span<const ProvidedKernelRing> rings);

} // namespace thor
