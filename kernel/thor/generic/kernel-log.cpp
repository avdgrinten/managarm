#include <frg/scope_exit.hpp>
#include <frg/string.hpp>
#include <frg/small_vector.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/reentrancy.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr int recordsBeforeFlush = 16;

	// ----------------------------------------------------------------------------------
	// General log ring infrastructure.
	// ----------------------------------------------------------------------------------

	// Protects pushes to the logRing.
	constinit ReentrancySafeSpinlock postMutex;

	constinit ReentrantRecordRing logRing;

	// ----------------------------------------------------------------------------------
	// emit() logic.
	// Logs are first posted to the logRing and then emitted to LogHandlers
	// via emit() or emitUrgent().
	// Calls into LogHandlers happen either in the drain fiber (see below)
	// or, for expedited logs, in postLogRecord().
	// ----------------------------------------------------------------------------------

	// Protects against concurrent calls to log handlers.
	constinit ReentrancySafeSpinlock emitMutex;

	// Protected by emitMutex but may be used from reentrant contexts.
	constinit std::atomic<uint64_t> emitSeq{0};

	// Assumption: !intsAreEnabled().
	// Assumption: emitMutex is taken.
	template<typename F>
	requires std::invocable<F, frg::string_view>
	bool dispatchLogFromRing(F fn) {
		auto seq = emitSeq.load(std::memory_order_relaxed);
		char buffer[logLineLength];
		auto [success, recordPtr, nextPtr, actualSize] = retrieveLogRecord(
				seq, buffer, logLineLength);
		if (!success)
			return false;

		if (actualSize < sizeof(LogMetadata))
			panic();
		frg::string_view record{buffer, actualSize};
		fn(record);

		// Do a CAS since a reentrant context may have emitted more logs in the meantime.
		emitSeq.compare_exchange_strong(
			seq, nextPtr, std::memory_order_relaxed, std::memory_order_relaxed
		);
		return true;
	}

	bool emitLogFromRing() {
		return dispatchLogFromRing([] (frg::string_view record) {
			for (const auto &it : globalLogList)
				it->emit(record);
		});
	}

	bool emitUrgentLogFromRing() {
		return dispatchLogFromRing([] (frg::string_view record) {
			for (const auto &it : globalLogList) {
				if (!it->takesUrgentLogs)
					continue;
				it->emitUrgent(record);
			}
		});
	}

	void flushLogHandlers() {
		for (const auto &it : globalLogList)
			it->flush();
	}

	void flushUrgentLogHandlers() {
		for (const auto &it : globalLogList) {
			if (!it->takesUrgentLogs)
				continue;
			it->flushUrgent();
		}
	}

	// ----------------------------------------------------------------------------------
	// Log draining fiber.
	// ----------------------------------------------------------------------------------

	// Raised whenever new log records are published.
	constinit SelfIntEvent drainEvent;

	// Raised by the drain fiber after each pass over the log ring.
	// Other waiters cannot use drainEvent: its raises coalesce until the drain fiber clears it.
	constinit async::recurring_event drainedEvent;

	// Whether the log drain fiber has started yet.
	constinit std::atomic<bool> drainOnline{false};

	void runLogDrain() {
		drainOnline.store(true, std::memory_order_relaxed);

		while(true) {
			KernelFiber::asyncBlockCurrent(drainEvent.wait());

			int sinceFlush = 0;
			bool mustWake = false;
			while(true) {
				// Wake waitForLog() even if records keep arriving, but not while holding emitMutex.
				if (mustWake) {
					drainedEvent.raise();
					mustWake = false;
				}

				StatelessIrqLock irqLock;
				auto emitLock = frg::guard(&emitMutex);

				if (emitLogFromRing()) {
					// Flush every few records.
					// Without this, the screen may not be updated for extended periods of time
					// while the system is emitting a lot of logs.
					++sinceFlush;
					if (sinceFlush >= recordsBeforeFlush) {
						flushLogHandlers();
						sinceFlush = 0;
						mustWake = true;
					}
					continue;
				} else {
					// Flush before we sleep again in the even above.
					if (sinceFlush) {
						flushLogHandlers();
						sinceFlush = 0;
					}
					mustWake = true;
				}

				if (!drainEvent.clear())
					break;
			}

			drainedEvent.raise();
		}
	}

	initgraph::Task initLogDrainTask{&globalInitEngine, "generic.init-log-drain",
		initgraph::Requires{getFibersAvailableStage()},
		[] {
			KernelFiber::run([] {
				runLogDrain();
			});
		}
	};
} // anonymous namespace

void postLogRecord(frg::string_view record, bool expedited) {
	RobustIrqLock irqLock;

	// First, post the record to the log ring.
	{
		bool reentrant{postMutex.owner() == getCpuData()};
		if (!reentrant)
			postMutex.lock();
		frg::scope_exit unlockOnExit{[&] {
			if (!reentrant)
				postMutex.unlock();
		}};

		logRing.enqueue(record.data(), record.size());
	}

	// We always wake up the logging thread.
	auto useThreaded = drainOnline.load(std::memory_order_relaxed)
			&& getCpuData()->cpuState.load(std::memory_order_relaxed) == CpuState::online;
	if (useThreaded)
		drainEvent.raise();

	// For expedited logs, we call into log handlers synchronously.
	if (!useThreaded || expedited) {
		// We are emitting all remaining events here anyway (without releasing the lock),
		// so flushing in between records offers little advantages. Simply flush at the end.
		bool reentrant{emitMutex.owner() == getCpuData()};
		if (!reentrant) {
			auto emitLock = frg::guard(&emitMutex);

			while (true) {
				if (!emitLogFromRing())
					break;
			}
			flushLogHandlers();
		} else {
			while (true) {
				if (!emitUrgentLogFromRing())
					break;
			}
			flushUrgentLogHandlers();
		}
	}
}

void flushLogHandler(LogHandler *handler) {
	StatelessIrqLock irqLock;
	auto emitLock = frg::guard(&emitMutex);

	handler->flush();
}

coroutine<void> waitForLog(uint64_t deqPtr) {
	// TODO: Log records that are posted after a drain pass may become available earlier
	//       to consumers of the asynchronous waitForLog() / retrieveLogRecord() API
	//       than for synchronous LogHandlers.
	//       We could wait for emitSeq to avoid making logs beyond emitSeq available.
	co_await drainedEvent.async_wait_if([=] () -> bool {
		return logRing.peekHeadPtr() == deqPtr;
	});
}

frg::tuple<bool, uint64_t, uint64_t, size_t> retrieveLogRecord(uint64_t deqPtr, void *data, size_t maxSize) {
	return logRing.dequeueAt(deqPtr, data, maxSize);
}

//-----------------------------------------------------------------------------
// Kmsg implementation.
//-----------------------------------------------------------------------------

namespace {
	frg::manual_box<LogRingBuffer> globalKmsgRing;

	struct KmsgLogHandlerContext {
		uint64_t kmsgSeq_ = 0;
		int state_ = 0;

		frg::small_vector<char, logLineLength + 1, KernelAlloc> buffer_{*kernelAlloc};
	};

	void printChar(KmsgLogHandlerContext *ctx, char c) {
		auto emit = [&](const char c) {
			ctx->buffer_.push_back(c);

			if(c == '\n') {
				ctx->buffer_.push_back(0);
				getGlobalKmsgRing()->enqueue(ctx->buffer_.data(), ctx->buffer_.size());
				ctx->buffer_.resize(0);
			}
		};

		if(!ctx->state_) {
			if(c == '\x1B') {
				ctx->state_ = 1;
			} else {
				emit(c);
			}
		} else if(ctx->state_ == 1) {
			if(c == '[')
				ctx->state_ = 2;
			else {
				emit(c);
				ctx->state_ = 0;
			}
		} else if(ctx->state_ == 2) {
			if(c >= '0' && c <= '?') {
				// ignore them
			} else if(c >= ' ' && c <= '/') {
				ctx->state_ = 3;
			} else if(c >= '@' && c <= '~') {
				ctx->state_ = 0;
			} else {
				emit(c);
				ctx->state_ = 0;
			}
		} else if(ctx->state_ == 3) {
			if(c >= ' ' && c <= '/') {
				// ignore them
			} else if(c >= '@' && c <= '~') {
				ctx->state_ = 0;
			} else {
				emit(c);
				ctx->state_ = 0;
			}
		} else {
			assert(!"invalid state reached!");
			emit(c);
			ctx->state_ = 0;
		}
	}

	void setPriority(KmsgLogHandlerContext *ctx, Severity prio) {
		assert(ctx->buffer_.empty());
		auto now = getClockNanos() / 1000;
		frg::output_to(ctx->buffer_) << frg::fmt("{},{},{};", uint8_t(prio), ctx->kmsgSeq_++, now);
	}

	void translateRecord(KmsgLogHandlerContext *ctx, frg::string_view record) {
		auto [md, msg] = destructureLogRecord(record);
		setPriority(ctx, md.severity);
		for (size_t i = 0; i < msg.size(); ++i)
			printChar(ctx, msg[i]);
		printChar(ctx, '\n');
	}

	coroutine<void> dumpLogToKmsg() {
		char buffer[logLineLength];
		uint64_t deqPtr = 0;
		KmsgLogHandlerContext ctx;
		while (true) {
			auto [success, recordPtr, nextPtr, actualSize] = retrieveLogRecord(
					deqPtr, buffer, logLineLength);
			if (!success) {
				co_await waitForLog(deqPtr);
				continue;
			}

			frg::string_view record{buffer, actualSize};
			translateRecord(&ctx, record);

			deqPtr = nextPtr;
		}
	}

	initgraph::Task initKmsg{&globalInitEngine, "generic.init-kmsg",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage(), getTaskingAvailableStage()},
		[] {
			// Initialize globalKmsgRing and related functionality.
			void *kmsgMemory = kernelAlloc->allocate(1 << 20);
			globalKmsgRing.initialize(reinterpret_cast<uintptr_t>(kmsgMemory), 1 << 20);

			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), dumpLogToKmsg());

			// Expose globalKmsgRing as I/O channel.
			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(),
					[] () -> coroutine<void> {
				auto channel = co_await solicitIoChannel("kernel-log");
				infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
				co_await dumpRingToChannel(globalKmsgRing.get(), std::move(channel), 2048);
			}());
		}
	};
} // namespace


LogRingBuffer *getGlobalKmsgRing() {
	return globalKmsgRing.get();
}

} // namespace thor
