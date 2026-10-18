#include "common.hpp"
#include "../epoll.hpp"
#include "../eventfd.hpp"
#include "../fifo.hpp"
#include "../inotify.hpp"
#include "../pidfd.hpp"
#include "../signalfd.hpp"
#include "../timerfd.hpp"
#include <sys/inotify.h>
#include <sys/pidfd.h>
#include <sys/timerfd.h>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::InotifyCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "INOTIFY_CREATE");

	if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC | managarm::posix::OpenFlags::OF_NONBLOCK)) {
		co_await sendErrorResponse<managarm::posix::InotifyCreateResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto file = inotify::createFile(req.flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
	auto fd = self->fileContext()->attachFile(file,
			req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::InotifyCreateResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
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
HandleRequest::operator()(managarm::posix::InotifyAddRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	managarm::posix::InotifyAddResponse resp;

	logRequest(logRequests || logPaths, self, "INOTIFY_ADD");

	auto ifile = self->fileContext()->getFile(req.fd());
	if(!ifile) {
		co_await sendErrorResponse<managarm::posix::InotifyAddResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	} else if(ifile->kind() != FileKind::inotify) {
		co_await sendErrorResponse<managarm::posix::InotifyAddResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	ResolveFlags flags = 0;

	if(req.flags() & IN_DONT_FOLLOW)
		flags |= resolveDontFollow;

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			self->fsContext()->getWorkingDirectory(), req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(flags);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::InotifyAddResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::InotifyAddResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	auto wd = inotify::addWatch(ifile.get(), resolver.currentLink()->getTarget(),
			req.flags());

	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_wd(wd);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::InotifyRmRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests || logPaths, self, "INOTIFY_RM");
	managarm::posix::InotifyRmReply resp;

	auto ifile = self->fileContext()->getFile(req.ifd());
	if(!ifile) {
		resp.set_error(managarm::posix::Errors::BAD_FD);
		co_return {};
	} else if(ifile->kind() != FileKind::inotify) {
		co_await sendErrorResponse<managarm::posix::InotifyRmReply>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(inotify::removeWatch(ifile.get(), req.wd()))
		resp.set_error(managarm::posix::Errors::SUCCESS);
	else
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::EventfdCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "EVENTFD_CREATE");

	managarm::posix::EventfdCreateResponse resp;

	if (req.flags() & ~(managarm::posix::EventFdFlags::CLOEXEC
			| managarm::posix::EventFdFlags::NONBLOCK
			| managarm::posix::EventFdFlags::SEMAPHORE)) {
		std::cout << "posix: invalid flag specified" << std::endl;
		std::cout << "posix: flags specified: " << req.flags() << std::endl;
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		auto file = eventfd::createFile(req.initval(),
			req.flags() & managarm::posix::EventFdFlags::NONBLOCK,
			req.flags() & managarm::posix::EventFdFlags::SEMAPHORE);
		auto fd = self->fileContext()->attachFile(file,
				req.flags() & managarm::posix::EventFdFlags::CLOEXEC);

		if (fd) {
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd.value());
		} else {
			resp.set_error(fd.error() | toPosixProtoError);
		}
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
HandleRequest::operator()(managarm::posix::TimerFdCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_FD_CREATE");

	if(req.flags() & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		std::println("posix: Unsupported flags {} for timerfd_create()", req.flags());
		co_await sendErrorResponse<managarm::posix::TimerFdCreateResponse>(conversation,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return {};
	}

	if (req.clock() != CLOCK_MONOTONIC && req.clock() != CLOCK_REALTIME) {
		std::println("posix: timerfd is not supported for clock {}", req.clock());
		co_await sendErrorResponse<managarm::posix::TimerFdCreateResponse>(conversation,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return {};
	}

	auto file = timerfd::createFile(req.clock(), req.flags() & TFD_NONBLOCK);
	auto fd = self->fileContext()->attachFile(file, req.flags() & TFD_CLOEXEC);

	managarm::posix::TimerFdCreateResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerFdSetRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_FD_SET");

	auto file = self->fileContext()->getFile(req.fd());
	if (!file) {
		co_await sendErrorResponse<managarm::posix::TimerFdSetResponse>(conversation,
			managarm::posix::Errors::NO_SUCH_FD
		);
		co_return {};
	} else if(file->kind() != FileKind::timerfd) {
		co_await sendErrorResponse<managarm::posix::TimerFdSetResponse>(conversation,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return {};
	}
	timespec initial = {};
	timespec interval = {};
	timerfd::setTime(file.get(), req.flags(),
			{static_cast<time_t>(req.value_sec()), static_cast<long>(req.value_nsec())},
			{static_cast<time_t>(req.interval_sec()), static_cast<long>(req.interval_nsec())},
			initial, interval);

	managarm::posix::TimerFdSetResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_value_sec(initial.tv_sec);
	resp.set_value_nsec(initial.tv_nsec);
	resp.set_interval_sec(interval.tv_sec);
	resp.set_interval_nsec(interval.tv_nsec);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TimerFdGetRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "TIMER_FD_GET");

	auto file = self->fileContext()->getFile(req.fd());
	if (!file) {
		co_await sendErrorResponse<managarm::posix::TimerFdGetResponse>(conversation,
			managarm::posix::Errors::NO_SUCH_FD
		);
		co_return {};
	} else if(file->kind() != FileKind::timerfd) {
		co_await sendErrorResponse<managarm::posix::TimerFdGetResponse>(conversation,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return {};
	}
	timespec initial = {};
	timespec interval = {};
	timerfd::getTime(file.get(), initial, interval);

	managarm::posix::TimerFdGetResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_value_sec(initial.tv_sec);
	resp.set_value_nsec(initial.tv_nsec);
	resp.set_interval_sec(interval.tv_sec);
	resp.set_interval_nsec(interval.tv_nsec);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::PidfdOpenRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	auto proc = ThreadGroup::findThreadGroup(req.pid());
	if(!proc) {
		co_await sendErrorResponse<managarm::posix::PidfdOpenResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto pidfd = createPidfdFile(proc->weak_from_this(), req.flags() & PIDFD_NONBLOCK);
	auto fd = self->fileContext()->attachFile(pidfd, req.flags() & PIDFD_NONBLOCK);

	managarm::posix::PidfdOpenResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
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
HandleRequest::operator()(managarm::posix::PidfdSendSignalRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	auto fd = self->fileContext()->getFile(req.pidfd());
	if(!fd || fd->kind() != FileKind::pidfd) {
		co_await sendErrorResponse<managarm::posix::PidfdSendSignalResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto target = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->threadGroup();
	if(!target) {
		co_await sendErrorResponse<managarm::posix::PidfdSendSignalResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
		co_return {};
	}

	if(req.signal() > 0) {
		UserSignal info;
		info.pid = self->pid();
		info.uid = 0;
		target->issueThreadGroupSignal(req.signal(), info);
	}

	managarm::posix::PidfdSendSignalResponse resp;
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
HandleRequest::operator()(managarm::posix::PidfdGetPidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	auto fd = self->fileContext()->getFile(req.pidfd());
	if(!fd || fd->kind() != FileKind::pidfd) {
		co_await sendErrorResponse<managarm::posix::PidfdGetPidResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto pid = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->pid();
	if(pid <= 0) {
		co_await sendErrorResponse<managarm::posix::PidfdGetPidResponse>(conversation, managarm::posix::Errors::NO_SUCH_RESOURCE);
		co_return {};
	}

	managarm::posix::PidfdGetPidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(pid);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::EpollCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "EPOLL_CREATE");

	assert(!(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)));

	auto file = epoll::createFile();
	auto fd = self->fileContext()->attachFile(file,
			req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::EpollCreateResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::PipeCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "PIPE_CREATE");

	assert(!(req.flags() & ~(O_CLOEXEC | O_NONBLOCK)));

	bool nonBlock = false;

	if(req.flags() & O_NONBLOCK)
		nonBlock = true;

	auto pair = fifo::createPair(nonBlock);
	auto r_fd = self->fileContext()->attachFile(std::get<0>(pair),
			req.flags() & O_CLOEXEC);
	auto w_fd = self->fileContext()->attachFile(std::get<1>(pair),
			req.flags() & O_CLOEXEC);

	managarm::posix::PipeCreateResponse resp;
	if (r_fd && w_fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fds({r_fd.value(), w_fd.value()});
	} else {
		resp.set_error((!r_fd ? r_fd.error() : w_fd.error()) | toPosixProtoError);
		if (r_fd)
			self->fileContext()->closeFile(r_fd.value());
		if (w_fd)
			self->fileContext()->closeFile(w_fd.value());
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SignalfdCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "SIGNALFD_CREATE");

	if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC
			| managarm::posix::OpenFlags::OF_NONBLOCK)) {
		co_await sendErrorResponse<managarm::posix::SignalfdCreateResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	managarm::posix::SignalfdCreateResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if(req.fd() == -1) {
		auto file = createSignalFile(req.sigset(),
				req.flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
		auto fd = self->fileContext()->attachFile(file,
				req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

		if (fd)
			resp.set_fd(fd.value());
		else
			resp.set_error(fd.error() | toPosixProtoError);
	} else {
		auto file = self->fileContext()->getFile(req.fd());
		if(file) {
			auto signal_file = static_cast<signal_fd::OpenFile *>(file.get());
			signal_file->setMask(req.sigset());
			resp.set_fd(req.fd());
		} else {
			resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);
		}
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
