#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr bool logService = false;
}

// --------------------------------------------------------
// IrqSink
// --------------------------------------------------------

IrqSink::IrqSink(frg::string<KernelAlloc> name)
: _name{std::move(name)}, _currentSequence{0} { }

IrqSink::~IrqSink() {
	if(_pin)
		IrqPin::detachSink(this);
}

void IrqSink::dumpHardwareState() {
	infoLogger() << "thor: No dump available for IRQ sink " << name() << frg::endlog;
}

IrqPin *IrqSink::getPin() {
	return _pin.get();
}

// --------------------------------------------------------
// IRQ management functions.
// --------------------------------------------------------

void IrqPin::attachSink(smarter::shared_ptr<IrqPin> pin, IrqSink *sink) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);
	assert(!sink->_pin);

	// Since the sink is attached in standBy state, it does not matter if the IRQ
	// is in-service or not (the sink does participate anyway).
	assert(sink->_status == IrqStatus::standBy);

	pin->_sinkList.push_back(sink);
	sink->_pin = std::move(pin);
}

void IrqPin::detachSink(IrqSink *sink) {
	// Move the reference out of the sink such that the pin can only die once we drop the locks.
	auto pin = std::move(sink->_pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	// Unlink first: _nack() can re-enter the sink list to dump the remaining sinks.
	pin->_sinkList.erase(pin->_sinkList.iterator_to(sink));

	// The sink still owes an ACK/NACK. NACK on its behalf, otherwise the pin never
	// leaves service.
	if(sink->_status == IrqStatus::indefinite)
		pin->_nack();
}

Error IrqPin::ackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	if(sequence != sink->currentSequence())
		return Error::illegalArgs;

	if(sink->_status != IrqStatus::indefinite)
		return Error::illegalArgs;
	sink->_status = IrqStatus::acked;
	pin->_acknowledge();
	return Error::success;
}

Error IrqPin::nackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	if(sequence != sink->currentSequence())
		return Error::illegalArgs;

	if(sink->_status != IrqStatus::indefinite)
		return Error::illegalArgs;
	sink->_status = IrqStatus::nacked;
	pin->_nack();
	return Error::success;
}

Error IrqPin::kickSink(IrqSink *sink, bool wantClear) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	// If wantClear == true, kickSink() is effictively also an ACK on the sink
	// *if the sink is indefinite right now*.
	//
	// This behavior makes it useful even if for IRQ objects that are not monitored
	// asynchronously (for example if kernlets are used instead).

	if(!wantClear || sink->_status != IrqStatus::indefinite) {
		pin->_kick(false);
		return Error::success;
	}

	if(sink->_status != IrqStatus::indefinite)
		return Error::success;
	sink->_status = IrqStatus::acked;
	pin->_kick(true);
	return Error::success;
}

// --------------------------------------------------------
// IrqPin
// --------------------------------------------------------

IrqPin::IrqPin(frg::string<KernelAlloc> name)
: _name{std::move(name)}, _strategy{0},
		_inService{false}, _dueSinks{0},
		_maskState{0} {
	_hash = frg::hash<frg::string<KernelAlloc>>{}(_name);

	[] (IrqPin *self, enable_detached_coroutine) -> void {
		while(true) {
			co_await self->_unstallEvent.async_wait_if([&] () -> bool {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->_mutex);

				return !(self->_maskState & maskedForNack);
			});

			// Check if the IRQ is still NACKed.
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->_mutex);

				if(!(self->_maskState & maskedForNack))
					continue;
			}

			auto ms = static_cast<uint64_t>(50) * (1 << self->_unstallExponent);
			co_await generalTimerEngine()->sleepFor(static_cast<uint64_t>(50'000'000)
					* (1 << self->_unstallExponent));

			// Kick the IRQ.
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->_mutex);

				if(!(self->_maskState & maskedForNack))
					continue;
				debugLogger() << "thor: Unstalling IRQ " << self->name()
						<< " after " << ms << " ms" << frg::endlog;
				self->_kick(false);
			}
		}
	}(this, enable_detached_coroutine{WorkQueue::criticalQueue().lock()});
}

void IrqPin::configure(IrqConfiguration desired) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(!_configured) {
		infoLogger() << "thor: Configuring IRQ " << _name
				<< " to trigger mode: " << static_cast<int>(desired.trigger)
				<< ", polarity: " << static_cast<int>(desired.polarity) << frg::endlog;
		_strategy = program(desired.trigger, desired.polarity);
		// suppressMaskInService and maskInService are contradictory.
		assert(!((_strategy & irq_strategy::maskInService)
				&& (_strategy & irq_strategy::suppressMaskInService)));

		_activeCfg = desired;
		_configured = true;
		_inService = false;
		_dueSinks = 0;
		_maskState = 0;
	}else{
		assert(_activeCfg.compatible(desired));
	}
}

void IrqPin::raise() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	if(!_strategy) {
		debugLogger() << "thor: Unconfigured IRQ was raised" << frg::endlog;
		dumpHardwareState();
	}

	// If the IRQ is already masked, we're encountering a hardware race.
	if(_hwMaskRequested()) {
		++_maskedRaiseCtr;
		// At least on x86, the IRQ controller may buffer up to one edge-triggered IRQ.
		// If an IRQ is already buffered while we mask it, it will inevitably be raised again.
		// Thus, we do not immediately complain about edge-triggered IRQs here.
		auto complain = (_strategy & irq_strategy::maskInService) || _maskedRaiseCtr > 1;

		if(complain) {
			debugLogger() << "thor: IRQ controller raised "
					<< _name << " despite being masked (" << _maskedRaiseCtr
					<< "x)" << frg::endlog;
			dumpHardwareState();

			for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
				auto lock = frg::guard(&(*it)->_mutex);
				if((*it)->_status == IrqStatus::standBy) {
					infoLogger() << "thor: IRQ sink " << (*it)->name()
							<< " is in standBy state" << frg::endlog;
				}else{
					(*it)->dumpHardwareState();
				}
			}

			infoLogger() << "thor: Sending end-of-interrupt" << frg::endlog;
		}

		if (_strategy & irq_strategy::endOfInterrupt)
			endOfInterrupt();
		if(complain)
			dumpHardwareState();
		return;
	}

	// This can only happen if irq_strategy::maskInService is clear.
	// Otherwise, the IRQ is masked and the previous if would have triggered.
	if(_inService) {
		assert(!(_strategy & irq_strategy::maskInService));
		_raiseBuffered = true;
		if (_strategy & irq_strategy::maskable)
			_maskState |= maskedWhileBuffered;

		_updateMask();
		if (_strategy & irq_strategy::endOfInterrupt)
			endOfInterrupt();
		return;
	}

	_doService();

	_updateMask();
	if (_strategy & irq_strategy::endOfInterrupt)
		endOfInterrupt();
}

void IrqPin::_acknowledge() {
	assert(_inService);
	assert(_dueSinks);
	_dispatchAcks = true;
	_dueSinks--;

	if(!_dueSinks)
		_dispatch();
}

void IrqPin::_nack() {
	assert(_inService);
	assert(_dueSinks);
	_dueSinks--;

	if(!_dueSinks)
		_dispatch();
}

void IrqPin::_kick(bool doClear) {
	if(doClear) {
		assert(_inService);
		assert(_dueSinks);
		_dueSinks--;
	}else{
		if(!_inService)
			return;
	}

	_dispatchKicks = true;

	// Re-dispatch to clear the IRQ.
	// Note that we also do *not* decrement _dueSinks here; in particular, the sink that
	// was kicked might already have decremented _dueSinks.
	if(!_dueSinks)
		_dispatch();
}

// This function is called at the end of IRQ handling.
// It unmasks IRQs that use maskInService and checks for asynchronous NACK.
void IrqPin::_dispatch() {
	if(_dispatchAcks) {
		if(_unstallExponent > 0)
			--_unstallExponent;
	}

	if(_dispatchKicks)
		_maskState &= ~maskedForNack;

	if(_dispatchAcks || _dispatchKicks) {
		if(logService)
			debugLogger() << "thor: IRQ pin " << name()
					<< " is acked (asynchronously)" << frg::endlog;

		_inService = false;
		_maskState &= ~maskedForService;

		if (_strategy & irq_strategy::endOfService)
			endOfService();

		// Avoid losing IRQs that were ignored in raise() as 'already active'.
		if(_raiseBuffered) {
			_raiseBuffered = false;
			_maskState &= ~maskedWhileBuffered;

			_doService();
		}
	}else{
		// Note that _inService returns true for NAKed IRQs.

		urgentLogger() << "thor: IRQ " << _name
				<< " was nacked (asynchronously)!" << frg::endlog;
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
			auto lock = frg::guard(&(*it)->_mutex);
			if((*it)->_status == IrqStatus::standBy) {
				infoLogger() << "thor: IRQ sink " << (*it)->name()
						<< " is in standBy state" << frg::endlog;
			}else{
				(*it)->dumpHardwareState();
			}
		}
		if (_strategy & irq_strategy::maskable) {
			_maskState |= maskedForNack;
			if(_unstallExponent < 8)
				++_unstallExponent;
			_unstallEvent.raise();
		}
	}

	_updateMask();
}

void IrqPin::warnIfPending() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(!_inService || (_maskState & maskedForNack))
		return;

	// The IRQ may have been raised on another CPU, so tolerate clock differences between CPUs.
	auto now = frg::max(getClockNanos(), _raiseClock);
	if(now - _raiseClock > 1000000000 && !_warnedAfterPending) {
		auto log = debugLogger();
		log << "thor: Pending IRQ " << _name << " has not been"
				" acked/nacked for more than one second.";
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
			if((*it)->_status == IrqStatus::indefinite)
				log << "\n   Sink " << (*it)->name() << " has not acked/nacked";
		log << frg::endlog;
		_warnedAfterPending = true;
	}
}

void IrqPin::dumpHardwareState() {
	infoLogger() << "thor: No dump available for IRQ pin " << name() << frg::endlog;
}

void IrqPin::endOfInterrupt() {
	// Default implementation is a no-op: not all IRQ controllers need endOfInterrupt().
}

void IrqPin::endOfService() {
	// Default implementation is a no-op: not all IRQ controllers need endOfService().
}

void IrqPin::_doService() {
	assert(!_inService);
	assert(!_raiseBuffered);

	// suppressMaskInService lines run unmasked while in service. A masked line cannot have been
	// claimed, so _maskState is clear here; unmask defensively in case it was masked externally.
	if(_strategy & irq_strategy::suppressMaskInService) {
		assert(!_maskState);
		unmask();
	}

	if(logService)
		infoLogger() << "thor: IRQ pin "
				<< _name << " enters service" << frg::endlog;

	_inService = true;
	// maskInService IRQs are masked while then are in service.
	if(_strategy & irq_strategy::maskInService)
		_maskState |= maskedForService;

	_dueSinks = 0;
	_dispatchAcks = false;
	_dispatchKicks = false;

	_raiseClock = getClockNanos();
	_warnedAfterPending = false;

	if(_sinkList.empty())
		debugLogger() << "thor: No sink for IRQ " << _name << frg::endlog;

	unsigned int numAsynchronous = 0;
	bool anyAck = false;
	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
		auto lock = frg::guard(&(*it)->_mutex);
		++((*it)->_currentSequence);
		auto status = (*it)->raise();
		(*it)->_status = status;

		if(status == IrqStatus::acked) {
			anyAck = true;
		}else if(status == IrqStatus::nacked) {
			// We do not need to do anything here; we just do not increment numAsynchronous.
		}else{
			numAsynchronous++;
		}
	}

	if(!numAsynchronous) {
		if(anyAck) {
			if(logService)
				infoLogger() << "thor: IRQ pin " << name()
						<< " is acked (synchronously)" << frg::endlog;

			if(_unstallExponent > 0)
				--_unstallExponent;

			_inService = false;
			_maskState &= ~maskedForService;

			if (_strategy & irq_strategy::endOfService)
				endOfService();
		}else{
			urgentLogger() << "thor: IRQ " << _name << " was nacked (synchronously)!" << frg::endlog;
			for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
				auto lock = frg::guard(&(*it)->_mutex);
				assert((*it)->_status != IrqStatus::standBy);
				(*it)->dumpHardwareState();
			}

			if (_strategy & irq_strategy::maskable) {
				_maskState |= maskedForNack;
				if(_unstallExponent < 8)
					++_unstallExponent;
				_unstallEvent.raise();
			}
		}
		return;
	}

	// The IRQ is handled asynchronously.

	if(anyAck)
		_dispatchAcks = true;
	_dueSinks = numAsynchronous;
}

bool IrqPin::_hwMaskRequested() {
	// suppressMaskInService controllers must not be masked while in service (completing a masked
	// source is dropped); they suppress the in-service IRQ themselves. Out of service _maskState
	// drives the hardware mask normally.
	if((_strategy & irq_strategy::suppressMaskInService) && _inService)
		return false;
	return _maskState;
}

void IrqPin::_updateMask() {
	// TODO: Avoid the virtual calls if the state does not change?
	if(!_hwMaskRequested()) {
		_maskedRaiseCtr = 0;
		unmask();
	}else{
		assert(_strategy & irq_strategy::maskable);
		mask();
	}
}

// --------------------------------------------------------
// IrqObject
// --------------------------------------------------------

// We create the IrqObject in latched state in order to ensure that users to not miss IRQs
// that happened before the object was created.
// However this can result in spurious raises.
IrqObject::IrqObject(frg::string<KernelAlloc> name)
: IrqSink{std::move(name)} { }

void IrqObject::finalizeBeforeRcu() {
	if(getPin())
		IrqPin::detachSink(this);
}

std::expected<smarter::shared_ptr<GenericIrqObject>, Error> GenericIrqObject::create(
		frg::string<KernelAlloc> name) {
	auto ptr = allocate_rcu_shared<GenericIrqObject>(*kernelAlloc, CtorToken{}, std::move(name));
	return ptr;
}

// TODO: Add a sequence parameter to this function and run the kernlet if the sequence advanced.
//       This would prevent races between automate() and IRQs.
void IrqObject::automate(smarter::shared_ptr<BoundKernlet> kernlet) {
	_automationKernlet = std::move(kernlet);
}

IrqStatus IrqObject::raise() {
	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->error_ = Error::success;
		node->sequence_ = currentSequence();
		if (node->cancelCb_.try_reset())
			node->wq_->post(node->awaited_);
	}

	if(_automationKernlet) {
		auto result = _automationKernlet->invokeIrqAutomation();
		if(result == 1) {
			return IrqStatus::acked;
		}else if(result == 2) {
			return IrqStatus::nacked;
		}else{
			assert(!result);
			infoLogger() << "thor: IRQ automation does not handle the IRQ?" << frg::endlog;
			return IrqStatus::indefinite;
		}
	}else
		return IrqStatus::indefinite;
}

void IrqObject::submitAwait(AwaitIrqNode *node, uint64_t sequence) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(sinkMutex());

	assert(sequence <= currentSequence());
	if(sequence < currentSequence()) {
		node->error_ = Error::success;
		node->sequence_ = currentSequence();
		node->wq_->post(node->awaited_);
	}else{
		if(!node->cancelCb_.try_set(node->cancelToken_)) {
			node->wasCancelled_ = true;
			node->wq_->post(node->awaited_);
			return;
		}

		_waitQueue.push_back(node);
	}
}

void AwaitIrqNode::cancel() {
	wasCancelled_ = true;
	irq_->_waitQueue.erase(irq_->_waitQueue.iterator_to(this));
	wq_->post(awaited_);
}

} // namespace thor
