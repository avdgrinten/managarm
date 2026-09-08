
#include <async/cancellation.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <iostream>
#include <map>
#include <print>

#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <frg/ringbuffer.hpp>
#include <helix/ipc.hpp>
#include "fifo.hpp"
#include "fs.bragi.hpp"

#include <sys/ioctl.h>

namespace fifo {

namespace {

constexpr bool logFifos = false;

constexpr size_t defaultFifoBufferSize = 65536;

struct Channel {
	Channel(size_t capacity) : writerCount{0}, readerCount{0}, ring{capacity} {
		assert(capacity);
		assert(std::has_single_bit(capacity));
	}

	// Status management for poll().
	async::recurring_event statusBell;
	// Start at currentSeq = 1 since the pipe is always writable.
	uint64_t currentSeq = 1;
	uint64_t noWriterSeq = 0;
	uint64_t noReaderSeq = 0;
	uint64_t inSeq = 0;
	uint64_t outSeq = 1;
	int writerCount;
	int readerCount;

	async::recurring_event readerPresent;
	async::recurring_event writerPresent;

	frg::byte_ring_buffer<frg::stl_allocator> ring;
};

struct OpenFile : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations));
	}

	OpenFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
		bool isReader, bool isWriter, bool nonBlock = false)
	: FileWithDefaults{FileKind::unknown,  StructName::get("fifo"), mount, link, File::defaultPipeLikeSeek},
		isReader_{isReader}, isWriter_{isWriter}, nonBlock_{nonBlock} { }

	void connectChannel(std::shared_ptr<Channel> channel) {
		assert(!_channel);
		_channel = std::move(channel);
		if (isReader_)
			_channel->readerCount++;
		if (isWriter_)
			_channel->writerCount++;
	}

	void handleClose() override {
		if (isReader_) {
			if (_channel->readerCount-- == 1) {
				_channel->noReaderSeq = ++_channel->currentSeq;
				_channel->statusBell.raise();
			}
		}
		if (isWriter_) {
			if(_channel->writerCount-- == 1) {
				_channel->noWriterSeq = ++_channel->currentSeq;
				_channel->statusBell.raise();
			}
		}
		_channel = nullptr;
	}

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) override {
		if (logFifos)
			std::cout << "posix: Read from pipe " << this << std::endl;
		if (!isReader_)
			co_return std::unexpected{Error::insufficientPermissions};
		if (!maxLength)
			co_return size_t{0};

		size_t chunk = 0;
		while (true) {
			chunk = _channel->ring.dequeue({static_cast<uint8_t *>(data), maxLength});
			if (chunk)
				break;

			if (!_channel->writerCount)
				co_return std::unexpected{Error::eof};
			else if(nonBlock_)
				co_return std::unexpected{Error::wouldBlock};

			if (!(co_await _channel->statusBell.async_wait_if([&]() {
				return !_channel->ring.size();
			}, ce)))
				co_return std::unexpected{Error::interrupted};
		}

		_channel->outSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
		co_return chunk;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t maxLength) override {
		if (!isWriter_)
			co_return Error::insufficientPermissions;
		if (!_channel->readerCount)
			co_return Error::brokenPipe; // TODO: SIGPIPE
		if (!maxLength)
			co_return 0;

		size_t chunk = 0;
		while (true) {
			chunk = _channel->ring.enqueue({static_cast<const uint8_t *>(data), maxLength});
			if (chunk)
				break;

			if (nonBlock_)
				co_return Error::wouldBlock;

			co_await _channel->statusBell.async_wait_if([&]() {
				return !_channel->ring.available_space();
			}); // TODO: EINTR
		}

		_channel->inSeq = ++_channel->currentSeq;
		_channel->statusBell.raise();
		co_return chunk;
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t pastSeq, int mask,
			async::cancellation_token cancellation) override {
		assert(pastSeq <= _channel->currentSeq);
		int edges = 0;

		while (true) {
			if(!_channel)
				co_return Error::fileClosed;

			edges = 0;
			if (isReader_) {
				if(_channel->noWriterSeq > pastSeq)
					edges |= EPOLLHUP;
				if(_channel->inSeq > pastSeq)
					edges |= EPOLLIN;
			}
			if (isWriter_) {
				if(_channel->outSeq > pastSeq)
					edges |= EPOLLOUT;
				if(_channel->noReaderSeq > pastSeq)
					edges |= EPOLLERR;
			}

			if (edges & mask)
				break;

			if (!co_await _channel->statusBell.async_wait(cancellation))
				break;
		}

		// std::println("posix: pollWait({}, {}) on \e[1;34m{}\e[0m returns ({}, {})",
		// 	pastSeq, mask, structName(), _channel->currentSeq, edges & mask);

		co_return PollWaitResult(_channel->currentSeq, edges & mask);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		int events = 0;
		if (isReader_) {
			if(!_channel->writerCount)
				events |= EPOLLHUP;
			if(!_channel->ring.empty())
				events |= EPOLLIN;
		}
		if (isWriter_) {
			if(_channel->ring.available_space())
				events |= EPOLLOUT;
			if(!_channel->readerCount)
				events |= EPOLLERR;
		}

		co_return PollStatusResult(_channel->currentSeq, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~O_NONBLOCK) {
			std::println("posix: setFileFlags on FIFO \e[1;34m{}\e[0m called with unknown flags {:#x}",
				structName(), flags & ~O_NONBLOCK);
			co_return;
		}
		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		int flags;
		if (isReader_ && isWriter_) {
			flags = O_RDWR;
		} else if(isReader_) {
			flags = O_RDONLY;
		} else {
			assert(isWriter_);
			flags = O_WRONLY;
		}

		if(nonBlock_)
			flags |= O_NONBLOCK;
		co_return flags;
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, OpenFile *self) {
			managarm::fs::GenericIoctlReply resp;

			switch(req.command()) {
				case FIONREAD: {
					size_t count = 0;
					if (self->isReader_)
						count = self->_channel->ring.size();

					resp.set_fionread_count(count);
					resp.set_error(managarm::fs::Errors::SUCCESS);

					break;
				}
				default: {
					auto [dismiss] = co_await helix_ng::exchangeMsgs(
						conversation, helix_ng::dismiss());
					HEL_CHECK(dismiss.error());
					co_return {};
				}
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}
	};

	async::result<void>
	ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if (!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

private:
	helix::UniqueLane _passthrough;

	std::shared_ptr<Channel> _channel;

	bool isReader_;
	bool isWriter_;
	bool nonBlock_;
};

} // anonymous namespace

// This maps FsNodes to Channels for named pipes (FIFOs)
std::map<FsNode *, std::shared_ptr<Channel>> globalChannelMap;

// TODO: Instead of relying on this function, openNamedChannel() should associate
//       the FsNode with a Channel on demand.
void createNamedChannel(FsNode *node) {
	assert(globalChannelMap.find(node) == globalChannelMap.end());
	globalChannelMap[node] = std::make_shared<Channel>(defaultFifoBufferSize);
}

void unlinkNamedChannel(FsNode *node) {
	assert(globalChannelMap.find(node) != globalChannelMap.end());
	globalChannelMap.erase(node);
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openNamedChannel(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, FsNode *node, SemanticFlags flags) {
	if (globalChannelMap.find(node) == globalChannelMap.end())
		co_return nullptr;

	auto channel = globalChannelMap.at(node);

	bool nonBlock = flags & semanticNonBlock;
	if ((flags & semanticRead) && (flags & semanticWrite)) {
		auto rw_file = smarter::make_shared<OpenFile>(mount, link, true, true, nonBlock);
		rw_file->setupWeakFile(rw_file);
		rw_file->connectChannel(channel);

		channel->readerPresent.raise();
		channel->writerPresent.raise();

		OpenFile::serve(rw_file);

		co_return File::constructHandle(std::move(rw_file));
	} else if (flags & semanticRead) {
		auto r_file = smarter::make_shared<OpenFile>(mount, link, true, false, nonBlock);
		r_file->setupWeakFile(r_file);
		r_file->connectChannel(channel);

		channel->readerPresent.raise();
		if (!channel->writerCount && !nonBlock)
			co_await channel->writerPresent.async_wait();

		OpenFile::serve(r_file);

		co_return File::constructHandle(std::move(r_file));
	} else if (flags & semanticWrite) {
		auto w_file = smarter::make_shared<OpenFile>(mount, link, false, true, nonBlock);
		w_file->setupWeakFile(w_file);
		w_file->connectChannel(channel);

		channel->writerPresent.raise();
		if (!channel->readerCount && !nonBlock)
			co_await channel->readerPresent.async_wait();

		// TODO: Opening for write-only with no reader present should return NXIO (man 7 fifo).

		OpenFile::serve(w_file);

		co_return File::constructHandle(std::move(w_file));
	} else {
		co_return Error::illegalArguments;
	}
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair(bool nonBlock) {
	auto link = SpecialLink::makeSpecialLink(VfsType::fifo, 0777);
	auto channel = std::make_shared<Channel>(defaultFifoBufferSize);
	auto r_file = smarter::make_shared<OpenFile>(nullptr, link, true, false, nonBlock);
	auto w_file = smarter::make_shared<OpenFile>(nullptr, link, false, true, nonBlock);
	r_file->setupWeakFile(r_file);
	w_file->setupWeakFile(w_file);
	r_file->connectChannel(channel);
	w_file->connectChannel(channel);
	OpenFile::serve(r_file);
	OpenFile::serve(w_file);
	return {File::constructHandle(std::move(r_file)),
			File::constructHandle(std::move(w_file))};
}

} // namespace fifo

