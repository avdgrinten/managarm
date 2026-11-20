#include <assert.h>

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/idle.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr bool logScheduling = false;
	constexpr bool logNextBest = false;
	constexpr bool logUpdates = false;
	constexpr bool logIdle = false;

	constexpr bool disablePreemption = false;

	// Whether a CPU that would go idle takes waiting threads from other CPUs.
	constexpr bool enableStealing = true;

	// Minimum length of a preemption time slice in ns.
	constexpr int64_t sliceGranularity = 10'000'000;

	struct IdleTask final : ScheduleEntity {
		IdleTask()
		: ScheduleEntity{ScheduleType::idle} { }

		[[noreturn]] void invoke() override {
			runOnStack([] (Continuation) {
				if(logIdle)
					infoLogger() << "System is idle" << frg::endlog;
				// Restore IPL (as in restoreExecutor() for threads/fibers).
				iplLeaveContext(IplState{.context = ipl::passive, .current = ipl::interrupt});
				while(true) {
					auto method = determineIdleState();

					// Note: rcuClearQuiescent() is also done by the interrupt entry path.
					//       However, some idle methods (e.g., mwait) can return without actually seeing an interrupt,
					//       so we have to also perform it below.
					iplLower(ipl::interrupt, ipl::exceptional);
					rcuSetQuiescent();
					idleUntilInterrupt(method);
					rcuClearQuiescent();
					iplRaise(ipl::interrupt);
					if(consumeIdlePing()) {
						auto *scheduler = &localScheduler.get();
						scheduler->forcePreemptionCall();
						scheduler->checkPreemption(ipl::passive);
					}
					// If we re-schedule in checkPreemption() above, we do not return here.
					// In that case, noteIdleWakeup(false) is skipped here and handlePreemption() does noteIdleWakeup(true).
					noteIdleWakeup(false);
				}
			}, getCpuData()->idleStack.base());
			__builtin_trap();
		}

		void handlePreemption(IrqImageAccessor image) override {
			assert(!image.inUserMode());
			consumeIdlePing();
			auto *scheduler = &localScheduler.get();
			scheduler->update();
			if(scheduler->maybeReschedule(false)) {
				noteIdleWakeup(true);
				runOnStack([] (Continuation cont, IrqImageAccessor image) {
					scrubStack(image, cont);
					localScheduler.get().commitReschedule();
				}, getCpuData()->detachedStack.base(), image);
			}else{
				scheduler->renewSchedule();
			}
		}

		void handlePreemption(Ipl) override {
			StatelessIrqLock irqLock;
			consumeIdlePing();
			auto *scheduler = &localScheduler.get();
			scheduler->update();
			if(scheduler->maybeReschedule(false)) {
				noteIdleWakeup(true);
				runOnStack([] (Continuation) {
					localScheduler.get().commitReschedule();
				}, getCpuData()->detachedStack.base());
			}else{
				scheduler->renewSchedule();
			}
		}
	};

	frg::eternal<IdleTask> globalIdleTask;
}

int ScheduleEntity::orderPriority(const ScheduleEntity *a, const ScheduleEntity *b) {
	assert(a->type() == ScheduleType::regular);
	assert(b->type() == ScheduleType::regular);
	return b->priority - a->priority; // Prefer larger priority.
}

bool ScheduleEntity::scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b) {
	assert(a->type() == ScheduleType::regular);
	assert(b->type() == ScheduleType::regular);
	return a->baseUnfairness - a->refProgress
			> b->baseUnfairness - b->refProgress; // Prefer greater unfairness.
}

ScheduleEntity::ScheduleEntity(ScheduleType type, bool pinned)
: type_{type}, pinned_{pinned}, state{ScheduleState::null}, priority{0}, migratable_{!pinned},
		_refClock{0}, _runTime{0},
		refProgress{0}, baseUnfairness{0} { }

ScheduleEntity::~ScheduleEntity() {
	assert(state == ScheduleState::null);
}

void Scheduler::associate(ScheduleEntity *entity, Scheduler *scheduler) {
	assert(entity->type() == ScheduleType::regular);

//	infoLogger() << "associate " << entity << frg::endlog;
	assert(entity->state == ScheduleState::null);
	entity->_scheduler = scheduler;
	entity->state = ScheduleState::attached;
}

void Scheduler::unassociate(ScheduleEntity *entity) {
	assert(entity->type() == ScheduleType::regular);

	// TODO: This is only really need to assert against _current.
	auto irqLock = frg::guard(&irqMutex());

	auto self = entity->_scheduler;
	assert(self);

	assert(entity->state == ScheduleState::attached);
	assert(entity != self->_current);
	entity->_scheduler = nullptr;
	entity->state = ScheduleState::null;
}

void Scheduler::setPriority(ScheduleEntity *entity, int priority) {
	assert(entity->type() == ScheduleType::regular);

	auto scheduleLock = frg::guard(&irqMutex());

	auto self = entity->_scheduler;
	assert(self);

	// Otherwise, we would have to remove-reinsert into the queue.
	assert(entity == self->_current);

	entity->priority = priority;
}

void Scheduler::resume(ScheduleEntity *entity) {
	resume(entity, entity->_scheduler);
}

void Scheduler::resume(ScheduleEntity *entity, Scheduler *target) {
	assert(entity->type() == ScheduleType::regular);

//	infoLogger() << "resume " << entity << frg::endlog;
	assert(entity->state == ScheduleState::attached);

	auto self = target;
	assert(self);
	entity->_scheduler = self;
	bool wasEmpty;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&self->_mutex);

		assert(entity != self->_current);
		entity->state = ScheduleState::pending;

		wasEmpty = self->_pendingList.empty();
		self->_pendingList.push_back(entity);
		self->numQueued_.fetch_add(1, std::memory_order_relaxed);
	}

	if(wasEmpty) {
		if(self == &localScheduler.get()) {
			// Note that IPIs have a significant cost (especially within virtual machines)
			// that we want to avoid if possible.
			//
			// Resuming an entity on the current CPU never needs an IPI to guarantee progress:
			// - If this function is called from a IRQ handler, fault handler or syscall,
			//   no ping is necessary since the kernel checks whether we need to reschedule
			//   before exiting the IRQ/fault/syscall handler.
			// - Otherwise, this function is called from a kernel fiber that eventually blocks.

			// TODO: In the case of kernel threads, it can be necessary to issue a self IPI
			//       to ensure that a higher priority thread gets to run as soon as possible.
			self->_mustCallPreemption = true;
		}else{
			if(!tryPingIdle(self->_cpuContext))
				sendPingIpi(self->_cpuContext);
		}
	}
}

bool Scheduler::moveQueued(ScheduleEntity *entity, Scheduler *target) {
	assert(!intsAreEnabled());
	assert(entity->type() == ScheduleType::regular);

	auto self = entity->_scheduler;
	if(!self || self == target)
		return false;
	if(!self->_takeQueued(entity))
		return false;
	resume(entity, target);
	return true;
}

void Scheduler::suspendCurrent(bool migratable) {
	assert(!intsAreEnabled());

	auto self = &localScheduler.get();
	auto lock = frg::guard(&self->_mutex);
	auto entity = self->_current;
	assert(entity);
	assert(entity->type() == ScheduleType::regular);
//	infoLogger() << "suspend " << entity << frg::endlog;

	// Update the unfairness on suspend.
	self->_updateEntityStats(entity);
	entity->state = ScheduleState::attached;
	entity->migratable_ = migratable && !entity->pinned_;

	self->_current = nullptr;
}

Scheduler::Scheduler(CpuData *cpuContext)
: _cpuContext{cpuContext}, _current{&globalIdleTask.get()} { }

Progress Scheduler::_liveUnfairness(const ScheduleEntity *entity) {
	assert(entity->type() == ScheduleType::regular);
	assert(entity->state == ScheduleState::active);

	auto delta_progress = _systemProgress - entity->refProgress;
	if(entity == _current) {
		return entity->baseUnfairness - _numWaiting * delta_progress;
	}else{
		return entity->baseUnfairness + delta_progress;
	}
}

int64_t Scheduler::_liveRuntime(const ScheduleEntity *entity) {
	assert(entity->type() == ScheduleType::regular);
	assert(entity->state == ScheduleState::active);

	if(entity == _current) {
		return entity->_runTime + (_refClock - entity->_refClock);
	}else{
		return entity->_runTime;
	}
}

void Scheduler::suppressRenewalUntilInterrupt() {
	if (getPreemptionDeadline())
		_mustCallPreemption = false;
}

void Scheduler::update() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	_updateState();
	_updateQueue();
}

void Scheduler::updateState() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	_updateState();
}

void Scheduler::updateQueue() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	_updateQueue();
}

void Scheduler::_updateState() {
	// Returns the reciprocal in fixed point format.
	auto fixedInverse = [] (int64_t x) -> int64_t {
		return (INT64_C(1) << progressShift) / x;
	};

	// Since other CPUs also update the state (in _takeQueued()), the entity that this CPU
	// has picked may not be current yet, or no entity may be picked at all.
	auto running = _current ? _current : _scheduled;

	// Number of waiting/running threads.
	auto n = _numWaiting;
	if(running && running->type() == ScheduleType::regular)
		n++;

	assert(haveTimer());
	// Since other CPUs also update the state, tolerate clock differences between CPUs (e.g., from TSC errata).
	auto now = frg::max(getClockNanos(), _refClock);
	auto deltaTime = now - _refClock;
	_refClock = now;
	if(n)
		_systemProgress += deltaTime * static_cast<Progress>(fixedInverse(n));

	if(running)
		_updateRunningEntity(running);
}

// Move entities from the pending queue to the waiting queue.
void Scheduler::_updateQueue() {
	while(!_pendingList.empty()) {
		auto entity = _pendingList.pop_front();
		assert(entity->state == ScheduleState::pending);

		// Update the unfairness reference.
		entity->refProgress = _systemProgress;
		entity->_refClock = _refClock;
		entity->state = ScheduleState::active;

		_waitQueue.push(entity);
		_numWaiting++;
	}
}

bool Scheduler::maybeReschedule(bool migratable) {
	assert(!intsAreEnabled());
	assert(_current);
	auto lock = frg::guard(&_mutex);

	auto wantToSchedule = [this] () -> bool {
		// If there are no waiters, we keep the current entity.
		// Otherwise, if the current entity is not active anymore, we always switch.
		if(_waitQueue.empty())
			return false;

		if(_current->type() == ScheduleType::idle)
			return true;
		assert(_current->type() == ScheduleType::regular);
		assert(_current->state == ScheduleState::active);

		// Switch based on entity priority.
		if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po > 0) {
			return true;
		}else if(po < 0) {
			return false;
		}

		// Switch based on unfairness.
		auto diff = _liveUnfairness(_current)
				+ (static_cast<Progress>(sliceGranularity) << progressShift)
				- _liveUnfairness(_waitQueue.top());
		return diff < 0;
	};

	if(!wantToSchedule())
		return false;

	_unschedule(migratable);
	_schedule();
	return true;
}

void Scheduler::forceReschedule(bool migratable) {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	if(_current)
		_unschedule(migratable);
	_schedule();
}

[[noreturn]] void Scheduler::commitReschedule() {
	assert(!intsAreEnabled());
	assert(!_current);
	assert(_scheduled);

	// Done here instead of in forceReschedule() since stealing takes the locks of the stolen thread,
	// while callers of forceReschedule() may still hold the mutex of the current thread.
	if(_scheduled->type() == ScheduleType::idle)
		_steal();

	{
		auto lock = frg::guard(&_mutex);

		_current = _scheduled;
		_scheduled = nullptr;
		_sliceClock = _refClock;
		_mustCallPreemption = false;

		if(!getPreemptionDeadline())
			_updatePreemption();
	}

	currentRunnable()->invoke();
}

void Scheduler::renewSchedule() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);

	_mustCallPreemption = false;

	if(!getPreemptionDeadline())
		_updatePreemption();
}

ScheduleEntity *Scheduler::currentRunnable() {
	assert(_current);
	return _current;
}

void Scheduler::_unschedule(bool migratable) {
	assert(_current);

	// Decrease the unfairness at the end of the time slice.
	_updateEntityStats(_current);

	if(_current->type() == ScheduleType::regular
			|| _current->state == ScheduleState::active) {
		_current->migratable_ = migratable && !_current->pinned_;
		_waitQueue.push(_current);
		_numWaiting++;
		numQueued_.fetch_add(1, std::memory_order_relaxed);
	}

	_current = nullptr;
}

void Scheduler::_schedule() {
	assert(!_current);
	assert(!_scheduled);

	if(_waitQueue.empty()) {
		if(logScheduling)
			infoLogger() << "No entities to schedule" << frg::endlog;
		_scheduled = &globalIdleTask.get();
		return;
	}

	auto entity = _waitQueue.top();
	_waitQueue.pop();
	_numWaiting--;
	numQueued_.fetch_sub(1, std::memory_order_relaxed);
	entity->migratable_ = false;

	// Increase the unfairness at the start of the time slice.
	assert(entity->state == ScheduleState::active);
	_updateWaitingEntity(entity);
	_updateEntityStats(entity);

	if(logScheduling) {
//		infoLogger() << "System progress: " << progressToNanos(_systemProgress) / (1000 * 1000)
//				<< " ms" << frg::endlog;
		infoLogger() << "Running entity with priority: " << entity->priority
				<< ", unfairness: " << progressToNanos(_liveUnfairness(entity)) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(entity) / (1000 * 1000)
				<< " ms (" << (_numWaiting + 1) << " active threads)" << frg::endlog;
	}
	if(logNextBest && !_waitQueue.empty())
		infoLogger() << "    Next entity has priority: " << _waitQueue.top()->priority
				<< ", unfairness: " << progressToNanos(_liveUnfairness(_waitQueue.top())) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(_waitQueue.top()) / (1000 * 1000)
				<< " ms" << frg::endlog;

	_scheduled = entity;
}

void Scheduler::_steal() {
	assert(!intsAreEnabled());
	assert(_scheduled && _scheduled->type() == ScheduleType::idle);
	if(!enableStealing)
		return;

	if(!LoadBalancer::singleton().stealFor(_cpuContext))
		return;

	auto lock = frg::guard(&_mutex);

	// Account for the time up to now before the stolen thread starts to wait here.
	_updateState();
	// The stolen thread is pending on this scheduler.
	_updateQueue();
	_scheduled = nullptr;
	_schedule();
}

ScheduleEntity *Scheduler::peekMigratable() {
	assert(!intsAreEnabled());
	if(!numQueued())
		return nullptr;

	auto lock = frg::guard(&_mutex);

	if(_waitQueue.empty())
		return nullptr;
	auto *entity = _waitQueue.top();
	if(!entity->migratable_)
		return nullptr;
	return entity;
}

bool Scheduler::_takeQueued(ScheduleEntity *entity) {
	auto lock = frg::guard(&_mutex);
	assert(entity->_scheduler == this);

	// Active entities are only migratable while they are in the wait queue.
	if(entity->state != ScheduleState::active || !entity->migratable_)
		return false;

	// The progress rate depends on _numWaiting, so account for the time up to now first.
	_updateState();
	_waitQueue.remove(entity);
	_numWaiting--;
	numQueued_.fetch_sub(1, std::memory_order_relaxed);
	// Credit the time that the entity waited on this CPU.
	_updateWaitingEntity(entity);
	entity->state = ScheduleState::attached;
	// Keep the entity on its new CPU until it runs, such that other CPUs do not take it again.
	entity->migratable_ = false;
	return true;
}

// Returns true if preemption should be done immediately.
void Scheduler::_updatePreemption() {
	if(disablePreemption)
		return;

	// Disable preemption if there are no other threads.
	if(_waitQueue.empty())
		return;

	// If there was no current entity, we would have rescheduled.
	assert(_current);
	assert(_current->type() == ScheduleType::regular);
	assert(_current->state == ScheduleState::active);

	if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po < 0) {
		// Disable preemption if we have higher priority.
		return;
	}else{
		// If there was an entity with higher priority, we would have rescheduled.
		assert(!po);
	}

	setPreemptionDeadline(getClockNanos() + sliceGranularity);
}

void Scheduler::_updateRunningEntity(ScheduleEntity *entity) {
	if(entity->type() == ScheduleType::idle)
		return;
	assert(entity->type() == ScheduleType::regular);

	auto delta_progress = _systemProgress - entity->refProgress;
	if(logUpdates)
		infoLogger() << "Running thread unfairness decreases by: "
				<< progressToNanos(_numWaiting * delta_progress) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frg::endlog;
	entity->baseUnfairness -= _numWaiting * delta_progress;
	entity->refProgress = _systemProgress;
}

void Scheduler::_updateWaitingEntity(ScheduleEntity *entity) {
	assert(entity->type() == ScheduleType::regular);
	assert(entity->state == ScheduleState::active);
	assert(entity != _current);

	if(logUpdates)
		infoLogger() << "Waiting thread unfairness increases by: "
				<< progressToNanos(_systemProgress - entity->refProgress) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frg::endlog;
	entity->baseUnfairness += _systemProgress - entity->refProgress;
	entity->refProgress = _systemProgress;
}

void Scheduler::_updateEntityStats(ScheduleEntity *entity) {
	if(entity->type() == ScheduleType::idle)
		return;
	assert(entity->type() == ScheduleType::regular);
	assert(entity->state == ScheduleState::active
			|| entity == _current);

	if(entity == _current)
		entity->_runTime += _refClock - entity->_refClock;
	entity->_refClock = _refClock;
}

namespace {

template<typename ImageAccessor>
void doCheckThreadPreemption(ImageAccessor image) {
	assert(!intsAreEnabled());
	auto thisThread = getCurrentThread();
	auto *scheduler = &localScheduler.get();

	// For IRQs, we call simply call
	//     currentRunnable()->handlePreemption(image)
	// However, since we know that only threads can performs syscalls,
	// we can avoid a virtual call here and directly call into Thread::handlePreemption().

	scheduler->suppressRenewalUntilInterrupt();
	if (!scheduler->mustCallPreemption())
		return;
	if (deferPreemption(image))
		return;
	thisThread->handlePreemption(image);
}

} // namespace

void checkThreadPreemption(Ipl ipl) {
	assert(!intsAreEnabled());
	auto thisThread = getCurrentThread();
	auto *scheduler = &localScheduler.get();

	scheduler->suppressRenewalUntilInterrupt();
	if (!scheduler->mustCallPreemption())
		return;
	thisThread->handlePreemption(ipl);
}
void checkThreadPreemption(FaultImageAccessor image) {
	doCheckThreadPreemption(image);
}

THOR_DEFINE_PERCPU(localScheduler);

smarter::borrowed_ptr<Thread> getCurrentThread() {
	auto thread = getCpuData()->activeThread;
	if(!thread)
		return {};
	return thread->self;
}

} // namespace thor
