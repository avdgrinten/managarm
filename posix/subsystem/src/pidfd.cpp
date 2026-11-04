#include <async/recurring-event.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include "pidfd.hpp"
#include "process.hpp"

namespace pidfd {

void OpenFile::serve(smarter::shared_ptr<OpenFile> file) {
	helix::UniqueLane lane;
	std::tie(lane, file->passthrough_) = helix::createStream();
	helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
			smarter::shared_ptr<File>{file}, &File::fileOperations, file->cancelServe_));
}

OpenFile::OpenFile(std::weak_ptr<ThreadGroup> proc, bool nonBlock)
: FileWithDefaults{FileKind::pidfd,  StructName::get("pidfd"), nullptr,
	SpecialLink::makeSpecialLink(VfsType::regular, 0777), defaultPipeLikeSeek},
	nonBlock_{nonBlock}, process_{std::move(proc)} { }

async::result<std::expected<size_t, Error>>
OpenFile::readSome(Process *, void *, size_t, async::cancellation_token) {
	co_return std::unexpected{Error::illegalArguments};
}

async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollWait(Process *, uint64_t inSeq, int pollMask,
		async::cancellation_token cancellation) {
	auto p = process_.lock();
	if(!p && EPOLLIN & pollMask)
		co_return PollWaitResult(1, EPOLLIN & pollMask);
	if(!isOpen())
		co_return Error::fileClosed;

	if(inSeq > 1) {
		co_return Error::illegalArguments;
	} else if(inSeq == 1 || (EPOLLIN & pollMask) != EPOLLIN) {
		co_await async::suspend_indefinitely(cancellation);
	} else {
		while(p->notifyType() != NotifyType::terminated) {
			if(!isOpen())
				co_return Error::fileClosed;

			if (!co_await p->awaitNotifyTypeChange(cancellation))
				break;
		}
	}

	co_return PollWaitResult(1, (p->notifyType() == NotifyType::terminated ? EPOLLIN : 0) & pollMask);
}

async::result<frg::expected<Error, PollStatusResult>>
OpenFile::pollStatus(Process *) {
	auto p = process_.lock();
	if(!p)
		co_return PollStatusResult{1, EPOLLIN};

	bool terminated = p->notifyType() == NotifyType::terminated;
	co_return PollStatusResult(terminated, terminated ? EPOLLIN : 0);
}

async::result<std::string> OpenFile::getFdInfo() {
	auto p = process_.lock();
	if(!p)
		co_return "Pid:\t-1\n";

	int pid = p->pid();
	if(p->notifyType() == NotifyType::terminated)
		pid = -1;
	co_return std::format("Pid:\t{}\n", pid);
}

void OpenFile::handleClose() {
	cancelServe_.cancel();
	passthrough_ = {};
}

int OpenFile::pid() const {
	auto p = process_.lock();
	if(!p)
		return -1;
	return p->pid();
}

std::shared_ptr<ThreadGroup> OpenFile::threadGroup() {
	return process_.lock();
}

} // namespace pidfd

smarter::shared_ptr<File, FileHandle> createPidfdFile(std::weak_ptr<ThreadGroup> proc, bool nonBlock) {
	auto file = smarter::make_shared<pidfd::OpenFile>(proc, nonBlock);
	file->setupWeakFile(file);
	pidfd::OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

