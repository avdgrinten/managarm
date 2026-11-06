
#include <async/cancellation.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <async/recurring-event.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include "process.hpp"
#include "protocols/fs/common.hpp"
#include "signalfd.hpp"

namespace signal_fd {

void OpenFile::serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
			smarter::shared_ptr<File>{file}, &File::fileOperations, file->cancelServe_));
}

OpenFile::OpenFile(uint64_t mask, bool nonBlock)
: FileWithDefaults{FileKind::unknown,  StructName::get("signalfd"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)}, _mask{mask}, _nonBlock{nonBlock} { }

async::result<std::expected<size_t, Error>>
OpenFile::readSome(Process *process, void *data, size_t maxLength, async::cancellation_token ce) {
	if(maxLength < sizeof(struct signalfd_siginfo))
		co_return std::unexpected{Error::illegalArguments};

	auto active = co_await process->fetchSignal(_mask, _nonBlock, ce);
	if (!active && _nonBlock)
		co_return std::unexpected{Error::wouldBlock};
	if (!active && !_nonBlock)
		co_return std::unexpected{Error::interrupted};

	struct signalfd_siginfo si = {};
	si.ssi_signo = active->signalNumber;
	std::visit(CompileSignalFdInfo{&si}, active->info);

	memcpy(data, &si, sizeof(struct signalfd_siginfo));
	co_return sizeof(struct signalfd_siginfo);
}

// Not a coroutine: the process is only borrowed for the duration of this call, so it has to be
// pinned here and not in the coroutine body, which may start running after we return.
async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollWait(Process *process, uint64_t inSeq, int pollMask,
		async::cancellation_token cancellation) {
	// epoll items outlive the thread that registered them; that thread receives no more signals.
	// Without EPOLLIN, there is nothing to wait for either.
	// TODO: An exited thread stays pinned until a matching signal arrives or the epoll item is
	//       modified or removed (once the signalfd is closed, only a signal or closing the epoll
	//       does that), and its items go idle even if the thread group still receives signals.
	//       Linux has neither issue: items wait on the sighand of whoever called EPOLL_CTL_ADD,
	//       each epoll_wait() evaluates readiness for its own caller, and signalfd_cleanup()
	//       detaches the waits when the sighand goes away.
	std::shared_ptr<Process> pinnedProcess;
	if (process && (EPOLLIN & pollMask))
		pinnedProcess = process->shared_from_this();
	return pollSignal_(std::move(pinnedProcess), inSeq, cancellation);
}

// Waits indefinitely if process is null.
async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollSignal_(std::shared_ptr<Process> process, uint64_t inSeq,
		async::cancellation_token cancellation) {
	if (!process) {
		co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{inSeq, 0};
	}

	auto [newSeq, signalEdges] = co_await process->pollSignal(inSeq, _mask, cancellation);

	if (newSeq > inSeq && (signalEdges & _mask))
		co_return PollWaitResult{newSeq, EPOLLIN};
	co_return PollWaitResult{newSeq, 0};
}

async::result<frg::expected<Error, PollStatusResult>>
OpenFile::pollStatus(Process *process) {
	if (!process)
		co_return PollStatusResult{0, 0};
	auto [seq, active] = process->checkSignal();
	co_return PollStatusResult{seq, (active & _mask) ? EPOLLIN : 0};
}

} // namespace signal_fd

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask, bool nonBlock) {
	auto file = smarter::make_shared<signal_fd::OpenFile>(mask, nonBlock);
	file->setupWeakFile(file);
	signal_fd::OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

