#include <assert.h>

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/ostrace.hpp>
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
				iplLeaveContext(IplState{.context = ipl::passive, .current = ipl::exceptional});
				suspendSelf();
				__builtin_trap();
			}, getCpuData()->idleStack.base());
			__builtin_trap();
		}

		void handlePreemption(IrqImageAccessor image) override {
			assert(!image.inUserMode());
			auto *scheduler = &localScheduler.get();
			scheduler->update();
			if(scheduler->maybeReschedule()) {
				runOnStack([] (Continuation cont, IrqImageAccessor image) {
					scrubStack(image, cont);
					localScheduler.get().commitReschedule();
				}, getCpuData()->detachedStack.base(), image);
			}else{
				scheduler->renewSchedule();
			}
		}

		void handlePreemption() override {
			StatelessIrqLock irqLock;
			auto *scheduler = &localScheduler.get();
			scheduler->update();
			if(scheduler->maybeReschedule()) {
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

ScheduleEntity::ScheduleEntity(ScheduleType type)
: type_{type}, state{ScheduleState::null}, priority{0}, _refClock{0}, _runTime{0},
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
	assert(entity->type() == ScheduleType::regular);

//	infoLogger() << "resume " << entity << frg::endlog;
	assert(entity->state == ScheduleState::attached);

	auto self = entity->_scheduler;
	assert(self);
	assert(entity != self->_current);
	bool wasEmpty;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&self->_mutex);

		entity->state = ScheduleState::pending;

		wasEmpty = self->_pendingList.empty();
		self->_pendingList.push_back(entity);
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
			sendPingIpi(self->_cpuContext);
		}
	}
}

void Scheduler::suspendCurrent() {
	assert(!intsAreEnabled());

	auto self = &localScheduler.get();
	auto entity = self->_current;
	assert(entity);
	assert(entity->type() == ScheduleType::regular);
//	infoLogger() << "suspend " << entity << frg::endlog;

	// Update the unfairness on suspend.
	self->_updateEntityStats(entity);
	entity->state = ScheduleState::attached;

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
	updateState();
	updateQueue();
}

void Scheduler::updateState() {
	// Returns the reciprocal in fixed point format.
	auto fixedInverse = [] (int64_t x) -> int64_t {
		return (INT64_C(1) << progressShift) / x;
	};

	assert(_current);

	// Number of waiting/running threads.
	auto n = _numWaiting;
	if(_current->type() == ScheduleType::regular)
		n++;

	assert(haveTimer());
	auto now = getClockNanos();
	auto deltaTime = now - _refClock;
	_refClock = now;
	if(n)
		_systemProgress += deltaTime * static_cast<Progress>(fixedInverse(n));

	_updateCurrentEntity();
}

// Move entities from the pending queue to the waiting queue.
void Scheduler::updateQueue() {
	frg::intrusive_list<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::default_list_hook<ScheduleEntity>,
			&ScheduleEntity::listHook
		>
	> pendingSnapshot;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		pendingSnapshot.splice(pendingSnapshot.end(), _pendingList);
	}
	while(!pendingSnapshot.empty()) {
		auto entity = pendingSnapshot.pop_front();
		assert(entity->state == ScheduleState::pending);

		// Update the unfairness reference.
		entity->refProgress = _systemProgress;
		entity->_refClock = _refClock;
		entity->state = ScheduleState::active;

		_waitQueue.push(entity);
		_numWaiting++;
	}
}

bool Scheduler::maybeReschedule() {
	assert(!intsAreEnabled());
	assert(_current);

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

	_unschedule();
	_schedule();
	return true;
}

void Scheduler::forceReschedule() {
	assert(!intsAreEnabled());

	if(_current)
		_unschedule();
	_schedule();
}

[[noreturn]] void Scheduler::commitReschedule() {
	assert(!_current);
	assert(_scheduled);

	_current = _scheduled;
	_scheduled = nullptr;
	_sliceClock = _refClock;
	_mustCallPreemption = false;

	if(!getPreemptionDeadline())
		_updatePreemption();

	currentRunnable()->invoke();
}

void Scheduler::renewSchedule() {
	_mustCallPreemption = false;

	if(!getPreemptionDeadline())
		_updatePreemption();
}

ScheduleEntity *Scheduler::currentRunnable() {
	assert(_current);
	return _current;
}

void Scheduler::_unschedule() {
	assert(_current);

	// Decrease the unfairness at the end of the time slice.
	_updateEntityStats(_current);

	if(_current->type() == ScheduleType::regular
			|| _current->state == ScheduleState::active) {
		_waitQueue.push(_current);
		_numWaiting++;
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

	// TODO: Re-enable once ostrace can emit from IRQ context without allocating.
	//ostrace::emit(ostEvtArmPreemption);
	setPreemptionDeadline(getClockNanos() + sliceGranularity);
}

void Scheduler::_updateCurrentEntity() {
	assert(_current);
	if(_current->type() == ScheduleType::idle)
		return;
	assert(_current->type() == ScheduleType::regular);

	auto delta_progress = _systemProgress - _current->refProgress;
	if(logUpdates)
		infoLogger() << "Running thread unfairness decreases by: "
				<< progressToNanos(_numWaiting * delta_progress) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frg::endlog;
	_current->baseUnfairness -= _numWaiting * delta_progress;
	_current->refProgress = _systemProgress;
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

void checkThreadPreemption() {
	assert(!intsAreEnabled());
	auto thisThread = getCurrentThread();
	auto *scheduler = &localScheduler.get();

	scheduler->suppressRenewalUntilInterrupt();
	if (!scheduler->mustCallPreemption())
		return;
	thisThread->handlePreemption();
}
void checkThreadPreemption(FaultImageAccessor image) {
	doCheckThreadPreemption(image);
}

THOR_DEFINE_PERCPU(localScheduler);

smarter::borrowed_ptr<Thread> getCurrentThread() {
	return getCpuData()->activeThread;
}

} // namespace thor

