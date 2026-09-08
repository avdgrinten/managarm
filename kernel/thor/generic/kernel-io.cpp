#include <async/oneshot-event.hpp>
#include <frg/allocation.hpp>
#include <frg/hash_map.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {
	// Slots are created by whichever of publishIoChannel() / solicitIoChannel() comes first
	// and are never freed, so the map can key on the slot's own tag string.
	struct ChannelSlot {
		ChannelSlot(frg::string<KernelAlloc> tag)
		: tag{std::move(tag)} { }

		frg::string<KernelAlloc> tag;
		smarter::shared_ptr<KernelIoChannel> channel;
		async::oneshot_event published;
	};

	constinit IrqSpinlock globalChannelMutex;

	// Protected by globalChannelMutex.
	frg::eternal<
		frg::hash_map<
			frg::string_view,
			ChannelSlot *,
			frg::hash<frg::string_view>,
			Allocator
		>
	> globalChannelMap{frg::hash<frg::string_view>{}};

	// Must be called with globalChannelMutex held.
	ChannelSlot *obtainSlot(frg::string_view tag) {
		auto maybeSlot = globalChannelMap->get(tag);
		if(maybeSlot)
			return *maybeSlot;
		auto slot = frg::construct<ChannelSlot>(*kernelAlloc,
				frg::string<KernelAlloc>{*kernelAlloc, tag});
		globalChannelMap->insert(slot->tag, slot);
		return slot;
	}
}

initgraph::Stage *getIoChannelsDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "general.iochannels-discovered"};
	return &s;
}

void publishIoChannel(smarter::shared_ptr<KernelIoChannel> channel) {
	ChannelSlot *slot;
	{
		auto lock = frg::guard(&globalChannelMutex);
		slot = obtainSlot(channel->tag());
		if(slot->channel) {
			warningLogger() << "thor: Ignoring duplicate I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
			return;
		}
		slot->channel = std::move(channel);
	}
	// Waiters resume inline, so raise outside of the lock.
	slot->published.raise();
}

coroutine<smarter::shared_ptr<KernelIoChannel>> solicitIoChannel(frg::string_view tag) {
	ChannelSlot *slot;
	{
		auto lock = frg::guard(&globalChannelMutex);
		slot = obtainSlot(tag);
	}
	co_await slot->published.wait();
	co_return slot->channel;
}

coroutine<void> dumpRingToChannel(LogRingBuffer *ringBuffer,
		smarter::shared_ptr<KernelIoChannel> channel, size_t maxRecordSize) {
	// One extra byte distinguishes records of exactly maxRecordSize from truncated ones.
	frg::unique_memory<KernelAlloc> record{*kernelAlloc, maxRecordSize + 1};
	uint64_t currentPtr = 0;
	bool unflushed = false;
	while(true) {
		auto [success, recordPtr, nextPtr, actualSize] = ringBuffer->dequeueAt(
				currentPtr, record.data(), maxRecordSize + 1);
		if(!success) {
			// Do not leave output in the channel while we block on the ring.
			if(unflushed) {
				auto ioOutcome = co_await channel->issueIo(KernelIoChannel::ioProgressOutput);
				assert(ioOutcome);
				unflushed = false;
			}
			co_await ringBuffer->wait(nextPtr);
			continue;
		}
		assert(actualSize); // For now, we do not support size zero records.
		if(recordPtr != currentPtr)
			infoLogger() << "thor: Up to " << (recordPtr - currentPtr)
					<< " lost on I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
		if(actualSize > maxRecordSize) {
			infoLogger() << "thor: Packet truncated on I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
			actualSize = maxRecordSize;
		}
		currentPtr = nextPtr;

		// Records can be larger than the channel's span, so copy them in chunks.
		size_t progress = 0;
		while(progress < actualSize) {
			auto span = channel->writableSpan();
			if(!span.size()) {
				auto ioOutcome = co_await channel->issueIo(KernelIoChannel::ioProgressOutput);
				assert(ioOutcome);
				unflushed = false;
				continue;
			}
			auto chunk = frg::min(span.size(), actualSize - progress);
			memcpy(span.data(), static_cast<std::byte *>(record.data()) + progress, chunk);
			channel->produceOutput(chunk);
			unflushed = true;
			progress += chunk;
		}
	}
}

} // namespace thor
