#include <signal.h>
#include <print>

#include "gdbserver.hpp"
#include "observations.hpp"
#include "ostrace.hpp"

#include <frg/scope_exit.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

#include "debug-options.hpp"

namespace {

void alertRemoteQueue(Process *self) {
	HelHandle handle;
	auto err = helTransferDescriptor(
	    self->accessThreadPage()->queueHandle,
	    self->fileContext()->getUniverse().getHandle(),
	    kHelTransferDescriptorIn,
	    &handle
	);

	if (err == kHelErrNone) {
		if (int alertErr = helAlertQueue(handle); alertErr != kHelErrNone)
			std::println(
				"posix: unexpected error {} for helAlertQueue(queueHandle)",
		    	_helErrorString(alertErr)
			);
		HEL_CHECK(helCloseDescriptor(kHelThisUniverse, handle));
	} else if (err != kHelErrNoDescriptor) {
		// We ignore kHelErrNoDescriptor because a thread might not (yet) have set up a queue,
		// but might have received a signal (e.g. via pthread_kill).
		std::println(
		    "posix: unexpected error {} for helTransferDescriptor(queueHandle)",
		    _helErrorString(err)
		);
	}
}

async::result<bool> handlePendingSignalsFromObservation(Process *self) {
	if constexpr (logSignals)
		std::println("posix: checking if we should raise pending signals; delayedSignal={}", bool(self->delayedSignal));

	if (!self->delayedSignal) {
		auto active =
		    co_await self->fetchSignal(~self->signalMask(), true);
		if constexpr (logSignals)
			std::println("posix: active SignalItem={}", bool(active));

		if (active) {
			auto handling = self->threadGroup()->signalContext()->determineHandling(active, self);
			if constexpr (logSignals)
				std::println("posix: signal={} handling={}", active->signalNumber, handling);

			if (handling.ignored) {
				co_await self->threadGroup()->signalContext()->raiseContext(active, self, handling);
			} else {
				self->accessThreadPage()->cancellationRequested = true;
				alertRemoteQueue(self);
				if (self->checkOrRequestSignalRaise()) {
					if constexpr (logSignals)
						std::println("posix: raising signal");
					co_await self->threadGroup()->signalContext()->raiseContext(
					    active, self, handling
					);
					if (handling.killed)
						co_return false;
				} else {
					if constexpr (logSignals)
						std::println("posix: making signal delayed");
					self->delayedSignal = active;
					self->delayedSignalHandling = handling;
				}
			}
		}
	}

	co_return true;
}

} // namespace

async::result<void> observeThread(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();

	while(true) {
		if(generation->inTermination)
			break;

		helix::Observe observe;
		auto &&submit = helix::submitObserve(thread, &observe,
				helix::Dispatcher::global());
		co_await submit.async_wait();

		// Usually, we should terminate via the generation->inTermination check above.
		if(observe.error() == kHelErrThreadTerminated) {
			std::cout << "\e[31m" "posix: Thread terminated unexpectedly" "\e[39m" << std::endl;
			co_return;
		}

		HEL_CHECK(observe.error());

		protocols::ostrace::Timer timer;

		frg::scope_exit traceOnExit{[&] {
			if(posix::ostContext.isActive()) {
				posix::ostContext.emit(
					posix::ostEvtObservation,
					posix::ostAttrRequest(observe.observation()),
					posix::ostAttrTime(timer.elapsed())
				);
			}
		}};

		if(observe.observation() == kHelObserveSuperCall + posix::superAnonAllocate) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			size_t size = gprs[kHelRegArg0];

			void *address = (co_await self->vmContext()->mapFile(0,
					{}, nullptr,
					0, size, true, kHelMapProtRead | kHelMapProtWrite)).unwrap();

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = reinterpret_cast<uintptr_t>(address);
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superAnonDeallocate) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			self->vmContext()->unmapFile(reinterpret_cast<void *>(gprs[kHelRegArg0]), gprs[kHelRegArg1]);

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superGetProcessData) {
			posix::ManagarmProcessData data = {
				self->clientPosixLane(),
				self->fileContext()->clientMbusLane(),
				self->clientThreadPage(),
				static_cast<HelHandle *>(self->clientFileTable()),
				fileTableSize,
				self->clientClkTrackerPage(),
			};

			if(logRequests)
				std::cout << "posix: GET_PROCESS_DATA supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto storeData = co_await helix_ng::writeMemory(thread, gprs[kHelRegArg0],
					sizeof(posix::ManagarmProcessData), &data);
			HEL_CHECK(storeData.error());
			gprs[kHelRegError] = kHelErrNone;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superFork) {
			if(logRequests)
				std::cout << "posix: fork supercall" << std::endl;
			auto child = co_await Process::fork(self);

			// Copy registers from the current thread to the new one.
			auto new_thread = child->threadDescriptor().getHandle();
			uintptr_t pcrs[2], gprs[kHelNumGprs], thrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsThread, &thrs));

			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsProgram, &pcrs));
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsThread, &thrs));

			// Setup post supercall registers in both threads and finally resume the threads.
			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = child->pid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegOut0] = 0;
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			HEL_CHECK(helResume(new_thread));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superClone) {
			if(logRequests)
				std::cout << "posix: clone supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			void *ip = reinterpret_cast<void *>(gprs[kHelRegArg0]);
			void *sp = reinterpret_cast<void *>(gprs[kHelRegArg1]);

			posix::superCloneArgs args{};
			if (gprs[kHelRegArg2]) {
				auto loadArgs = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
						gprs[kHelRegArg2], sizeof(args), &args);
				HEL_CHECK(loadArgs.error());
			}

			auto cloneResult = co_await Process::clone(self, ip, sp, &args);
			HelHandle newThread = kHelNullHandle;

			gprs[kHelRegError] = kHelErrNone;

			if (cloneResult) {
				newThread = cloneResult.value()->threadDescriptor().getHandle();
				gprs[kHelRegOut0] = static_cast<uintptr_t>(managarm::posix::Errors::SUCCESS);
				gprs[kHelRegOut1] = cloneResult.value()->tid();
			} else {
				gprs[kHelRegOut0] = static_cast<uintptr_t>(cloneResult.error() | toPosixProtoError);
			}
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			if (newThread != kHelNullHandle)
				HEL_CHECK(helResume(newThread));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superExecve) {
			if(logRequests)
				std::cout << "posix: execve supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			std::string path;
			path.resize(gprs[kHelRegArg1]);
			auto loadPath = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg0], gprs[kHelRegArg1], path.data());
			HEL_CHECK(loadPath.error());

			std::string args_area;
			args_area.resize(gprs[kHelRegArg3]);
			auto loadArgs = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg2], gprs[kHelRegArg3], args_area.data());
			HEL_CHECK(loadArgs.error());

			std::string env_area;
			env_area.resize(gprs[kHelRegArg5]);
			auto loadEnv = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg4], gprs[kHelRegArg5], env_area.data());
			HEL_CHECK(loadEnv.error());

			if(logRequests || logPaths)
				std::cout << "posix: execve path: " << path << std::endl;

			// Parse both the arguments and the environment areas.
			size_t k;

			std::vector<std::string> args;
			k = 0;
			while(k < args_area.size()) {
				auto d = args_area.find(char(0), k);
				assert(d != std::string::npos);
				args.push_back(args_area.substr(k, d - k));
//				std::cout << "arg: " << args.back() << std::endl;
				k = d + 1;
			}

			std::vector<std::string> env;
			k = 0;
			while(k < env_area.size()) {
				auto d = env_area.find(char(0), k);
				assert(d != std::string::npos);
				env.push_back(env_area.substr(k, d - k));
//				std::cout << "env: " << env.back() << std::endl;
				k = d + 1;
			}

			auto error = co_await Process::exec(self,
					path, std::move(args), std::move(env));
			if(error == Error::success) {
				// Continue
			}else if(error == Error::noSuchFile) {
				gprs[kHelRegError] = kHelErrNone;
				gprs[kHelRegOut0] = ENOENT;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else if(error == Error::badExecutable || error == Error::eof) {
				gprs[kHelRegError] = kHelErrNone;
				gprs[kHelRegOut0] = ENOEXEC;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else {
				// Unhandled error, log and bubble up EIO
				std::cout << "posix: exec: unhandled error from Process::exec, we got: " << (int)error << std::endl;
				gprs[kHelRegError] = kHelErrNone;
				gprs[kHelRegOut0] = EIO;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}
		}else if(observe.observation() == kHelObserveSuperCall + posix::superExit) {
			if(logRequests)
				std::cout << "posix: EXIT supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto code = gprs[kHelRegArg0];

			co_await self->terminate();
			co_await self->threadGroup()->terminateGroup(TerminationByExit{static_cast<int>(code & 0xFF)});
		}else if(observe.observation() == kHelObserveSuperCall + posix::superThreadExit) {
			if(logRequests)
				std::cout << "posix: THREAD_EXIT supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto code = gprs[kHelRegArg0];

			bool lastInGroup;
			co_await self->terminate(&lastInGroup);
			if (lastInGroup)
				co_await self->threadGroup()->terminateGroup(TerminationByExit{static_cast<int>(code & 0xFF)});
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigMask) {
			if(logRequests)
				std::cout << "posix: SIG_MASK supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto mode = gprs[kHelRegArg0];
			auto mask = gprs[kHelRegArg1];

			int err = 0;
			uint64_t former = self->signalMask();
			if(mode == SIG_SETMASK) {
				self->setSignalMask(mask);
			}else if(mode == SIG_BLOCK) {
				self->setSignalMask(former | mask);
			}else if(mode == SIG_UNBLOCK) {
				self->setSignalMask(former & ~mask);
			}else{
				err = EINVAL;
			}

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = err;
			gprs[kHelRegOut1] = former;
			gprs[kHelRegOut2] = self->enteredSignalSeq();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if (!co_await handlePendingSignalsFromObservation(self.get()))
				break;

			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigRaise) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RAISE supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegError] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"in SIG_RAISE supercall" "\e[39m" << std::endl;
			bool killed = false;

			if(self->delayedSignal) {
				killed = self->delayedSignalHandling->killed;
				co_await self->threadGroup()->signalContext()->raiseContext(self->delayedSignal, self.get(), *self->delayedSignalHandling);
				self->delayedSignal = nullptr;
				self->delayedSignalHandling = std::nullopt;
			} else {
				std::println("posix: userspace misbehavior, superSigRaise called without available signal");
			}

			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigRestore) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RESTORE supercall" << std::endl;

			co_await self->threadGroup()->signalContext()->restoreContext(thread, self.get());

			if (!co_await handlePendingSignalsFromObservation(self.get()))
				break;

			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigKill) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_KILL supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto mode = posix::SuperKillMode(gprs[kHelRegArg0]);

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = 0;

			if (mode != posix::SuperKillMode::Kill && mode != posix::SuperKillMode::QueueInfo) {
				gprs[kHelRegOut0] = EINVAL;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
				HEL_CHECK(helResume(thread.getHandle()));
				continue;
			}

			auto tgid = (intptr_t) gprs[kHelRegArg1];
			auto tid = (intptr_t) gprs[kHelRegArg2];

			UserSignal si;
			int sn = 0;

			if (mode == posix::SuperKillMode::Kill) {
				sn = gprs[kHelRegArg3];

				si.pid = self->pid();
				si.uid = self->threadGroup()->uid();
				si.code = tid ? SI_TKILL : SI_USER;
			} else {
				assert(mode == posix::SuperKillMode::QueueInfo);

				siginfo_t info{};
				auto load = co_await helix_ng::readMemory(
					self->vmContext()->getSpace(), gprs[kHelRegArg3], sizeof(info), &info
				);
				HEL_CHECK(load.error());

				if (tgid != self->pid()) {
					// These restrictions only apply if the signal is sent to any other process.
					if (info.si_code >= 0 || info.si_code == SI_TKILL) {
						gprs[kHelRegOut0] = EPERM;
						HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
						HEL_CHECK(helResume(thread.getHandle()));
						continue;
					}
				}

				sn = info.si_signo;
				si.code = info.si_code;
				si.err_no = info.si_errno;
				si.pid = info.si_pid;
				si.uid = info.si_uid;
				si.val = info.si_value;
			}

			std::shared_ptr<ProcessGroup> targetProcessGroup;
			std::shared_ptr<ThreadGroup> targetThreadGroup;
			std::shared_ptr<Process> targetThread;

			if (!tgid) {
				if(logSignals)
					std::println("posix: SIG_KILL on PGRP {} (self)", self->pid());
				targetProcessGroup = self->pgPointer();
				if (!targetProcessGroup) {
					gprs[kHelRegOut0] = ESRCH;
					HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
					HEL_CHECK(helResume(thread.getHandle()));
					continue;
				}
			} else if (tgid == -1) {
				std::println("posix: SIG_KILL(-1) is ignored!");
				gprs[kHelRegOut0] = EPERM;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
				HEL_CHECK(helResume(thread.getHandle()));
				continue;
			} else if (tgid > 0) {
				targetThreadGroup = ThreadGroup::findThreadGroup(tgid);
				if (!targetThreadGroup) {
					gprs[kHelRegOut0] = ESRCH;
					HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
					HEL_CHECK(helResume(thread.getHandle()));
					continue;
				}

				if (tid) {
					targetThread = targetThreadGroup->findThread(tid);
					if (!targetThread) {
						gprs[kHelRegOut0] = ESRCH;
						HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
						HEL_CHECK(helResume(thread.getHandle()));
						continue;
					}
				}
			} else {
				if(logSignals)
					std::println("posix: SIG_KILL on PGRP {} (self)", -tgid);
				targetProcessGroup = ProcessGroup::findProcessGroup(-tgid);
				if (!targetProcessGroup) {
					gprs[kHelRegOut0] = ESRCH;
					HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
					HEL_CHECK(helResume(thread.getHandle()));
					continue;
				}
			}

			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if(sn) {
				if(targetProcessGroup) {
					targetProcessGroup->issueSignalToGroup(sn, si);
				} else if (targetThread) {
					targetThread->issueThreadSignal(sn, si);
				} else {
					targetThreadGroup->issueThreadGroupSignal(sn, si);
				}
			}

			// If the process signalled itself, we should process the signal before resuming.
			if (!co_await handlePendingSignalsFromObservation(self.get()))
				break;

			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigAltStack) {
			// sigaltstack is implemented as a supercall because it
			// needs to access the thread's registers.

			if(logRequests || logSignals)
				std::cout << "posix: SIGALTSTACK supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			uintptr_t pcrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));

			auto ss = gprs[kHelRegArg0];
			auto oss = gprs[kHelRegArg1];

			if (oss) {
				stack_t st{};
				st.ss_sp = reinterpret_cast<void *>(self->altStackSp());
				st.ss_size = self->altStackSize();
				st.ss_flags = (self->isOnAltStack(pcrs[kHelRegSp]) ? SS_ONSTACK : 0)
						| (self->isAltStackEnabled() ? 0 : SS_DISABLE);

				auto store = co_await helix_ng::writeMemory(self->vmContext()->getSpace(),
						oss, sizeof(stack_t), &st);
				HEL_CHECK(store.error());
			}

			int error = 0;

			if (ss) {
				stack_t st{};

				auto load = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
						ss, sizeof(stack_t), &st);
				HEL_CHECK(load.error());

				if (st.ss_flags & ~SS_DISABLE) {
					error = EINVAL;
				} else if (self->isOnAltStack(pcrs[kHelRegSp])) {
					error = EPERM;
				} else {
					self->setAltStackSp(reinterpret_cast<uint64_t>(st.ss_sp), st.ss_size);
					self->setAltStackEnabled(!(st.ss_flags & SS_DISABLE));
				}
			}

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = error;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigSuspend) {
			if(logRequests || logSignals)
				std::cout << "posix: SIGSUSPEND supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			auto seq = gprs[kHelRegArg0];

			if (seq == self->enteredSignalSeq()) {
				auto check = self->checkSignal();

				if (!std::get<1>(check))
					co_await self->pollSignal(std::get<0>(check), ~self->signalMask());
			}

			gprs[kHelRegError] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if (!co_await handlePendingSignalsFromObservation(self.get()))
				break;

			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superGetTid){
			if(logRequests)
				std::cout << "posix: GET_TID supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = self->tid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigGetPending) {
			if(logRequests)
				std::cout << "posix: SIG_GET_PENDING supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegError] = 0;
			auto [_, active] = self->checkSignal();
			gprs[kHelRegOut0] = active & self->signalMask();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigTimedWait) {
			if(logRequests)
				std::cout << "posix: SIG_TIMED_WAIT supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			auto mask = gprs[kHelRegArg0];
			auto timeout = gprs[kHelRegArg1];
			auto infoPtr = gprs[kHelRegArg2];

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = EAGAIN;
			gprs[kHelRegOut1] = 0;

			auto check = co_await self->fetchSignal(mask, true);
			if(check) {
				if(infoPtr) {
					siginfo_t siginfo{};
					siginfo.si_signo = check->signalNumber;
					std::visit(CompileSignalInfo{&siginfo}, check->info);
					auto store = co_await helix_ng::writeMemory(
						self->vmContext()->getSpace(),
						infoPtr, sizeof(siginfo_t), &siginfo);
					HEL_CHECK(store.error());
				}

				gprs[kHelRegOut0] = 0;
				gprs[kHelRegOut1] = check->signalNumber;
			} else if(timeout) {
				SignalItem *item = nullptr;

				co_await async::race_and_cancel(
					async::lambda([&](async::cancellation_token c) -> async::result<void> {
						if(timeout != UINT64_MAX) {
							co_await helix_ng::sleepFor(timeout, c);
						} else {
							co_await async::suspend_indefinitely(c);
						}
					}),
					async::lambda([&](async::cancellation_token c) -> async::result<void> {
						item = co_await self->fetchSignal(mask, false, c);
					}),
					async::lambda([&](async::cancellation_token c) -> async::result<void> {
						co_await async::suspend_indefinitely(c, generation->cancelServe);
					})
				);

				if(item) {
					if(infoPtr) {
						siginfo_t siginfo{};
						std::visit(CompileSignalInfo{&siginfo}, item->info);
						auto store = co_await helix_ng::writeMemory(
							self->vmContext()->getSpace(),
							infoPtr, sizeof(siginfo_t), &siginfo);
						HEL_CHECK(store.error());
					}

					gprs[kHelRegOut0] = 0;
					gprs[kHelRegOut1] = item->signalNumber;
				}
			}

			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superCancel) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			auto cancelId = gprs[kHelRegArg0];
			auto fd = gprs[kHelRegArg1];

			co_await self->cancelEvent(cancelId, fd);

			gprs[kHelRegError] = 0;

			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveInterrupt) {
			// std::println("Process {} ({}) was interrupted forceTermination={}", self->name(), self->pid(), self->forceTermination);
			if (self->forceTermination) {
				co_await self->terminate();
				break;
			}

			if (!co_await handlePendingSignalsFromObservation(self.get()))
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObservePanic) {
			printf("\e[35mposix: User space panic in process %s\e[39m\n", self->path().c_str());
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGABRT;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous user space panic" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveBreakpoint) {
			printf("\e[35mposix: Breakpoint in process %s\n", self->path().c_str());
			self->dumpRegisters();
			printf("\e[39m");
			fflush(stdout);

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely(async::cancellation_token{});
			}
		}else if(observe.observation() == kHelObservePageFault) {
			if(logPageFaults) {
				printf("\e[31mposix: Page fault in process %s\n", self->path().c_str());
				self->dumpRegisters();
				printf("\e[39m");
				fflush(stdout);
			}

			uintptr_t gprs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsPageFault, &gprs));

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			item->info = SegfaultSignal{
			    gprs[0],
			    (gprs[1] == kHelPageFaultAccessError),
			    (gprs[1] == kHelPageFaultMapError),
			};
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveGeneralFault) {
			printf("\e[31mposix: General fault in process %s\e[39m\n", self->path().c_str());
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveIllegalInstruction) {
			printf("\e[31mposix: Illegal instruction in process %s\e[39m\n", self->path().c_str());
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGILL;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGILL" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed) {
				if(debugFaults) {
					launchGdbServer(self.get());
					co_await async::suspend_indefinitely(async::cancellation_token{});
				}
				break;
			}
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveDivByZero) {
			printf("\e[31mposix: Divide by zero in process %s\e[39m\n", self->path().c_str());
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGFPE;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGFPE" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else{
			printf("\e[31mposix: Unexpected observation in process %s\e[39m\n", self->path().c_str());
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGILL;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGILL" "\e[39m" << std::endl;
			bool killed;
			co_await self->threadGroup()->signalContext()->determineAndRaiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}
	}
}
