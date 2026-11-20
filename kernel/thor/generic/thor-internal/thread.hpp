#pragma once

#include <atomic>
#include <expected>

#include <frg/container_of.hpp>
#include <frg/seqlock.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/credentials.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/load-tracking.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

// Bitwise flags that cause a thread to take some asynchronous action.
using Condition = uint32_t;

// Conditions are dequeued in descending order (i.e., high condition bits take priority).
namespace condition {
// The load balancer assigned the thread to a different CPU.
inline constexpr Condition cpuMigration = Condition{1} << 0;
// One of the WQs is non-empty.
inline constexpr Condition passiveWq = Condition{1} << 1;
inline constexpr Condition exceptionalWq = Condition{1} << 2;
// Request kIntrRequested to be raised.
inline constexpr Condition interrupt = Condition{1} << 3;
// Request transition to kRunTerminated.
inline constexpr Condition terminate = Condition{1} << 4;
} // namespace condition

// Conditions that cancel blocking operations.
inline constexpr Condition cancelConditions = condition::interrupt | condition::terminate;

enum Interrupt {
	kIntrNull,
	kIntrDivByZero,
	kIntrRequested,
	kIntrPanic,
	kIntrBreakpoint,
	kIntrPageFault,
	kIntrGeneralFault,
	kIntrIllegalInstruction,
	kIntrSuperCall = 0x80000000
};

enum class PageFaultType {
	None,
	NotMapped,
	BadPermissions,
	OutOfMemory,
};

struct InterruptInfo {
	uintptr_t offendingAddress = 0;
	PageFaultType pageFaultType = PageFaultType::None;
};

struct AsyncBlockCurrentInterruptibleTag {};
struct AsyncBlockCurrentNormalTag {};

template <typename T>
concept AnyTag = (std::same_as<T, AsyncBlockCurrentNormalTag> || std::same_as<T, AsyncBlockCurrentInterruptibleTag>);

struct Thread;
#ifdef THOR_HAS_KCOV
struct KcovBuffer;
#endif
struct LbThreadState;

smarter::borrowed_ptr<Thread> getCurrentThread();

struct Thread final : ScheduleEntity, Credentials, RcuProtected {
	// The load balancer accounts the load of threads per CPU; this is tied to their run state.
	friend struct LoadBalancer;

private:
	struct CtorToken {};

	struct AssociatedWorkQueue final : WorkQueue {
		AssociatedWorkQueue(Thread *thread, Ipl wqIpl)
		: WorkQueue{thread->_executorContext, wqIpl}, _thread{thread} { }

		void wakeup() override;

	private:
		Thread *_thread;
	};

	template<typename T>
	struct BlockingState {
		struct Empty { };
		using MaybeT = std::conditional_t<
			std::is_same_v<T, void>,
			Empty,
			T
		>;

		// We need a shared_ptr since the thread might continue (and thus could be killed)
		// immediately after we set the done flag.
		smarter::shared_ptr<Thread> thread;
		WorkQueue *wq;
		// Acquire-release semantics to publish the result of the async operation.
		std::atomic<bool> done{false};
		frg::optional<MaybeT> value{};
	};

	template<typename T>
	struct BlockingEnv {
		WorkQueue *get_work_queue() {
			return blsp->wq;
		}

		BlockingState<T> *blsp;
	};

	template<typename T>
	struct BlockingReceiver {
		template<typename... Args>
		void set_value(Args &&... args) {
			// The blsp pointer may become invalid as soon as we set blsp->done.
			auto thread = std::move(blsp->thread);
			assert(!blsp->done.load(std::memory_order_relaxed));
			blsp->value.emplace(std::forward<Args>(args)...);
			blsp->done.store(true, std::memory_order_release);
			Thread::unblockOther(thread);
		}

		auto get_env() {
			return BlockingEnv<T>{.blsp = blsp};
		}

		BlockingState<T> *blsp;
	};

public:
	static std::expected<smarter::shared_ptr<Thread, ActiveHandle>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy,
			smarter::shared_ptr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi) {
		smarter::borrowed_ptr<Hierarchy> hierarchyPtr{hierarchy};
		auto thread = allocate_rcu_shared<Thread>(HierarchyAllocator{std::move(hierarchy)},
				CtorToken{}, hierarchyPtr, std::move(universe), std::move(address_space), abi);
		thread->self = thread;
		thread->_executorContext->exceptionalWq = &thread->_pagingWorkQueue;

		// The kernel owns one reference to the thread until the thread finishes execution.
		thread.policy().increment();

		auto ptr = thread.get();
		ptr->_activeCtr.setup(smarter::adopt_rc, 1);
		thread.release();
		return smarter::shared_ptr<Thread, ActiveHandle>{smarter::adopt_rc, ptr, ActiveHandle{ptr}};
	}

	template <typename Sender>
	static auto asyncBlockCurrent(Sender s, WorkQueue *wq, Condition maskedCancelConditions = 0) {
		return asyncBlockCurrent([s = std::move(s)](async::cancellation_token) mutable -> Sender {
			return std::move(s);
		}, wq, maskedCancelConditions, AsyncBlockCurrentNormalTag{});
	}

	template <typename SenderFactory>
	requires(std::is_invocable_v<SenderFactory, async::cancellation_token>)
	static auto asyncBlockCurrentInterruptible(SenderFactory s, WorkQueue *wq, Condition maskedCancelConditions = 0) {
		return asyncBlockCurrent(std::move(s), wq, maskedCancelConditions, AsyncBlockCurrentInterruptibleTag{});
	}

	// maskedCancelConditions allows callers to suppress interrupts due to some of the conditions.
	// In particular, if a condition C is set in maskedCancelConditions,
	// then raising C will *not* interrupt the blocking.
	template <typename SenderFactory, AnyTag Tag>
	requires std::is_invocable_v<SenderFactory, async::cancellation_token>
	static auto asyncBlockCurrent(SenderFactory s, WorkQueue *wq, Condition maskedCancelConditions, Tag tag) {
		using ValueType = std::invoke_result_t<SenderFactory, async::cancellation_token>::value_type;

		auto ipl = currentIpl();
		if (wq) {
			assert(ipl < wq->wqIpl());
		} else {
			assert(currentIpl() < ipl::noSchedule);
		}
		assert(!(maskedCancelConditions & ~cancelConditions));
		(void)tag;
		auto thisThread = getCurrentThread();

		// Conditions that indicate that a runnable WQ is non-empty.
		Condition wqConditions = 0;
		if (ipl < ipl::passiveWork)
			wqConditions |= condition::passiveWq;
		if (ipl < ipl::exceptionalWork)
			wqConditions |= condition::exceptionalWq;

		// Conditions that cause us to cancel the operation.
		auto effectiveCancelConditions = cancelConditions & ~maskedCancelConditions;

		async::cancellation_event ce;
		BlockingState<ValueType> bls{.thread = thisThread.lock(), .wq = wq};

		// Start the operation.
		auto operation = async::execution::connect(
			s(async::cancellation_token{ce}), BlockingReceiver<ValueType>{&bls}
		);
		async::execution::start(operation);

		// Wait for completion.
		bool interruptible = std::is_same_v<Tag, AsyncBlockCurrentInterruptibleTag>;
		while(true) {
			if(bls.done.load(std::memory_order_acquire))
				break;
			auto pending = thisThread->_pendingConditions.load(std::memory_order_relaxed);
			if (pending & wqConditions) {
				runWqs();
				continue;
			}
			if (interruptible && (pending & effectiveCancelConditions)) {
				ce.cancel();
				interruptible = false;
				continue;
			}
			auto checkedConditions = wqConditions;
			if (interruptible)
				checkedConditions |= effectiveCancelConditions;
			Thread::blockCurrent(checkedConditions);
		}

		if constexpr (!std::is_same_v<ValueType, void>)
			return std::move(*bls.value);
	}

	bool checkConditions() {
		return _pendingConditions.load(std::memory_order_relaxed);
	}

	bool checkCancelConditions() {
		return _pendingConditions.load(std::memory_order_relaxed) & cancelConditions;
	}

	// Run the current thread's WQs. Returns true if there was any work to do.
	static bool runWqs() {
		auto ipl = currentIpl();
		auto thisThread = getCurrentThread();
		auto pending = thisThread->_pendingConditions.load(std::memory_order_relaxed);
		if (ipl < ipl::exceptionalWork) {
			if (pending & condition::exceptionalWq) {
				thisThread->_pagingWorkQueue.run();
				thisThread->_pendingConditions.fetch_and(~condition::exceptionalWq, std::memory_order_acq_rel);
				// Re-check after clearing the condition as work may have been queued.
				if (thisThread->_pagingWorkQueue.check())
					thisThread->_pagingWorkQueue.run();
				return true;
			}
		}
		if (ipl < ipl::passiveWork) {
			if (pending & condition::passiveWq) {
				thisThread->_mainWorkQueue.run();
				thisThread->_pendingConditions.fetch_and(~condition::passiveWq, std::memory_order_acq_rel);
				// Re-check after clearing the condition as work may have been queued.
				if (thisThread->_mainWorkQueue.check())
					thisThread->_mainWorkQueue.run();
				return true;
			}
		}
		return false;
	}

	// Run the current thread's WQ until they are empty.
	static void drainWqs() {
		while (runWqs())
			;
	}

	// State transitions that apply to the current thread only.

	// If any conditions in checkedConditions is set, we do not block.
	static void blockCurrent(Condition checkedConditions);
	static void deferCurrent();
	static void deferCurrent(IrqImageAccessor image);
	static void suspendCurrent(IrqImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, FaultImageAccessor image, InterruptInfo info);
	static void interruptCurrent(Interrupt interrupt, SyscallImageAccessor image, InterruptInfo info);
	static void interruptCurrent(Interrupt interrupt, IrqImageAccessor image, InterruptInfo info);

	static void handleConditions(SyscallImageAccessor image);
	static void handleConditions(FaultImageAccessor image);
	static void handleConditions(IrqImageAccessor image);

	// State transitions that apply to arbitrary threads.
	// TODO: interruptOther() needs an Interrupt argument.
	static void unblockOther(smarter::borrowed_ptr<Thread> thread);
	static void killOther(smarter::borrowed_ptr<Thread> thread);
	static void interruptOther(smarter::borrowed_ptr<Thread> thread);
	static void migrateOther(smarter::borrowed_ptr<Thread> thread);
	static Error resumeOther(smarter::borrowed_ptr<Thread> thread);

	enum Flags : uint32_t {
		kFlagServer = 1
	};

	Thread(CtorToken, smarter::borrowed_ptr<Hierarchy> hierarchy,
			smarter::shared_ptr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi);
	~Thread();

	smarter::borrowed_ptr<WorkQueue> mainWorkQueue() {
		return {&_mainWorkQueue, self.policy()};
	}
	smarter::borrowed_ptr<WorkQueue> pagingWorkQueue() {
		return {&_pagingWorkQueue, self.policy()};
	}

	UserContext &getContext();
	smarter::borrowed_ptr<Universe> getUniverse();
	smarter::borrowed_ptr<AddressSpace, BindableHandle> getAddressSpace();
	smarter::borrowed_ptr<Hierarchy> getHierarchy();

	// ----------------------------------------------------------------------------------
	// observe() and its boilerplate.
	// ----------------------------------------------------------------------------------
private:
	struct ObserveNode {
		async::any_receiver<frg::tuple<Error, Interrupt>> receiver;
		frg::default_list_hook<ObserveNode> hook = {};
	};

	void observe_(ObserveNode *node);

public:
	template<typename Receiver>
	struct [[nodiscard]] ObserveOperation {
		ObserveOperation(Thread *self, Receiver receiver)
		: self_{self}, node_{.receiver = std::move(receiver)} { }

		void start() {
			self_->observe_(&node_);
		}

	private:
		Thread *self_;
		ObserveNode node_;
	};

	struct [[nodiscard]] ObserveSender {
		using value_type = frg::tuple<Error, Interrupt>;

		async::sender_awaiter<ObserveSender, frg::tuple<Error, Interrupt>>
		operator co_await() {
			return {std::move(*this)};
		}

		template<typename Receiver>
		ObserveOperation<Receiver> connect(Receiver receiver) {
			return {self, std::move(receiver)};
		}

		Thread *self;
	};

	ObserveSender observe() {
		return {this};
	}

	// ----------------------------------------------------------------------------------

	// TODO: Do not expose these functions publically.
	// Called when the ActiveHandle refcount reaches zero.
	void dispose();

	[[ noreturn ]] void invoke() override;

	void handlePreemption(Ipl ipl) override;
	void handlePreemption(IrqImageAccessor image) override;
	// Non-virtual since syscalls/faults know that they are called from a thread.
	void handlePreemption(FaultImageAccessor image);

	// Access a thread's registers while the thread is interrupted.
	template<typename F>
	requires requires(F f, Executor *executor) {
		{ f(executor) } -> std::same_as<void>;
	}
	std::expected<void, Error> accessRegisters(F &&f) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(intrState_ != IntrState::inInterrupt)
			return std::unexpected{Error::illegalState};
		f(&intrImage_);
		return {};
	}

	std::expected<InterruptInfo, Error> getInterruptInfo() {
		InterruptInfo info;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			if(intrState_ != IntrState::inInterrupt)
				return std::unexpected{Error::illegalState};
			info = _interruptInfo;
		}
		return info;
	}

private:
	static void launchCurrent_();
	static void terminateCurrent_();

	template<typename ImageAccessor>
	static void genericInterruptCurrent(Interrupt interrupt, ImageAccessor image, InterruptInfo info);

	template<typename ImageAccessor>
	static void genericHandleConditions(ImageAccessor image);

	template<typename ImageAccessor>
	static void migrateCurrentToAssignedCpu(ImageAccessor image);

	template<typename ImageAccessor>
	void genericHandlePreemption(ImageAccessor image);

	void raiseCondition_(Condition c);

	void _updateRunTime();
	void _publishLoad();
	void _uninvoke();

public:
	// TODO: Tidy this up.
	smarter::counter _activeCtr;
	smarter::borrowed_ptr<Thread> self;

	uint32_t flags;

#ifdef THOR_HAS_KCOV
	// Covers writes to kcov, kcovRef and kcovExited.
	frg::ticket_spinlock kcovMutex;
	// Buffer where coverage data gets written to.
	KcovBuffer *kcov{nullptr};
	// Actual reference to the kcov object.
	smarter::shared_ptr<KcovBuffer> kcovRef;
	// Set when the thread exits.
	bool kcovExited{false};
#endif

private:
	typedef frg::ticket_spinlock Mutex;

	enum RunState {
		kRunNone,

		// the thread is running on some processor.
		kRunActive,

		// the thread is in the schedule queue but not active on any processor.
		kRunDeferred,

		// the thread is waiting for progress inside the kernel.
		// it is not scheduled.
		kRunBlocked,

		// Thread terminated (i.e., it will never be scheduled again).
		// If a thread is in kRunTerminated, then:
		// * mainWorkQueue() and pagingWorkQueue() must be empty.
		//   TODO: This is actually not enfored right now.
		// * There must be no pending conditions.
		// * The thread has retired its use of any kernel data structures.
		//   For example, it must not hold any mutexes anymore etc.
		// The thread's data structures must not be destructed until we reach this state.
		kRunTerminated
	};

	enum class IntrState {
		none,
		inInterrupt,
		resumeFromInterrupt,
	};

	void _setRunState(RunState state);

	// Precondition: _mutex is held.
	bool _isRunnable() {
		return _runState == kRunActive || _runState == kRunDeferred;
	}

	// Like _runnableAverage and _runningAverage, with their fractional bits.
	struct Averages {
		uint64_t runnable;
		uint64_t running;
	};

	// Exact variant of load(): extrapolates the averages to the given time
	// under the assumption that the run state did not change since _lastRunTimeUpdate.
	// Precondition: _mutex is held.
	Averages _averagesAt(uint64_t now) {
		constexpr uint64_t full = UINT64_C(1) << (loadShift + loadFractionShift);
		uint64_t elapsed = now > _lastRunTimeUpdate ? now - _lastRunTimeUpdate : 0;
		auto factor = loadDecayFactor(elapsed);
		return {
			.runnable = advanceLoad(_runnableAverage, _isRunnable() ? full : 0, factor),
			.running = advanceLoad(_runningAverage, _runState == kRunActive ? full : 0, factor),
		};
	}

	// Used by the AssociatedWorkQueues below so must be initialized before.
	ExecutorContext *_executorContext;

	AssociatedWorkQueue _mainWorkQueue;
	AssociatedWorkQueue _pagingWorkQueue;

	// When the current thread is scheduled out,
	// _mutex is taken before the thread is queued or suspended (in maybeReschedule() or forceReschedule())
	// and only released once its executor is saved.
	// This serializes executor saving vs. invoke().
	Mutex _mutex;

	RunState _runState;
	// For kRunActive: CPU that we are running on.
	CpuData *activeCpu_{nullptr};
	// Conditions that unblock the thread while in kRunBlocked.
	Condition unblockConditions_{0};

	// If this flag is set, blockCurrent() returns immediately.
	// In blockCurrent(), the flag is checked within _mutex.
	// On 0-1 transitions, we take _mutex and try to unblock the thread.
	// Since _mutex enforces a total order, this guarantees correctness
	// (i.e., that we never block when we should not).
	std::atomic<bool> _unblockLatch{false};

	// Whether the thread is currently interrupted (e.g., due to an unhandled fault) or not.
	IntrState intrState_{IntrState::none};
	// Only valid if intrState_ == IntrState::inInterrupt;
	Interrupt _lastInterrupt;
	// Only valid if intrState_ == IntrState::inInterrupt;
	InterruptInfo _interruptInfo;
	// Raised after intrState_ becomes IntrState::resumeFromInterrupt.
	async::recurring_event resumeEvent_;

	// Conditions are raised while holding the thread mutex.
	// In particular, functions like blockCurrent() can be sure that no conditions
	// become pending while they are modifying the thread state.
	// Conditions can be read and cleared without holding the mutex.
	// Conditions can only be cleared from the thread itself.
	std::atomic<Condition> _pendingConditions{0};

	// Number of references that keep this thread running.
	// The thread is killed when this counter reaches zero.
	std::atomic<int> _runCount;

	UserContext _userContext;
	// Depends on _userContext, MUST come after it in the struct due to initialization order.
	Executor _executor;
	// Register image that is saved/restored by the interrupt state.
	// Only valid if intrState_ == IntrState::inInterrupt;
	// Depends on _userContext, MUST come after it in the struct due to initialization order.
	Executor intrImage_;

public:
	// Timestamp at which _updateRunTime() was last called.
	uint64_t _lastRunTimeUpdate{0};
	// Decaying averages of the time during which the thread was runnable and running.
	// The thread is runnable if it is either running or waiting in a scheduler queue
	// (i.e., not blocked).
	// Fixed point numbers with (loadShift + loadFractionShift) fractional bits.
	// New threads start at full runnable load (as on Linux) such that they are spread
	// before their average converges.
	uint64_t _runnableAverage{UINT64_C(1) << (loadShift + loadFractionShift)};
	uint64_t _runningAverage{0};
	// Snapshot of the averages for readers that do not take _mutex. Written under _mutex.
	frg::seqlock_cell<ThreadLoad> _publishedLoad;

	// Return the load as of the last change of the thread's run state.
	// Use ThreadLoad::at() to extrapolate it to the current time.
	ThreadLoad load() {
		return _publishedLoad.load();
	}

	LbThreadState _lbState;

private:
	smarter::shared_ptr<Universe> _universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> _addressSpace;
	// Kept alive by the HierarchyAllocator that allocated this Thread.
	smarter::borrowed_ptr<Hierarchy> _hierarchy;

	using ObserveQueue = frg::intrusive_list<
		ObserveNode,
		frg::locate_member<
			ObserveNode,
			frg::default_list_hook<ObserveNode>,
			&ObserveNode::hook
		>
	>;

	ObserveQueue _observeQueue;
};

inline void ActiveHandle::increment() const {
	_thread->_activeCtr.increment();
}

inline void ActiveHandle::decrement() const {
	if(_thread->_activeCtr.decrement_and_check_if_zero()) {
		_thread->dispose();
		_thread->self.policy().decrement();
	}
}

inline smarter::default_rc_policy
ActiveHandle::downcast_policy(smarter::rc_policy_tag<smarter::default_rc_policy>) const {
	return _thread->self.policy();
}

} // namespace thor
