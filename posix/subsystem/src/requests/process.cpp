#include "common.hpp"
#include "../requests.hpp"
#include <sys/wait.h>
#include <sys/resource.h>
#include <iostream>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::WaitIdRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	if((req.flags() & ~(WNOHANG | WCONTINUED | WEXITED | WSTOPPED | WNOWAIT)) ||
		!(req.flags() & (WEXITED /*| WSTOPPED | WCONTINUED*/))) {
		std::cout << "posix: WAIT_ID invalid flags: " << req.flags() << std::endl;
		co_await sendErrorResponse<managarm::posix::WaitIdResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	WaitFlags flags = 0;

	if(req.flags() & WNOHANG)
		flags |= waitNonBlocking;

	if(req.flags() & WEXITED)
		flags |= waitExited;

	if(req.flags() & WSTOPPED)
		std::cout << "\e[31mposix: WAIT_ID flag WSTOPPED is silently ignored\e[39m" << std::endl;

	if(req.flags() & WCONTINUED)
		std::cout << "\e[31mposix: WAIT_ID flag WCONTINUED is silently ignored\e[39m" << std::endl;

	if(req.flags() & WNOWAIT)
		flags |= waitLeaveZombie;

	int wait_pid = 0;
    // TODO (geert): make this operation cancelable.
	if(req.idtype() == P_PID) {
		wait_pid = req.id();
	} else if(req.idtype() == P_ALL) {
		wait_pid = -1;
	} else if(req.idtype() == P_PIDFD) {
		auto fd = self->fileContext()->getFile(req.id());
		if(!fd || fd->kind() != FileKind::pidfd) {
			co_await sendErrorResponse<managarm::posix::WaitIdResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}
		auto pidfd = smarter::static_pointer_cast<pidfd::OpenFile>(fd);
		wait_pid = pidfd->pid();
		if(pidfd->nonBlock())
			flags |= waitNonBlocking;
	} else {
		std::cout << "\e[31mposix: WAIT_ID idtype other than P_PID, P_PIDFD and P_ALL are not implemented\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::WaitIdResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	logRequest(logRequests, self, "WAIT_ID", "pid={}", wait_pid);

	auto wait_result = co_await self->wait(wait_pid, flags, {});

	managarm::posix::WaitIdResponse resp;

	if(wait_result) {
		auto proc_state = wait_result.value();
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_pid(proc_state.pid);
		resp.set_uid(proc_state.uid);

		if(auto byExit = std::get_if<TerminationByExit>(&proc_state.state); byExit) {
			resp.set_sig_status(W_EXITCODE(byExit->code, 0));
			resp.set_sig_code(CLD_EXITED);
		}else if(auto bySignal = std::get_if<TerminationBySignal>(&proc_state.state); bySignal) {
			resp.set_sig_status(W_EXITCODE(0, bySignal->signo));
			resp.set_sig_code(self->threadGroup()->getDumpable() ? CLD_DUMPED : CLD_KILLED);
		}else{
			resp.set_sig_status(0);
			resp.set_sig_code(0);
			assert(std::holds_alternative<std::monostate>(proc_state.state));
		}
	} else if (wait_result.error() == Error::wouldBlock) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_pid(0);
	} else {
		assert(wait_result.error() == Error::noChildProcesses);
		resp.set_error(managarm::posix::Errors::NO_CHILD_PROCESSES);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::WaitRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	if(req.flags() & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
		std::println("posix: WAIT invalid flags: {:#x}", req.flags());
		co_await sendErrorResponse<managarm::posix::WaitResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	WaitFlags flags = waitExited;

	if(req.flags() & WNOHANG)
		flags |= waitNonBlocking;

	if(req.flags() & WUNTRACED)
		std::println("\e[31mposix: WAIT flag WUNTRACED is silently ignored\e[39m");

	if(req.flags() & WCONTINUED)
		std::println("\e[31mposix: WAIT flag WCONTINUED is silently ignored\e[39m");

	logRequest(logRequests, self, "WAIT", "pid={}", req.pid());

	frg::expected<Error, Process::WaitResult> waitResult = Error::ioError;

	{
		auto cancelEvent = self->cancelEventRegistry().event(self->credentials(), req.cancellation_id());
		if (!cancelEvent) {
			std::println("posix: possibly duplicate cancellation ID registered");
			co_await sendErrorResponse<managarm::posix::WaitResponse>(conversation, managarm::posix::Errors::INTERNAL_ERROR);
			co_return {};
		}

		waitResult = co_await self->wait(req.pid(), flags, cancelEvent);
	}

	managarm::posix::WaitResponse resp;
	if(waitResult) {
		auto proc_state = waitResult.value();
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_pid(proc_state.pid);
		resp.set_ru_user_time(proc_state.stats.userTime);

		uint32_t status = 0;
		if(auto byExit = std::get_if<TerminationByExit>(&proc_state.state); byExit) {
			status |= W_EXITCODE(byExit->code, 0);
		}else if(auto bySignal = std::get_if<TerminationBySignal>(&proc_state.state); bySignal) {
			status |= W_EXITCODE(0, bySignal->signo);
		}else{
			assert(std::holds_alternative<std::monostate>(proc_state.state));
		}
		resp.set_status(status);
	} else if (waitResult.error() == Error::interrupted) {
		resp.set_error(managarm::posix::Errors::INTERRUPTED);
	} else if (waitResult.error() == Error::wouldBlock) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_pid(0);
	} else {
		resp.set_error(waitResult.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);

	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetAffinityRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_AFFINITY");

	auto target = self;

	if(self->pid() != req.pid()) {
		// TODO: permission checking
		target = Process::findProcess(req.pid());
		if(!target) {
			co_await sendErrorResponse<managarm::posix::SetAffinityResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}

	// Applications may pass masks that are larger than what thor accepts (e.g., sizeof(cpu_set_t)).
	// Like Linux, ignore the bits beyond the last CPU instead of rejecting the oversized mask.
	auto maskSize = std::min(req.mask().size(), getAffinityMaskSize());

	HelError e = target->setAffinity(req.mask().data(), maskSize);

	if(e == kHelErrIllegalArgs || e == kHelErrThreadTerminated) {
		co_await sendErrorResponse<managarm::posix::SetAffinityResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	} else if(e != kHelErrNone) {
		std::cout << "posix: SET_AFFINITY hel call returned unexpected error: " << e << std::endl;
		co_await sendErrorResponse<managarm::posix::SetAffinityResponse>(conversation, managarm::posix::Errors::INTERNAL_ERROR);
		co_return {};
	}

	managarm::posix::SetAffinityResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetAffinityRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_AFFINITY");

	if(!req.size()) {
		co_await sendErrorResponse<managarm::posix::GetAffinityResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	std::vector<uint8_t> affinity(req.size());

	auto target = self;

	if(req.pid() && self->pid() != req.pid()) {
		// TODO: permission checking
		target = Process::findProcess(req.pid());
		if(!target) {
			co_await sendErrorResponse<managarm::posix::GetAffinityResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}

	size_t actual_size;
	HelError e = target->getAffinity(affinity.data(), req.size(), &actual_size);

	if(e == kHelErrBufferTooSmall || e == kHelErrThreadTerminated) {
		co_await sendErrorResponse<managarm::posix::GetAffinityResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	} else if(e != kHelErrNone) {
		std::cout << "posix: GET_AFFINITY hel call returned unexpected error: " << e << std::endl;
		co_await sendErrorResponse<managarm::posix::GetAffinityResponse>(conversation, managarm::posix::Errors::INTERNAL_ERROR);
		co_return {};
	}

	managarm::posix::GetAffinityResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(self->pid());

	auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(affinity.data(), affinity.size())
	);
	HEL_CHECK(sendResp.error());
	HEL_CHECK(sendData.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetPgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_PGID");

	std::shared_ptr<ThreadGroup> target = nullptr;
	if(req.pid()) {
		target = ThreadGroup::findThreadGroup(req.pid());
		if(!target) {
			co_await sendErrorResponse<managarm::posix::GetPgidResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return {};
		}
	} else {
		target = self->threadGroup()->shared_from_this();
	}

	managarm::posix::GetPgidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(target->pgPointer()->getHull()->getPid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetPgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_PGID");

	std::shared_ptr<ThreadGroup> target;

	if (req.pgid() < 0) {
		// POSIX: reject negative `pgid` (or implementation-unsupported) values with EINVAL
		co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	} else if (req.pid() > 0) {
		target = ThreadGroup::findThreadGroup(req.pid());
		if (!target) {
			co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return {};
		}

		auto isSelf = req.pid() == self->pid();
		auto isChild = (!isSelf && target->getParent() && target->getParent()->pid() == self->pid());

		// POSIX: if `pid` is not the PID of the calling process or its children, ESRCH.
		if (!isSelf && !isChild) {
			co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return {};
		}

		// POSIX: if target process is not in the same session, EPERM.
		if (target->pgPointer()->getSession() != self->pgPointer()->getSession()) {
			co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
			co_return {};
		}

		// POSIX: if `pid` matches the process ID of a child and the child has successfully
		// executed one of the `exec*` functions, return EACCES.
		if (isChild && target->didExecute()) {
			co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::ACCESS_DENIED);
			co_return {};
		}
	} else {
		target = self->threadGroup()->shared_from_this();
	}

	// POSIX: We can't change the process group ID of the session leader, EPERM
	if (target->pid() == target->pgPointer()->getSession()->getSessionId()) {
		co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return {};
	}

	// If pgid is 0, we're going to set it to the calling process's pid, use the target group.
	auto resolvedPgid = req.pgid() ? req.pgid() : target->pid();
	std::shared_ptr<ProcessGroup> group = target->pgPointer()->getSession()->getProcessGroupById(resolvedPgid);

	if(group) {
		// Found, do permission checking and join
		group->reassociateProcess(target.get());
	} else {
		// Not found, making it if pgid and pid match, or if pgid is 0, indicating that we should make one
		if(target->pid() == req.pgid() || !req.pgid()) {
			target->pgPointer()->getSession()->spawnProcessGroup(target.get());
		} else {
			// POSIX: invalid `pgid` supplied, return EINVAL.
			co_await sendErrorResponse<managarm::posix::SetPgidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}

	// Wake any waiters (e.g. waitpid); this is necessary as that may need to return ECHLD if we
	// moved out the last child.
	target->getParent()->raiseNotifyBell();

	managarm::posix::SetPgidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetSidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_SID", "pid={}", req.pid());

	std::shared_ptr<ThreadGroup> target;
	if(req.pid()) {
		target = ThreadGroup::findThreadGroup(req.pid());
		if(!target) {
			co_await sendErrorResponse<managarm::posix::GetSidResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return {};
		}
	} else {
		target = self->threadGroup()->shared_from_this();
	}
	managarm::posix::GetSidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(target->pgPointer()->getSession()->getSessionId());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetSidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "SETSID");

	// POSIX: if the calling process is already a group leader, EPERM.
	if(self->pgPointer()->getSession()->getSessionId() == self->pid()) {
		co_await sendErrorResponse<managarm::posix::SetSidResponse>(conversation, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return {};
	}

	auto session = TerminalSession::initializeNewSession(self->threadGroup());

	// Wake any waiters (e.g. waitpid); this is necessary as that may need to return ECHLD if we
	// moved out the last child.
	self->getParent()->raiseNotifyBell();

	managarm::posix::SetSidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_sid(session->getSessionId());

	auto ser = resp.SerializeAsString();
	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
	HEL_CHECK(send_resp.error());
	logBragiSerializedReply(ser);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::ParentDeathSignalRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	self->threadGroup()->setParentDeathSignal(req.signal() ? std::optional{req.signal()} : std::nullopt);

	managarm::posix::ParentDeathSignalResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::ProcessDumpableRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	managarm::posix::ProcessDumpableResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if(req.set()) {
		self->threadGroup()->setDumpable(req.new_value());
	}

	resp.set_value(self->threadGroup()->getDumpable());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetResourceLimitRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	managarm::posix::SetResourceLimitResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	switch(req.resource()) {
		case RLIMIT_NOFILE:
			self->fileContext()->setFdLimit(req.cur());
			break;
		default:
			resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			break;
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetResourceUsageRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "GET_RESOURCE_USAGE");

	HelThreadStats stats;
	HEL_CHECK(helQueryThreadStats(self->threadDescriptor().getHandle(), &stats));

	int32_t mode = static_cast<int32_t>(req.mode());
	uint64_t user_time;
	if(mode == RUSAGE_SELF) {
		user_time = stats.userTime;
	}else if(mode == RUSAGE_CHILDREN) {
		user_time = self->threadGroup()->accumulatedUsage().userTime;
	}else if(mode == RLIMIT_FSIZE) {
		user_time = RLIM_INFINITY;
	}else{
		std::println("\e[31mposix: GET_RESOURCE_USAGE mode is not supported, requested mode: {}\e[39m", mode);
		user_time = 0;
		// TODO: Return an error response.
	}

	managarm::posix::GetResourceUsageResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_ru_user_time(user_time);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SigactionRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logRequest(logRequests, self, "SIG_ACTION");

	if(req.flags() & ~(SA_ONSTACK | SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT)) {
		std::cout << "\e[31mposix: Unknown SIG_ACTION flags: 0x"
				<< std::hex << req.flags()
				<< std::dec << "\e[39m" << std::endl;
		assert(!"Flags not implemented");
	}

	managarm::posix::SigactionResponse resp;

	if(req.sig_number() <= 0 || req.sig_number() > 64) {
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	auto removePendingSignal = [&](int signo) {
		// This function does *not* drop delayedSignals (if we did, we would cause spurious EINTR).

		for (auto &thread : self->threadGroup()->threads())
			while (auto item = thread->tryFetchSignal(UINT64_C(1) << (signo - 1)))
				delete item;
	};

	std::set<int> defaultIgnoredSignals = {SIGCHLD, SIGURG, SIGWINCH};

	SignalHandler saved_handler;
	if(req.mode()) {
		SignalHandler handler;
		if(req.sig_handler() == uintptr_t(SIG_DFL)) {
			handler.disposition = SignalDisposition::none;
			// POSIX requires discarding pending signals when setting SIG_DFL for signals,
			// if their default action is to ignore (POSIX 2024, B.2.4.3 Signal Actions)
			if (defaultIgnoredSignals.contains(req.sig_number()))
				removePendingSignal(req.sig_number());
		}else if(req.sig_handler() == uintptr_t(SIG_IGN)) {
			// POSIX requires discarding pending signals when setting SIG_IGN
			handler.disposition = SignalDisposition::ignore;
			removePendingSignal(req.sig_number());
		}else{
			handler.disposition = SignalDisposition::handle;
			handler.handlerIp = req.sig_handler();
		}

		handler.flags = 0;
		handler.mask = req.sig_mask();
		handler.restorerIp = req.sig_restorer();

		if(req.flags() & SA_SIGINFO)
			handler.flags |= signalInfo;
		if(req.flags() & SA_RESETHAND)
			handler.flags |= signalOnce;
		if(req.flags() & SA_NODEFER)
			handler.flags |= signalReentrant;
		if(req.flags() & SA_ONSTACK)
			handler.flags |= signalOnStack;
		if(req.flags() & SA_NOCLDSTOP)
			std::cout << "\e[31mposix: Ignoring SA_NOCLDSTOP\e[39m" << std::endl;
		if(req.flags() & SA_NOCLDWAIT)
			handler.flags |= signalNoChildWait;

		saved_handler = self->threadGroup()->signalContext()->changeHandler(req.sig_number(), handler);
	}else{
		saved_handler = self->threadGroup()->signalContext()->getHandler(req.sig_number());
	}

	int saved_flags = 0;
	if(saved_handler.flags & signalInfo)
		saved_flags |= SA_SIGINFO;
	if(saved_handler.flags & signalOnce)
		saved_flags |= SA_RESETHAND;
	if(saved_handler.flags & signalReentrant)
		saved_flags |= SA_NODEFER;
	if(saved_handler.flags & signalOnStack)
		saved_flags |= SA_ONSTACK;
	if(saved_handler.flags & signalNoChildWait)
		saved_flags |= SA_NOCLDWAIT;

	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_flags(saved_flags);
	resp.set_sig_mask(saved_handler.mask);
	if(saved_handler.disposition == SignalDisposition::handle) {
		resp.set_sig_handler(saved_handler.handlerIp);
		resp.set_sig_restorer(saved_handler.restorerIp);
	}else if(saved_handler.disposition == SignalDisposition::none) {
		resp.set_sig_handler((uint64_t)SIG_DFL);
	}else{
		assert(saved_handler.disposition == SignalDisposition::ignore);
		resp.set_sig_handler((uint64_t)SIG_IGN);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
