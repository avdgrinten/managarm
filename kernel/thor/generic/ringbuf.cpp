#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/vector.hpp>
#include <thor-internal/ringbuf.hpp>
#include <thor-internal/work-queue.hpp>

#include "ringbuf.frigg_bragi.hpp"

namespace thor {

namespace {
	constexpr uint32_t writerRights = kHelRightRead | kHelRightWrite | kHelRightAssign;
	constexpr uint32_t readerRights = kHelRightRead | kHelRightAssign;
}

// --------------------------------------------------------
// RingKickSender
// --------------------------------------------------------

RingKickSender::RingKickSender(smarter::shared_ptr<Stream, LanePolicy> lane)
: state_{smarter::allocate_shared<State>(*kernelAlloc)} {
	state_->lane = std::move(lane);
}

void RingKickSender::kick() {
	// The receiver may have examined the shared state before our latest change even if it
	// did not complete the exchange yet. Hence, we need to kick again in this case.
	state_->again.store(true, std::memory_order_seq_cst);
	if(state_->inFlight.exchange(true, std::memory_order_seq_cst))
		return;
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), run_(state_));
}

coroutine<void> RingKickSender::run_(smarter::shared_ptr<State> state) {
	while(true) {
		while(state->again.exchange(false, std::memory_order_seq_cst)) {
			auto [offerError, conversation] = co_await offer(state->lane);
			// Errors mean that the receiver is gone; it will not miss the kick.
			if(offerError != Error::success)
				continue;

			managarm::ringbuf::Kick<KernelAlloc> kick{*kernelAlloc};
			frg::unique_memory<KernelAlloc> kickBuffer{*kernelAlloc, kick.head_size};
			bragi::write_head_only(kick, kickBuffer);
			(void)co_await sendBuffer(conversation, std::move(kickBuffer));
		}

		// Re-check to avoid losing a kick() that raced with the loop above.
		state->inFlight.store(false, std::memory_order_seq_cst);
		if(!state->again.load(std::memory_order_seq_cst))
			break;
		if(state->inFlight.exchange(true, std::memory_order_seq_cst))
			break;
	}
}

// --------------------------------------------------------
// RingKickReceiver
// --------------------------------------------------------

RingKickReceiver::RingKickReceiver(smarter::shared_ptr<Stream, LanePolicy> lane)
: state_{smarter::allocate_shared<State>(*kernelAlloc)} {
	state_->lane = std::move(lane);
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), run_(state_));
}

RingKickReceiver::~RingKickReceiver() {
	// run_() terminates on the next kick or when the peer is gone.
	state_->abandoned.store(true, std::memory_order_relaxed);
}

coroutine<void> RingKickReceiver::waitPast(uint64_t sequence) {
	auto state = state_;
	co_await state->event.async_wait_if([&] () -> bool {
		return state->sequence.load(std::memory_order_relaxed) == sequence
				&& !state->closed.load(std::memory_order_relaxed);
	});
}

coroutine<void> RingKickReceiver::run_(smarter::shared_ptr<State> state) {
	while(true) {
		auto [acceptError, conversation] = co_await accept(state->lane);
		if(state->abandoned.load(std::memory_order_relaxed))
			co_return;
		if(acceptError != Error::success)
			break;
		// The contents of the message do not matter.
		(void)co_await recvBuffer(conversation);

		state->sequence.fetch_add(1, std::memory_order_relaxed);
		state->event.raise();
	}
	state->closed.store(true, std::memory_order_relaxed);
	state->event.raise();
}

// --------------------------------------------------------
// KernelRing
// --------------------------------------------------------

std::expected<KernelRing, Error>
KernelRing::create(protocols::ringbuf::RingParameters params) {
	if(!protocols::ringbuf::isValidRingSize(params.size) || (params.size & (kPageSize - 1)))
		return std::unexpected{Error::illegalArgs};
	static_assert(protocols::ringbuf::dataOffset == kPageSize);

	auto producerMemory = ImmediateMemory::create(protocols::ringbuf::dataOffset + params.size);
	if(!producerMemory)
		return std::unexpected{producerMemory.error()};
	auto consumerMemory = ImmediateMemory::create(protocols::ringbuf::consumerMemorySize);
	if(!consumerMemory)
		return std::unexpected{consumerMemory.error()};

	KernelRing ring;
	ring.params = params;
	ring.producerMemory = std::move(*producerMemory);
	ring.consumerMemory = std::move(*consumerMemory);
	ring.producerWindow = ImmediateWindow{ring.producerMemory};
	ring.consumerWindow = ImmediateWindow{ring.consumerMemory};
	return ring;
}

// --------------------------------------------------------
// provideRings()
// --------------------------------------------------------

coroutine<frg::expected<Error, RingKickLanes>>
provideRings(smarter::shared_ptr<Stream, LanePolicy> conversation,
		frg::span<const ProvidedKernelRing> rings) {
	frg::vector<protocols::ringbuf::RingSpec, KernelAlloc> specs{*kernelAlloc};
	for(auto &provided : rings)
		specs.push_back({provided.ring->params, provided.remoteRole});
	protocols::ringbuf::LaneNeeds needs{specs.data(), specs.size()};

	RingKickLanes lanes;
	if(needs.toReceiver) {
		auto streamOutcome = createStream();
		if(!streamOutcome)
			co_return streamOutcome.error();
		lanes.sender = smarter::allocate_shared<RingKickSender>(*kernelAlloc,
				std::move(streamOutcome->get<0>()));
		auto pushError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::lane>(
					std::move(streamOutcome->get<1>()), kHelRightInvoke));
		if(pushError != Error::success)
			co_return pushError;
	}
	if(needs.toProvider) {
		auto streamOutcome = createStream();
		if(!streamOutcome)
			co_return streamOutcome.error();
		lanes.receiver = smarter::allocate_shared<RingKickReceiver>(*kernelAlloc,
				std::move(streamOutcome->get<0>()));
		auto pushError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::lane>(
					std::move(streamOutcome->get<1>()), kHelRightInvoke));
		if(pushError != Error::success)
			co_return pushError;
	}

	for(auto &provided : rings) {
		bool remoteProduces = provided.remoteRole == protocols::ringbuf::Role::producer;
		auto producerError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::memoryView>(provided.ring->producerMemory,
					remoteProduces ? writerRights : readerRights));
		if(producerError != Error::success)
			co_return producerError;
		auto consumerError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::memoryView>(provided.ring->consumerMemory,
					remoteProduces ? readerRights : writerRights));
		if(consumerError != Error::success)
			co_return consumerError;
	}
	co_return lanes;
}

} // namespace thor
