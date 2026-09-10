#include "ring.hpp"
#include "xhci.hpp"

#include <print>

// ------------------------------------------------------------------------
// Event
// ------------------------------------------------------------------------

constexpr const char *trbTypeNames[] = {
	"Reserved",

	"Normal",
	"Setup stage",
	"Data stage",
	"Status stage",
	"Isochronous",
	"Link",
	"Event data",
	"No Op (transfer)",

	"Enable slot",
	"Disable slot",
	"Address device",
	"Configure endpoint",
	"Evaluate context",
	"Reset endpoint",
	"Stop endpoint",
	"Set TR dequeue pointer",
	"Reset device",
	"Force event",
	"Negotiate bandwidth",
	"Set latency tolerance value",
	"Get port bandwidth",
	"Force header",
	"No Op (command)",
	"Get extended property",
	"Set extended property",

	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",

	"Transfer event",
	"Command completion event",
	"Port status change event",
	"Bandwidth request event",
	"Doorbell event",
	"Host controller event",
	"Device notification event",
	"MFINDEX wrap event"
};

Event Event::fromRawTrb(RawTrb trb) {
	Event ev;

	ev.type = static_cast<TrbType>((trb.val[3] >> 10) & 63);
	ev.completionCode = static_cast<CompletionCode>((trb.val[2] >> 24) & 0xFF);
	ev.slotId = (trb.val[3] >> 24) & 0xFF;
	ev.raw = trb;

	switch(ev.type) {
		case TrbType::transferEvent:
			ev.trbPointer = trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32);
			ev.transferLen = trb.val[2] & 0xFFFFFF;
			ev.endpointId = (trb.val[3] >> 16) & 0x1F;
			ev.eventData = trb.val[3] & (1 << 2);
			break;

		case TrbType::commandCompletionEvent:
			ev.trbPointer = trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32);

			ev.commandCompletionParameter = trb.val[2] & 0xFFFFFF;
			break;

		case TrbType::portStatusChangeEvent:
			ev.portId = (trb.val[0] >> 24) & 0xFF;
			break;

		case TrbType::deviceNotificationEvent:
			ev.notificationData = (trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32))
				>> 8;
			ev.notificationType = (trb.val[0] >> 4) & 0xF;
			break;

		default:
			std::println("xhci: Unexpected event {:#02x} in Event::fromRawTrb, ignoring...",
					static_cast<uint32_t>(ev.type));
	}

	return ev;
}

void Event::printInfo() {
	std::println("xhci: --- Event dump ---");
	std::println("xhci: Raw: {:08x} {:08x} {:08x} {:08x}",
			raw.val[0], raw.val[1], raw.val[2], raw.val[3]);
	std::println("xhci: Type: {} ({})",
			trbTypeNames[static_cast<unsigned int>(type)],
			static_cast<unsigned int>(type));
	std::println("xhci: Slot ID: {}", slotId);
	std::println("xhci: Completion code: {} ({})\n",
			completionCodeName(),
			static_cast<int>(completionCode));

	switch(type) {
		case TrbType::transferEvent:
			std::println("xhci: TRB pointer: {:#016x}, transfer length {}\n", trbPointer,
					transferLen);
			std::println("xhci: Endpoint ID: {}, has event data? {}\n",
					endpointId, eventData);
			break;
		case TrbType::commandCompletionEvent:
			std::println("xhci: TRB pointer: {:#016x}", trbPointer);
			std::println("xhci: Command completion parameter: {}",
					commandCompletionParameter);
			break;
		case TrbType::portStatusChangeEvent:
			std::println("xhci: Port ID: {}", portId);
			break;
		case TrbType::bandwidthRequestEvent:
		case TrbType::doorbellEvent:
		case TrbType::hostControllerEvent:
		case TrbType::mfindexWrapEvent:
			break;
		case TrbType::deviceNotificationEvent:
			std::println("xhci: Notification data: {:#x}",
					notificationData);
			std::println("xhci: Notification type: {}",
					notificationType);
			break;
		default:
			std::println("xhci: Invalid event");
	}

	std::println("xhci: --- End of event dump ---");
}

const char *Event::completionCodeName() const {
	return completionCodeNames[static_cast<int>(completionCode)];
}

// ------------------------------------------------------------------------
// EventRing
// ------------------------------------------------------------------------

EventRing::EventRing(Controller *controller)
: _eventRing{controller->memoryPool()}, _erst{controller->memoryPool(), 1}
, _controller{controller}, _dequeue{0, true} {
	for (size_t i = 0; i < eventRingSize; i++) {
		_eventRing->ent[i] = {{0, 0, 0, 0}};
	}
}

async::result<void> EventRing::init() {
	co_await _controller->dmaSpace().ensure_mapped(_eventRing);
	co_await _controller->dmaSpace().ensure_mapped(_erst);
	_cachedEventRingIova = _controller->dmaSpace().iova_of(_eventRing);
	_cachedErstIova = _controller->dmaSpace().iova_of(_erst);

	_erst[0].ringSegmentBaseLow = _cachedEventRingIova & 0xFFFFFFFF;
	_erst[0].ringSegmentBaseHi = _cachedEventRingIova >> 32;
	_erst[0].ringSegmentSize = eventRingSize;
	_erst[0].reserved = 0;

	_controller->barrier.writeback(_eventRing.view_buffer());
	_controller->barrier.writeback(_erst.view_buffer());
}

uintptr_t EventRing::getErstPtr() {
	assert(_cachedErstIova != 0);
	return _cachedErstIova;
}

uintptr_t EventRing::getEventRingPtr() {
	assert(_cachedEventRingIova != 0);
	return _cachedEventRingIova + _dequeue.index * sizeof(RawTrb);
}

size_t EventRing::getErstSize() {
	return _erst.size();
}

void EventRing::processRing() {
	_controller->barrier.invalidate(_eventRing.view_buffer());
	while((_eventRing->ent[_dequeue.index].val[3] & 1) == _dequeue.cycle) {
		RawTrb rawEv = _eventRing->ent[_dequeue.index];

		_dequeue.advance(1, eventRingSize);

		Event ev = Event::fromRawTrb(rawEv);
		_controller->processEvent(ev);
	}
}

bool EventRing::hasPendingEvent() {
	_controller->barrier.invalidate(_eventRing.view_buffer());
	return (_eventRing->ent[_dequeue.index].val[3] & 1) == _dequeue.cycle;
}

// ------------------------------------------------------------------------
// ProducerRing
// ------------------------------------------------------------------------

ProducerRing::ProducerRing(Controller *controller)
: _transactions{}, _ring{controller->memoryPool()}, _controller{controller}
, _enqueue{0, true}, _dequeue{0, true} {
	for (size_t i = 0; i < ringSize; i++) {
		_ring->ent[i] = {{0, 0, 0, 0}};
	}

	_controller->barrier.writeback(_ring.view_buffer());
}

async::result<void> ProducerRing::initialize() {
	co_await _controller->dmaSpace().ensure_mapped(_ring);
	_cachedRingIova = _controller->dmaSpace().iova_of(_ring);
	assert(!(_cachedRingIova & 63));
}

uintptr_t ProducerRing::getPtr() {
	assert(_cachedRingIova != 0);
	return _cachedRingIova;
}

void ProducerRing::processEvent(Event ev) {
	assert(ev.type == TrbType::commandCompletionEvent
			|| ev.type == TrbType::transferEvent);

	size_t idx = (ev.trbPointer - getPtr()) / sizeof(RawTrb);
	assert(idx < ringSize);

	auto trb = _ring->ent[idx];
	auto tx = std::exchange(_transactions[idx], nullptr);

	retire({idx, bool(trb.val[3] & 1)});

	if (tx) {
		tx->onEvent(_controller, ev, trb);
	}
}

async::result<RingPointer> ProducerRing::pushTrbs(const std::vector<RawTrb> &trbs, Transaction *tx) {
	assert(trbs.size() <= usableRingSize);

	while (true) {
		co_await _progressEvent.async_wait_if([&] {
			std::unique_lock lock{_mutex};
			return usableRingSize - inFlight() < trbs.size();
		});

		std::unique_lock lock{_mutex};

		if (usableRingSize - inFlight() < trbs.size())
			continue;

		lock.release();
		break;
	}

	std::unique_lock lock{_mutex, std::adopt_lock};

	auto initialPtr = _enqueue;
	auto finalPtr = _enqueue;
	for (size_t i = 0; i < trbs.size(); i++) {
		auto trb = trbs[i];
		// Post all TRBs, except use the incorrect cycle bit for the first.
		// This is to prevent the controller from potentially starting a TD we
		// are still writing TRBs for (and failing because it finishes early).
		trb.val[3] |= i > 0 ? uint32_t{_enqueue.cycle} : uint32_t{!_enqueue.cycle};

		_transactions[_enqueue.index] = tx;
		_ring->ent[_enqueue.index] = trb;

		finalPtr = _enqueue;
		_enqueue.advance(1, usableRingSize);
	}

	// If the TD continues after the Link TRB, the Link TRB has to be part of the chain.
	_updateLink(initialPtr.cycle, finalPtr.cycle != initialPtr.cycle);

	// Make sure this is all visible to the controller.
	_controller->barrier.writeback(_ring.view_buffer());

	// TODO(qookie): Write barrier here.

	// Now, update the first TRB to the correct cycle state.
	auto v = _ring->ent[initialPtr.index].val[3];
	v &= ~uint32_t{1};
	v |= uint32_t{initialPtr.cycle};
	_ring->ent[initialPtr.index].val[3] = v;

	_controller->barrier.writeback(_ring.view_buffer());

	co_return finalPtr;
}

void ProducerRing::retire(RingPointer newDequeue) {
	std::unique_lock lock{_mutex};

	// Make sure we don't accidentally rewind the dequeue.
	assert(newDequeue.cycle != _dequeue.cycle || newDequeue.index >= _dequeue.index);
	// Make sure the dequeue is at or before enqueue.
	assert(newDequeue.cycle != _enqueue.cycle || newDequeue.index <= _enqueue.index);

	_dequeue = newDequeue;
	_progressEvent.raise();
}

void ProducerRing::_updateLink(bool initialCycle, bool chain) {
	if (_enqueue.cycle == initialCycle) return;

	auto ptr = getPtr();
	_ring->ent[ringSize - 1] = {{
		static_cast<uint32_t>(ptr & 0xFFFFFFFF),
		static_cast<uint32_t>(ptr >> 32),
		0,
		static_cast<uint32_t>(initialCycle | (1 << 1) | (chain ? (1 << 4) : 0) | (6 << 10))
	}};
}

// NOTE(qookie): The logic as far as I understand is as follows.
// There are 3 cases to consider that cause events to be generated:
// 1. Successful completion of the whole chain or short packet at the
//    end. Only one event is produced for the final TRB.
// 2. Short packet in the middle of the chain. Two events are
//    produced: one for the TRB that got the short packet, and one
//    for the final TRB that has IOC, so we also need to wait for the
//    latter one in that case.
//    TODO: The second packet not be generated by controllers
//          implementing xHCI 0.96 or older.
// 3. Other error completion. This causes the endpoint to go into the
//    halted state, and only one event is produced for the failing
//    TRB, hence we do not need to wait for any other TRB and can
//    bail out early via FRG_CO_TRY.

// TODO(qookie): The logic in transfer() might not work for isochronous
// endpoints (which we don't support yet) on some controllers (e.g.
// NEC ones). According to the Linux driver, if a TRB in the middle of
// an isoch TD fails, the controller carries on (as it should), but no
// event is generated for the final TRB in the chain (the one with IOC
// set). Other controllers do generate two events though.
async::result<frg::expected<protocols::usb::UsbError, size_t>>
ProducerRing::Transaction::transfer() {
	// This will either be a short packet completion for a data stage/normal TRB,
	// the success completion for the final TRB in the TD, or an error completion.
	auto [trb, ev] = FRG_CO_TRY(co_await nextEvent_());

	if (ev.completionCode == CompletionCode::shortPacket) {
		// This either has to be a data stage or normal TRB.
		auto trbType = static_cast<TrbType>((trb.val[3] >> 10) & 0x3F);
		bool trbChain = trb.val[3] & (1 << 4);
		assert(trbType == TrbType::dataStage || trbType == TrbType::normal);

		// If it's a TRB in the middle of a chain, or the data stage of a
		// control transfer, there's one more event for the full TD.
		if (trbType == TrbType::dataStage || trbChain)
			std::tie(trb, ev) = FRG_CO_TRY(co_await nextEvent_());
	}

	co_return ev.transferLen;
}

async::result<Event> ProducerRing::Transaction::command() {
	co_await progressEvent_.async_wait(progressSeq_);
	co_return events_[progressSeq_++].second;
}

void ProducerRing::Transaction::onEvent(Controller *controller, Event event, RawTrb associatedTrb) {
	if (event.completionCode != CompletionCode::success) {
		auto associatedTrbType = static_cast<TrbType>((associatedTrb.val[3] >> 10) & 63);

		// Ignore short packet completions for transfers
		if (event.type == TrbType::transferEvent && event.completionCode != CompletionCode::shortPacket) {
			std::println("{} Transfer TRB '{}' completed with '{}' (Slot {}, EP {})",
					controller,
					trbTypeNames[static_cast<int>(associatedTrbType)],
					event.completionCodeName(),
					event.slotId, event.endpointId);
		} else if (event.type == TrbType::commandCompletionEvent) {
			std::println("{} Command TRB '{}' completed with '{}'",
					controller,
					trbTypeNames[static_cast<int>(associatedTrbType)],
					event.completionCodeName());
		}
	}

	events_.push_back({associatedTrb, event});
	progressEvent_.raise();
}
