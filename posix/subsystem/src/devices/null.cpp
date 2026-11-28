#include <string.h>
#include <helix/dispatcher-pool.hpp>
#include <sys/epoll.h>

#include "../common.hpp"
#include "null.hpp"

#include <atomic>
#include <bitset>
#include <print>

namespace {

struct NullFile final : FileWithDefaults {
private:
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *, size_t, async::cancellation_token) override {
		co_return std::unexpected{Error::eof};
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *, size_t length) override {
		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t, VfsSeek) override {
		co_return 0;
	}

	// Like on Linux, reads (which return EOF) and writes never block.
	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask;

		if(sequence > 1)
			co_return Error::illegalArguments;

		if(sequence)
			co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{1, EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{1, EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM};
	}

	async::result<int> getFileFlags() override {
		int flags = 0;

		if(_nonBlock.load(std::memory_order_relaxed))
			flags |= O_NONBLOCK;

		if(_read && _write)
			flags |= O_RDWR;
		else if(_write)
			flags |= O_WRONLY;
		else
			flags |= O_RDONLY;

		co_return flags;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~O_NONBLOCK) {
			std::println("posix: setFileFlags on \e[1;34m{}\e[0m called with unknown flags {:#x}",
				structName(), flags & ~O_NONBLOCK);
			co_return;
		}

		_nonBlock.store(flags & O_NONBLOCK, std::memory_order_relaxed);
		co_return;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override {
		_cancelServe.cancel();
		_passthrough = {};
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;
	const bool _read;
	const bool _write;
	std::atomic<bool> _nonBlock{false};

public:
	static void serve(smarter::shared_ptr<NullFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	NullFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags)
	: FileWithDefaults{FileKind::unknown,  StructName::get("null-file"), std::move(mount), std::move(link)},
			_read{static_cast<bool>(semantic_flags & semanticRead)},
			_write{static_cast<bool>(semantic_flags & semanticWrite)} { }
};

struct NullDevice final : UnixDevice {
	NullDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 3});
	}

	std::string nodePath() override {
		return "null";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: NullFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<NullFile>(std::move(mount), std::move(link), semantic_flags);
		file->setupWeakFile(file);
		NullFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createNullDevice() {
	return std::make_shared<NullDevice>();
}
