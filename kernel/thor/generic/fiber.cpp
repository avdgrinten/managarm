#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

initgraph::Stage *getFibersAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.fibers-available"};
	return &s;
}

void KernelFiber::blockCurrent(FiberBlocker *blocker) {
	auto this_fiber = thisFiber();
	while(true) {
		// Run the WQ outside of the locks.
		auto ipl = currentIpl();
		if (ipl < ipl::exceptionalWork)
			this_fiber->_associatedWorkQueue->run();

		StatelessIrqLock irq_lock;
		auto lock = frg::guard(&this_fiber->_mutex);
		
		// Those are the important tests; they are protected by the fiber's mutex.
		if(blocker->_done)
			break;
		if (ipl < ipl::exceptionalWork) {
			if(this_fiber->_associatedWorkQueue->check())
				continue;
		}

		assert(!this_fiber->_blocked);
		this_fiber->_blocked = true;
		this_fiber->_executorContext->active.store(false, std::memory_order_relaxed);
		getCpuData()->executorContext = nullptr;
		getCpuData()->activeFiber = nullptr;
		localScheduler.get().update();
		Scheduler::suspendCurrent(false);
		localScheduler.get().forceReschedule(false);

		forkExecutor([&] {
			runOnStack([] (Continuation cont, Executor *executor,
					frg::unique_lock<frg::ticket_spinlock> lock) {
				scrubStack(executor, cont);
				lock.unlock();
				localScheduler.get().commitReschedule();
			}, getCpuData()->detachedStack.base(), &this_fiber->_executor, std::move(lock));
		}, &this_fiber->_executor);
	}
}

void KernelFiber::exitCurrent() {
	FiberBlocker blocker;
	blocker.setup();
	KernelFiber::blockCurrent(&blocker);
}

void KernelFiber::unblockOther(FiberBlocker *blocker) {
	auto fiber = blocker->_fiber;
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&fiber->_mutex);

	assert(!blocker->_done);
	blocker->_done = true;

	if(!fiber->_blocked)
		return;
	
	fiber->_blocked = false;
	Scheduler::resume(fiber);
}

void KernelFiber::run(UniqueKernelStack stack,
		void (*function)(void *), void *argument, Scheduler *scheduler) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, scheduler);
	Scheduler::resume(fiber);
}

KernelFiber *KernelFiber::post(UniqueKernelStack stack,
		void (*function)(void *), void *argument, Scheduler *scheduler) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, scheduler);
	return fiber;
}

KernelFiber::KernelFiber(UniqueKernelStack stack, AbiParameters abi)
: ScheduleEntity{ScheduleType::regular, true},
		_blocked{false}, _fiberContext{std::move(stack)}, _executor{&_fiberContext, abi} {
	_associatedWorkQueue = smarter::allocate_shared<AssociatedWorkQueue>(*kernelAlloc, this);
	_executorContext->exceptionalWq = _associatedWorkQueue.get();
}

KernelFiber::~KernelFiber() {
	ExecutorContext::retire(_executorContext);
}

void KernelFiber::invoke() {
	assert(!intsAreEnabled());

	_executorContext->active.store(true, std::memory_order_relaxed);
	getCpuData()->executorContext = _executorContext;
	getCpuData()->activeFiber = this;
	restoreExecutor(&_executor);
}

void KernelFiber::handlePreemption(Ipl) {
	assert(!intsAreEnabled());
	assert(thisFiber() == this);

	auto *scheduler = &localScheduler.get();

	scheduler->update();
	if(scheduler->maybeReschedule(false)) {
		auto lock = frg::guard(&_mutex);

		_executorContext->active.store(false, std::memory_order_relaxed);
		getCpuData()->executorContext = nullptr;
		getCpuData()->activeFiber = nullptr;

		forkExecutor([&] {
			runOnStack([] (Continuation cont, Executor *executor,
					frg::unique_lock<frg::ticket_spinlock> lock) {
				scrubStack(executor, cont);
				lock.unlock();
				localScheduler.get().commitReschedule();
			}, getCpuData()->detachedStack.base(), &_executor, std::move(lock));
		}, &_executor);
	}else{
		scheduler->renewSchedule();
	}
}

void KernelFiber::handlePreemption(IrqImageAccessor image) {
	assert(!intsAreEnabled());
	assert(thisFiber() == this);

	auto *scheduler = &localScheduler.get();

	scheduler->update();
	if(scheduler->maybeReschedule(false)) {
		auto lock = frg::guard(&_mutex);

		saveExecutor(&_executor, image);
		_executorContext->active.store(false, std::memory_order_relaxed);
		getCpuData()->executorContext = nullptr;
		getCpuData()->activeFiber = nullptr;

		runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<frg::ticket_spinlock> lock) {
			scrubStack(image, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), image, std::move(lock));
	}else{
		scheduler->renewSchedule();
	}
}

void KernelFiber::AssociatedWorkQueue::wakeup() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&fiber_->_mutex);

	// TODO: As for threads, we should suppress wakeups if the IPL at which the fiber is saved
	//       does not allow the work queue to run.

	if(!fiber_->_blocked)
		return;

	fiber_->_blocked = false;
	Scheduler::resume(fiber_);
}

void FiberBlocker::setup() {
	_fiber = thisFiber();
	_done = false;
}

namespace {

KernelFiber *postWorkQueueFiber() {
	return KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
}

} // anonymous namespace

void postWorkQueueFibers(CpuData *cpuData) {
	cpuData->criticalFiber = postWorkQueueFiber();
	cpuData->criticalWorkQueue = cpuData->criticalFiber->associatedWorkQueue().lock();
	assert(cpuData->criticalWorkQueue);

	cpuData->memoryFiber = postWorkQueueFiber();
	cpuData->memoryWorkQueue = cpuData->memoryFiber->associatedWorkQueue().lock();
	assert(cpuData->memoryWorkQueue);

	cpuData->generalFiber = postWorkQueueFiber();
	cpuData->generalWorkQueue = cpuData->generalFiber->associatedWorkQueue().lock();
	assert(cpuData->generalWorkQueue);
	// Only the general WQ may sleep for memory, see WorkQueue::generalQueue().
	cpuData->generalWorkQueue->executorContext()->mayBlockForMemory = true;
}

void resumeWorkQueueFibers(CpuData *cpuData) {
	Scheduler::resume(cpuData->criticalFiber);
	Scheduler::resume(cpuData->memoryFiber);
	Scheduler::resume(cpuData->generalFiber);
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

