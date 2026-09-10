#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include <protocols/usb/api.hpp>

#include <arch/dma_pool.hpp>

#include <async/mutex.hpp>
#include <async/result.hpp>
#include <async/sequenced-event.hpp>

#include <frg/expected.hpp>

#include "trb.hpp"

enum class CompletionCode {
	invalid,
	success,
	dataBufferError,
	babbleDetected,
	usbTransactionError,
	trbError,
	stallError,
	resourceError,
	bandwidthError,
	noSlotsAvailable,
	invalidStreamType,
	slotNotEnabled,
	endpointNotEnabled,
	shortPacket,
	ringUnderrun,
	ringOverrun,
	vfEventRingFull,
	parameterError,
	bandwidthOverrun,
	contextStateError,
	noPingResponse,
	eventRingFull,
	incompatibleDevice,
	missedService,
	commandRingStopped,
	commandAborted,
	stopped,
	stoppedInvalidLength,
	stoppedShortPacket,
	maxExitLatencyTooHigh,
	reserved,
	isochBufferOverrun,
	eventLost,
	undefinedError,
	invalidStreamId,
	secondaryBandwidthError,
	splitTransactionError,
};

struct Event {
	TrbType type;
	int slotId;
	CompletionCode completionCode;

	// Transfer and command completion events
	uintptr_t trbPointer;

	// Transfer event
	size_t transferLen;
	size_t endpointId;
	bool eventData;

	// Command completion event
	int commandCompletionParameter;

	// Port status change event
	size_t portId;

	// Device notification event
	uintptr_t notificationData;
	size_t notificationType;

	// Raw TRB
	RawTrb raw;

	static Event fromRawTrb(RawTrb trb);
	void printInfo();

	const char *completionCodeName() const;
};

inline frg::expected<protocols::usb::UsbError> completionToError(Event ev) {
	using protocols::usb::UsbError;

	switch (ev.completionCode) {
		using enum CompletionCode;
		case success: return frg::success;
		case shortPacket: return frg::success;
		case babbleDetected: return UsbError::babble;
		case stallError: return UsbError::stall;
		case incompatibleDevice: return UsbError::unsupported;
		default: return UsbError::other;
	}
}

struct Controller;

struct RingPointer {
	size_t index = 0;
	bool cycle = true;

	void advance(size_t n, size_t ringSize) {
		assert(n <= ringSize);
		index += n;
		if (index >= ringSize) {
			index -= ringSize;
			cycle = !cycle;
		}
	}
};

struct EventRing {
	constexpr static size_t eventRingSize = 128;

	struct alignas(64) ErstEntry {
		uint32_t ringSegmentBaseLow;
		uint32_t ringSegmentBaseHi;
		uint32_t ringSegmentSize;
		uint32_t reserved;
	};

	struct alignas(64) EventRingEntries {
		RawTrb ent[eventRingSize];
	};

	static_assert(sizeof(ErstEntry) == 64, "invalid ErstEntry size");

	EventRing(Controller *controller);
	async::result<void> init();
	uintptr_t getErstPtr();
	uintptr_t getEventRingPtr();
	size_t getErstSize();

	void processRing();

	// True if the entry at the dequeue pointer is valid, i.e., there is an event we have not processed yet.
	bool hasPendingEvent();

private:
	arch::dma_object<EventRingEntries> _eventRing;
	arch::dma_array<ErstEntry> _erst;
	Controller *_controller;

	RingPointer _dequeue;

	uintptr_t _cachedEventRingIova = 0;
	uintptr_t _cachedErstIova = 0;
};

struct ProducerRing {
	constexpr static size_t ringSize = 128;
	// Last ring entry has to be a link TRB.
	constexpr static size_t usableRingSize = ringSize - 1;

	struct Transaction {
		async::result<frg::expected<protocols::usb::UsbError, size_t>>
		transfer();

		async::result<Event> command();

		void onEvent(Controller *controller, Event event, RawTrb associatedTrb);
	private:
		std::vector<std::pair<RawTrb, Event>> events_;
		async::sequenced_event progressEvent_;
		uint64_t progressSeq_ = 0;

		async::result<frg::expected<protocols::usb::UsbError, std::pair<RawTrb, Event>>>
		nextEvent_() {
			co_await progressEvent_.async_wait(progressSeq_);
			auto ev = events_[progressSeq_++];
			FRG_CO_TRY(completionToError(ev.second));
			co_return ev;
		}
	};

	struct alignas(64) RingEntries {
		RawTrb ent[ringSize];
	};

	ProducerRing(Controller *controller);
	async::result<void> initialize();
	uintptr_t getPtr();

	// Returns the position of the last TRB that was inserted.
	async::result<RingPointer> pushTrbs(const std::vector<RawTrb> &trbs, Transaction *tx);
	void retire(RingPointer newDequeue);

	void processEvent(Event ev);

	size_t inFlight() {
		if (_enqueue.cycle == _dequeue.cycle) {
			return _enqueue.index - _dequeue.index;
		} else {
			return _enqueue.index + usableRingSize - _dequeue.index;
		}
	}

private:
	std::array<Transaction *, ringSize> _transactions;
	arch::dma_object<RingEntries> _ring;
	Controller *_controller;
	std::mutex _mutex;

	// Protected by _mutex
	RingPointer _enqueue;
	RingPointer _dequeue;

	async::recurring_event _progressEvent;

	void _updateLink(bool initialCycle);
	uintptr_t _cachedRingIova = 0;
};
