#include <stddef.h>
#include <string.h>

#include <frg/container_of.hpp>

#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/credentials.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

namespace {
	constexpr bool logTransitions = false;
	constexpr bool logRunStates = false;
	constexpr bool logMigration = false;
	constexpr bool logCleanup = false;
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::blockCurrent(Condition checkedConditions) {
	auto ipl = currentIpl();
	assert(ipl < ipl::noSchedule);

	auto thisThread = getCurrentThread();

	// Optimistically clear the unblock latch before entering the mutex.
	// We need acquire semantics to synchronize with unblockOther().
	if(thisThread->_unblockLatch.exchange(false, std::memory_order_acquire))
		return;

	// Note: for performance reasons, callers should check that checkedConditions
	//       are not already set in _pendingConditions before calling this function.

	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thisThread->_mutex);

	// We do not need any memory barrier here: no matter how our aquisition of _mutex
	// is ordered to the aquisition in unblockOther(), we are still correct.
	if(thisThread->_unblockLatch.load(std::memory_order_relaxed))
		return;

	// Conditions can only become pending if the _mutex is held,
	// hence we never move to blocked state while conditions are pending.
	if (thisThread->_pendingConditions.load(std::memory_order_relaxed) & checkedConditions)
		return;

	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is blocked" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_setRunState(kRunBlocked);
	thisThread->unblockConditions_ = checkedConditions;
	localScheduler.get().update();
	Scheduler::suspendCurrent(ipl < ipl::noMigration);
	localScheduler.get().forceReschedule(false);
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), &thisThread->_executor, std::move(lock));
	}, &thisThread->_executor);
}

void Thread::deferCurrent() {
	auto ipl = currentIpl();
	assert(ipl < ipl::noSchedule);

	auto thisThread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&thisThread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is deferred" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_setRunState(kRunDeferred);
	localScheduler.get().update();
	localScheduler.get().forceReschedule(ipl < ipl::noMigration);
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), &thisThread->_executor, std::move(lock));
	}, &thisThread->_executor);
}

void Thread::deferCurrent(IrqImageAccessor image) {
	assert(image.iplState()->current < ipl::noPreemption);

	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_setRunState(kRunDeferred);
	saveExecutor(&this_thread->_executor, image);
	localScheduler.get().update();
	localScheduler.get().forceReschedule(false);
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler.get().commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

void Thread::suspendCurrent(IrqImageAccessor image) {
	assert(image.iplState()->current < ipl::noPreemption);

	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is suspended" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_setRunState(kRunDeferred);
	saveExecutor(&this_thread->_executor, image);
	localScheduler.get().update();
	localScheduler.get().forceReschedule(false);
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler.get().commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

template<typename ImageAccessor>
void Thread::genericInterruptCurrent(Interrupt interrupt, ImageAccessor image, InterruptInfo info) {
	assert(image.iplState()->current < ipl::noPreemption);
	auto thisThread = getCurrentThread();

	// We must never save kernel state into intrImage_.
	if constexpr (!std::same_as<ImageAccessor, SyscallImageAccessor>)
		assert(image.inUserMode());

	ObserveQueue queue;
	{
		StatelessIrqLock irqLock;
		auto lock = frg::guard(&thisThread->_mutex);

		// States other than IntrState::none are only valid while we are in this function.
		assert(thisThread->intrState_ == IntrState::none);
		thisThread->intrState_ = IntrState::inInterrupt;
		thisThread->_lastInterrupt = interrupt;
		thisThread->_interruptInfo = info;
		saveExecutor(&thisThread->intrImage_, image);

		queue.splice(queue.end(), thisThread->_observeQueue);
	}

	while(!queue.empty()) {
		auto node = queue.pop_front();
		async::execution::set_value(node->receiver,
				frg::make_tuple(Error::success, interrupt));
	}

	bool done = false;
	while(!done) {
		auto outcome = asyncBlockCurrentInterruptible(
			async::lambda([&] (async::cancellation_token ct) {
				return thisThread->resumeEvent_.async_wait_if(
					[&] () -> bool {
						StatelessIrqLock irqLock;
						auto lock = frg::guard(&thisThread->_mutex);

						if (thisThread->intrState_ == IntrState::inInterrupt)
							return true;
						assert(thisThread->intrState_ == IntrState::resumeFromInterrupt);

						thisThread->intrState_ = IntrState::none;
						done = true;
						return false;
					},
					ct
				);
			}),
			thisThread->pagingWorkQueue().get(),
			condition::interrupt
		);
		if (!outcome)
			break;
	}

	if (!done) {
		auto pending = thisThread->_pendingConditions.fetch_and(
			~condition::terminate,
			std::memory_order_relaxed
		);
		assert(pending & condition::terminate);
		terminateCurrent_();
		return;
	}

	StatelessIrqLock irqLock;
	// Use runOnStack() to be able to scrub the stack if KASAN is used.
	runOnStack([] (Continuation cont, smarter::borrowed_ptr<Thread> thisThread) {
		scrubStackFrom(thisThread->_userContext.kernelStack.baseAddress(), cont);
		restoreExecutor(&thisThread->intrImage_);
	}, getCpuData()->detachedStack.base(), thisThread);
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image, InterruptInfo info) {
	genericInterruptCurrent(interrupt, image, info);
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image, InterruptInfo info) {
	genericInterruptCurrent(interrupt, image, info);
}

void Thread::interruptCurrent(Interrupt interrupt, IrqImageAccessor image, InterruptInfo info) {
	genericInterruptCurrent(interrupt, image, info);
}

void Thread::launchCurrent_() {
	auto thisThread = getCurrentThread();

	bool done = false;
	while(!done) {
		auto outcome = asyncBlockCurrentInterruptible(
			async::lambda([&] (async::cancellation_token ct) {
				return thisThread->resumeEvent_.async_wait_if(
					[&] () -> bool {
						StatelessIrqLock irqLock;
						auto lock = frg::guard(&thisThread->_mutex);

						if (thisThread->intrState_ == IntrState::inInterrupt)
							return true;
						assert(thisThread->intrState_ == IntrState::resumeFromInterrupt);

						thisThread->intrState_ = IntrState::none;
						done = true;
						return false;
					},
					ct
				);
			}),
			thisThread->pagingWorkQueue().get(),
			condition::interrupt
		);
		if (!outcome)
			break;
	}

	if (!done) {
		auto pending = thisThread->_pendingConditions.fetch_and(
			~condition::terminate,
			std::memory_order_relaxed
		);
		assert(pending & condition::terminate);
		terminateCurrent_();
		return;
	}

	StatelessIrqLock irqLock;
	// Use runOnStack() to be able to scrub the stack if KASAN is used.
	runOnStack([] (Continuation cont, smarter::borrowed_ptr<Thread> thisThread) {
		scrubStackFrom(thisThread->_userContext.kernelStack.baseAddress(), cont);
		restoreExecutor(&thisThread->intrImage_);
	}, getCpuData()->detachedStack.base(), thisThread);
}

void Thread::terminateCurrent_() {
	auto thisThread = getCurrentThread();

	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thisThread->_mutex);

	if(logTransitions)
		infoLogger() << "thor: terminateCurrent_() in " << (void *)thisThread.get() << frg::endlog;
	assert(thisThread->_runState == kRunActive);

	thisThread->_setRunState(kRunTerminated);
	thisThread->_uninvoke();

	localScheduler.get().updateState();
	Scheduler::suspendCurrent(false);
	Scheduler::unassociate(thisThread.get());

	runOnStack([] (Continuation cont,
			Thread *thread, frg::unique_lock<Mutex> lock) {
		scrubStackFrom(thread->_userContext.kernelStack.baseAddress(), cont);

		auto *scheduler = &localScheduler.get();

		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);

		lock.unlock();

		// Release the kernel's reference to the thread after it finished execution.
		auto threadSelf = thread->self;
		threadSelf.policy().decrement();

		// Run observer callbacks before re-scheduling (as callbacks may unblock threads).
		while(!queue.empty()) {
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::threadExited, kIntrNull));
		}

		scheduler->updateQueue();
		scheduler->forceReschedule(false);
		scheduler->commitReschedule();
	}, getCpuData()->detachedStack.base(), thisThread.get(), std::move(lock));
}

template<typename ImageAccessor>
void Thread::migrateCurrentToAssignedCpu(ImageAccessor image) {
	auto this_thread = getCurrentThread();

	auto assignedCpu = this_thread->_lbState.getAssignedCpu();
	if(assignedCpu == getCpuData())
		return;

	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);

	assert(this_thread->_runState == kRunActive);

	// Handle thread migration due to load balancing.
	assert(assignedCpu);
	if(logMigration)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is moved to CPU " << assignedCpu->cpuIndex << frg::endlog;

	this_thread->_setRunState(kRunDeferred);
	saveExecutor(&this_thread->_executor, image);
	localScheduler.get().update();
	Scheduler::suspendCurrent(image.iplState()->current < ipl::noMigration);
	this_thread->_uninvoke();
	Scheduler::resume(this_thread.get(), &localScheduler.get(assignedCpu));
	localScheduler.get().forceReschedule(false);

	runOnStack([] (Continuation cont, ImageAccessor image,
			frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler.get().commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

template<typename ImageAccessor>
void Thread::genericHandleConditions(ImageAccessor image) {
	assert(image.iplState()->current < ipl::noPreemption);
	auto thisThread = getCurrentThread();

	auto clearPending = [&] (Condition c) {
		auto pending = thisThread->_pendingConditions.fetch_and(~c, std::memory_order_acq_rel);
		// Holds because only the thread itself can clear _pendingConditions.
		assert(pending & c);
	};

	auto pending = thisThread->_pendingConditions.load(std::memory_order_relaxed);
	while (pending) {
		// Dequeue the highest pending bit.
		auto c = Condition{1} << frg::floor_log2(pending);

		switch (c) {
			case condition::cpuMigration:
				clearPending(c);
				// Does not return if the thread is actually migrated.
				migrateCurrentToAssignedCpu(image);
				break;
			case condition::passiveWq:
				[[fallthrough]];
			case condition::exceptionalWq:
				// No clearPending(), this is handled by runWq().
				drainWqs();
				break;
			case condition::interrupt:
				clearPending(c);
				interruptCurrent(kIntrRequested, image, {});
				break;
			case condition::terminate:
				clearPending(c);
				terminateCurrent_();
				break;
			default:
				panicLogger() << "Bad thread condition " << c << frg::endlog;
		}
		// Re-load in case new conditions were raised.
		pending = thisThread->_pendingConditions.load(std::memory_order_relaxed);
	}
}

void Thread::handleConditions(SyscallImageAccessor image) {
	genericHandleConditions(image);
}

void Thread::handleConditions(FaultImageAccessor image) {
	genericHandleConditions(image);
}

void Thread::handleConditions(IrqImageAccessor image) {
	genericHandleConditions(image);
}

void Thread::unblockOther(smarter::borrowed_ptr<Thread> thread) {
	// Release semantics ensure that we synchronize with the thread when it flips the flag
	// back to false. Acquire semantics are needed to synchronize with other threads
	// that already set the flag to true in the meantime.
	auto ul = thread->_unblockLatch.exchange(true, std::memory_order_acq_rel);
	if(ul)
		return;

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&thread->_mutex);

	if (thread->_runState != kRunBlocked)
		return;
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thread.get()
				<< " is deferred (via unblock)" << frg::endlog;

	thread->_setRunState(kRunDeferred);
	Scheduler::resume(thread.get());
}

void Thread::killOther(smarter::borrowed_ptr<Thread>) {
	// TODO: This function is a no-op.
	//       We only transition to kRunTerminate when all runnable references
	//       to this thread have been dropped.
	//       This is necessary to ensure that WQs can be drained etc.
	//       Remove or rework this function in the future.
}

void Thread::interruptOther(smarter::borrowed_ptr<Thread> thread) {
	thread->raiseCondition_(condition::interrupt);
}

void Thread::migrateOther(smarter::borrowed_ptr<Thread> thread) {
	thread->raiseCondition_(condition::cpuMigration);
}

Error Thread::resumeOther(smarter::borrowed_ptr<Thread> thread) {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&thread->_mutex);

		if(thread->_runState == kRunTerminated)
			return Error::threadExited;
		if(thread->intrState_ != IntrState::inInterrupt)
			return Error::illegalState;

		thread->intrState_ = IntrState::resumeFromInterrupt;
	}

	thread->resumeEvent_.raise();

	return Error::success;
}

void Thread::raiseCondition_(Condition c) {
	auto irqLock = frg::guard(&irqMutex());

	CpuData *cpuToPing = nullptr;
	bool unblock = false;
	{
		auto lock = frg::guard(&_mutex);

		// TODO: Send an IPI for expedited condition handling.

		// acq_rel ordering to synchronize with code running on the thread that does not take _mutex.
		// For example, this applies to runWqs().
		auto pending = _pendingConditions.fetch_or(
			c, std::memory_order_acq_rel
		);
		if (pending & c)
			return;

		if (_runState == kRunActive) {
			// Suppress IPIs if any condition is already active.
			if (pending)
				return;
			// If the thread is running on another CPU, we ping the other CPU.
			// If it is running on the current CPU, we rely on handleConditions_() on kernel exit code paths.
			if (activeCpu_ == getCpuData())
				return;
			cpuToPing = activeCpu_;
		} else if(_runState == kRunBlocked) {
			// If the thread is blocked and can be interrupted, then unblock it to notify.
			if (!(unblockConditions_ & c))
				return;
			unblock = true;
		}
	}

	if (cpuToPing) {
		sendPingIpi(cpuToPing);
	} else if (unblock) {
		unblockOther(self);
	}
}

Thread::Thread(CtorToken, smarter::borrowed_ptr<Hierarchy> hierarchy,
		smarter::shared_ptr<Universe> universe,
		smarter::shared_ptr<AddressSpace, BindableHandle> address_space, AbiParameters abi)
: flags{0}, _executorContext{ExecutorContext::create(hierarchy->controller())},
		_mainWorkQueue{this, ipl::passiveWork}, _pagingWorkQueue{this, ipl::exceptionalWork},
		_runState{kRunDeferred},
		_runCount{1},
		_executor{&_userContext, &Thread::launchCurrent_},
		intrImage_{&_userContext, abi},
		_universe{std::move(universe)}, _addressSpace{std::move(address_space)},
		_hierarchy{std::move(hierarchy)} {
	_executorContext->mayBlockForMemory = true;
	_lastRunTimeUpdate = getClockNanos();
	_publishLoad();
	// TODO: Alternatively, we could add a separate observation for new launched threads.
	intrState_ = IntrState::inInterrupt;
	_lastInterrupt = kIntrRequested;
}

Thread::~Thread() {
	if(logCleanup)
		infoLogger() << "thor: Thread is destructed" << frg::endlog;
	assert(_runState == kRunTerminated);
	assert(_observeQueue.empty());
	LoadBalancer::singleton().disconnect(this);
	ExecutorContext::retire(_executorContext);
}

// This function has to initiate the thread's shutdown.
void Thread::dispose() {
	if(logCleanup)
		infoLogger() << "thor: Killing thread after no more handles keep it alive" << frg::endlog;
	raiseCondition_(condition::terminate);
}

void Thread::observe_(ObserveNode *node) {
	bool terminated{false};
	Interrupt interrupt{kIntrNull};
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if (_runState == kRunTerminated) {
			terminated = true;
		} else if(intrState_ == IntrState::inInterrupt) {
			interrupt = _lastInterrupt;
		}else{
			_observeQueue.push_back(node);
			return;
		}
	}

	if (terminated) {
		async::execution::set_value(node->receiver,
				frg::make_tuple(Error::threadExited, kIntrNull));
		return;
	}

	async::execution::set_value(node->receiver,
			frg::make_tuple(Error::success, interrupt));
}

UserContext &Thread::getContext() {
	return _userContext;
}

smarter::borrowed_ptr<Universe> Thread::getUniverse() {
	return _universe;
}
smarter::borrowed_ptr<AddressSpace, BindableHandle> Thread::getAddressSpace() {
	return _addressSpace;
}
smarter::borrowed_ptr<Hierarchy> Thread::getHierarchy() {
	return _hierarchy;
}

void Thread::invoke() {
	assert(!intsAreEnabled());
	auto *cpuData = getCpuData();
	auto lock = frg::guard(&_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: "
				<< " " << _credentials[0] << " " << _credentials[1]
				<< " " << _credentials[2] << " " << _credentials[3]
				<< " " << _credentials[4] << " " << _credentials[5]
				<< " " << _credentials[6] << " " << _credentials[7]
				<< " " << _credentials[8] << " " << _credentials[9]
				<< " " << _credentials[10] << " " << _credentials[11]
				<< " " << _credentials[12] << " " << _credentials[13]
				<< " " << _credentials[14] << " " << _credentials[15]
				<< " is activated" << frg::endlog;

	assert(_runState == kRunDeferred);
	_setRunState(kRunActive);
	activeCpu_ = cpuData;

	lock.unlock();

	_userContext.migrate(cpuData);
	AddressSpace::activate(_addressSpace);
	_executorContext->active.store(true, std::memory_order_relaxed);
	cpuData->executorContext = _executorContext;
	cpuData->activeThread = self;
	restoreExecutor(&_executor);
}

void Thread::handlePreemption(Ipl ipl) {
	assert(!intsAreEnabled());
	assert(ipl < ipl::noPreemption);
	assert(getCurrentThread().get() == this);

	auto *scheduler = &localScheduler.get();

	// Done before the scheduler is updated since this may make the CPU's work queue runnable.
	LoadBalancer::singleton().checkOverload();

	scheduler->update();
	auto lock = frg::guard(&_mutex);
	if(scheduler->maybeReschedule(ipl < ipl::noMigration)) {
		if(logRunStates)
			infoLogger() << "thor: " << (void *)this << " is deferred" << frg::endlog;

		assert(_runState == kRunActive);
		_setRunState(kRunDeferred);
		_uninvoke();

		forkExecutor([&] {
			runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
				scrubStack(executor, cont);
				lock.unlock();
				localScheduler.get().commitReschedule();
			}, getCpuData()->detachedStack.base(), &_executor, std::move(lock));
		}, &_executor);
	}else{
		lock.unlock();
		scheduler->renewSchedule();
	}
}

void Thread::handlePreemption(IrqImageAccessor image) {
	assert(!image.inUserMode());
	genericHandlePreemption(image);
}
void Thread::handlePreemption(FaultImageAccessor image) {
	assert(!image.inUserMode());
	genericHandlePreemption(image);
}

template<typename ImageAccessor>
void Thread::genericHandlePreemption(ImageAccessor image) {
	assert(!intsAreEnabled());
	assert(getCurrentThread().get() == this);
	assert(image.iplState()->current < ipl::noPreemption);

	auto *scheduler = &localScheduler.get();

	// Done before the scheduler is updated since this may make the CPU's work queue runnable.
	LoadBalancer::singleton().checkOverload();

	scheduler->update();
	auto lock = frg::guard(&_mutex);
	// The thread is preempted at an arbitrary point in the kernel, which may depend on the CPU.
	if(scheduler->maybeReschedule(false)) {
		if(logRunStates)
			infoLogger() << "thor: " << (void *)this << " is deferred" << frg::endlog;

		assert(_runState == kRunActive);
		_setRunState(kRunDeferred);
		saveExecutor(&_executor, image);
		_uninvoke();

		runOnStack([] (Continuation cont, ImageAccessor image, frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), image, std::move(lock));
	}else{
		lock.unlock();
		scheduler->renewSchedule();
	}
}

void Thread::_setRunState(RunState state) {
	bool wasRunnable = _isRunnable();
	_updateRunTime();
	_runState = state;
	_publishLoad();
	if (_isRunnable() != wasRunnable)
		LoadBalancer::singleton().updateRunnable(this, _lastRunTimeUpdate, !wasRunnable);
}

void Thread::_updateRunTime() {
	// Threads migrate between CPUs, so tolerate clock differences between CPUs (e.g., from TSC errata).
	auto now = frg::max(getClockNanos(), _lastRunTimeUpdate);

	// TODO: Terminated counts as not runnable; we may want to revisit this.
	assert(_runState == kRunActive || _runState == kRunDeferred
			|| _runState == kRunBlocked || _runState == kRunTerminated);
	auto averages = _averagesAt(now);
	_runnableAverage = averages.runnable;
	_runningAverage = averages.running;
	_lastRunTimeUpdate = now;
}

void Thread::_publishLoad() {
	ThreadLoad load{
		.timestamp = _lastRunTimeUpdate,
		.runnable = dropLoadFraction(_runnableAverage),
		.running = dropLoadFraction(_runningAverage),
		.isRunnable = _isRunnable(),
		.isRunning = _runState == kRunActive,
	};
	_publishedLoad.store(load);
}

void Thread::_uninvoke() {
	UserContext::deactivate();

	auto cpuData = getCpuData();
	_executorContext->active.store(false, std::memory_order_relaxed);
	cpuData->executorContext = nullptr;
	cpuData->activeThread = {};
}

void Thread::AssociatedWorkQueue::wakeup() {
	if (wqIpl() == ipl::passiveWork) {
		_thread->raiseCondition_(condition::passiveWq);
	} else {
		assert(wqIpl() == ipl::exceptionalWork);
		_thread->raiseCondition_(condition::exceptionalWq);
	}
}

} // namespace thor
