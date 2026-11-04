#include <async/cancellation.hpp>
#include <async/recurring-event.hpp>
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <iostream>
#include <mutex>
#include <print>
#include <protocols/fs/common.hpp>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>

#include "fs.hpp"
#include "inotify.hpp"
#include "process.hpp"

namespace inotify {

namespace {

constexpr int supportedFlags = IN_DELETE | IN_CREATE | IN_ISDIR | IN_DELETE_SELF | IN_MODIFY | IN_ACCESS | IN_CLOSE;
constexpr int alwaysReturnedFlags = IN_IGNORED | IN_ISDIR | IN_Q_OVERFLOW | IN_UNMOUNT;

struct OpenFile : FileWithDefaults {
public:
	struct Packet {
		int descriptor;
		uint32_t events;
		std::string name;
		uint32_t cookie;
	};

	struct Watch final : FsObserver {
		Watch(smarter::weak_ptr<File> file_, int descriptor, uint32_t mask, smarter::weak_ptr<FsNode> node)
		: file{file_}, descriptor{descriptor}, mask{mask}, node{node} { }

		void observeNotification(uint32_t events,
				const std::string &name, uint32_t cookie, bool isDir) override {
			uint32_t inotifyEvents = 0;
			if(events & FsObserver::deleteEvent)
				inotifyEvents |= IN_DELETE;
			if(events & FsObserver::deleteSelfEvent)
				inotifyEvents |= IN_DELETE_SELF;
			if(events & FsObserver::createEvent)
				inotifyEvents |= IN_CREATE;
			if(events & FsObserver::modifyEvent)
				inotifyEvents |= IN_MODIFY;
			if(events & FsObserver::accessEvent)
				inotifyEvents |= IN_ACCESS;
			if(events & FsObserver::closeWriteEvent)
				inotifyEvents |= IN_CLOSE_WRITE;
			if(events & FsObserver::closeNoWriteEvent)
				inotifyEvents |= IN_CLOSE_NOWRITE;
			if(events & FsObserver::ignoredEvent)
				inotifyEvents |= IN_IGNORED;

			auto f = smarter::static_pointer_cast<OpenFile>(file.lock());
			if(!f)
				return;

			{
				std::lock_guard lock{f->mutex_};
				// FsNode may still notify us after removeWatch(); nothing may follow IN_IGNORED.
				if(removed)
					return;
				if(!queueLocked(f.get(), inotifyEvents, name, cookie, isDir))
					return;
			}
			f->_statusBell.raise();
		}

		// Must be called with the mutex_ of the OpenFile held. Returns whether an event was queued.
		bool queueLocked(OpenFile *f, uint32_t inotifyEvents, const std::string &name,
				uint32_t cookie, bool isDir) {
			auto preexists = std::ranges::find_if(f->_queue, [&name, inotifyEvents](auto a) {
				return a.name == name && (a.events & inotifyEvents) == inotifyEvents;
			}) != f->_queue.end();
			if(preexists)
				return false;

			if(isDir)
				inotifyEvents |= IN_ISDIR;

			if(!(inotifyEvents & (mask | alwaysReturnedFlags)))
				return false;

			f->_queue.push_back(Packet{descriptor, inotifyEvents & (mask | alwaysReturnedFlags), name, cookie});
			f->_inSeq = ++f->_currentSeq;
			return true;
		}

		smarter::weak_ptr<File> file;
		int descriptor;
		// Protected by the mutex_ of the OpenFile.
		uint32_t mask;
		// Protected by the mutex_ of the OpenFile.
		bool removed = false;
		smarter::weak_ptr<FsNode> node;
	};

	static void serve(smarter::shared_ptr<OpenFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
			smarter::shared_ptr<File>{file}, &File::fileOperations, file->cancelServe_));
	}

	void handleClose() override {
		cancelServe_.cancel();
		_passthrough = {};

		std::unordered_map<int, std::shared_ptr<Watch>> watches;
		{
			std::lock_guard lock{mutex_};
			watches = std::move(watches_);
		}

		for(auto [_, watch] : watches) {
			auto node = watch->node.lock();

			if(node)
				node->removeObserver(watch.get());
		}
	}

	OpenFile(bool nonBlock = false)
	: FileWithDefaults{FileKind::inotify,  StructName::get("inotify"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777)}, nonBlock_{nonBlock} { }

	~OpenFile() override { }

	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t maxLength, async::cancellation_token ce) override {
		while(true) {
			{
				std::lock_guard lock{mutex_};
				if(!_queue.empty())
					co_return dequeueLocked(data, maxLength);
				if(nonBlock_)
					co_return std::unexpected{Error::wouldBlock};
			}

			auto result = co_await _statusBell.async_wait_if([&] {
				std::lock_guard lock{mutex_};
				return _queue.empty();
			}, ce);
			if (!result)
				co_return std::unexpected{Error::interrupted};
		}
	}

	// Must be called with mutex_ held and a non-empty _queue.
	std::expected<size_t, Error> dequeueLocked(void *data, size_t maxLength) {
		size_t written = 0;

		while(written < maxLength) {
			size_t packetSize = sizeof(inotify_event) +
				(_queue.front().name.empty() ? 0 : _queue.front().name.size() + 1);

			if(written + packetSize > maxLength)
				break;

			Packet packet = std::move(_queue.front());
			_queue.pop_front();

			inotify_event e;
			memset(&e, 0, sizeof(inotify_event));
			e.wd = packet.descriptor;
			e.mask = packet.events;
			e.cookie = packet.cookie;
			if(!packet.name.empty()) {
				e.len = packet.name.size() + 1;
				memcpy(reinterpret_cast<char *>(data) + written + sizeof(inotify_event),
					packet.name.c_str(), packet.name.size() + 1);
			}

			memcpy(reinterpret_cast<char *>(data) + written, &e, sizeof(inotify_event));

			written += packetSize;

			if(_queue.empty())
				break;
		}

		if(written)
			return written;
		else
			return std::unexpected{Error::illegalArguments};
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
	async::cancellation_token cancellation) override {
		// TODO: Return Error::fileClosed as appropriate.

		uint64_t currentSeq;
		int edges = 0;
		while(true) {
			{
				std::lock_guard lock{mutex_};
				assert(sequence <= _currentSeq);
				currentSeq = _currentSeq;
				edges = 0;
				if(_inSeq > sequence)
					edges |= EPOLLIN;
			}

			if (edges & mask)
				break;

			auto result = co_await _statusBell.async_wait_if([&] {
				std::lock_guard lock{mutex_};
				return _inSeq <= sequence;
			}, cancellation);
			if (!result)
				break;
		}

		co_return PollWaitResult(currentSeq, edges & mask);
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		std::lock_guard lock{mutex_};
		int events = 0;
		if(!_queue.empty())
			events |= EPOLLIN;

		co_return PollStatusResult(_currentSeq, events);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	int addWatch(smarter::shared_ptr<FsNode> node, uint32_t mask) {
		if(mask & ~supportedFlags)
			std::println("posix: inotify mask {:#x} is partially ignored", mask);

		// Node references are dropped after releasing mutex_ since destroying a node notifies observers.
		std::vector<smarter::shared_ptr<FsNode>> nodes;
		std::lock_guard lock{mutex_};
		for(const auto &[desc, existing] : watches_) {
			auto &other = nodes.emplace_back(existing->node.lock());
			if(other.get() != node.get())
				continue;
			if(mask & IN_MASK_ADD)
				existing->mask |= mask;
			else
				existing->mask = mask;
			return desc;
		}

		auto descriptor = descriptorAllocator_.allocate();
		auto watch = std::make_shared<Watch>(weakFile(), descriptor, mask, node);
		watches_.insert({descriptor, watch});
		// Registered under mutex_ such that removeWatch() always finds the observer.
		// The lock order is mutex_ -> FsNode::_observersMutex.
		node->addObserver(std::move(watch));
		return descriptor;
	}

	bool removeWatch(int wd) {
		// Dropped after releasing mutex_.
		std::shared_ptr<Watch> watch;
		smarter::shared_ptr<FsNode> node;
		bool queued = false;
		{
			std::lock_guard lock{mutex_};
			auto it = watches_.find(wd);
			if(it == watches_.end())
				return false;
			watch = std::move(it->second);
			watches_.erase(it);

			node = watch->node.lock();
			if(node) {
				node->removeObserver(watch.get());
				queued = watch->queueLocked(this, IN_IGNORED, {}, 0, false);
			}
			// The descriptor can be reused once IN_IGNORED is queued; stale notifications see removed.
			watch->removed = true;
			descriptorAllocator_.free(wd);
		}

		if(queued)
			_statusBell.raise();
		return true;
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, OpenFile *self) {
			managarm::fs::GenericIoctlReply resp;

			switch(req.command()) {
				case FIONREAD: {
					resp.set_error(managarm::fs::Errors::SUCCESS);

					size_t size;
					{
						std::lock_guard lock{self->mutex_};
						size = std::ranges::fold_left(self->_queue, 0ULL, [](size_t total, const auto& packet) {
							return total + sizeof(inotify_event) + (packet.name.empty() ? 0 : packet.name.size() + 1);
						});
					}
					resp.set_fionread_count(size);
					break;
				}
				default: {
					std::cout << "Invalid ioctl for inotify" << std::endl;
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
					break;
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
	async::cancellation_event cancelServe_;

	// Taken before FsNode::_observersMutex; _statusBell is raised after releasing it.
	std::mutex mutex_;
	// Protected by mutex_.
	std::deque<Packet> _queue;

	// Protected by mutex_.
	id_allocator<int> descriptorAllocator_{1};
	// Protected by mutex_.
	std::unordered_map<int, std::shared_ptr<Watch>> watches_;

	async::recurring_event _statusBell;
	// Protected by mutex_.
	uint64_t _currentSeq = 1;
	// Protected by mutex_.
	uint64_t _inSeq = 0;

	std::atomic<bool> nonBlock_;
};

} // anonymous namespace

smarter::shared_ptr<File, FileHandle> createFile(bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

int addWatch(File *base, smarter::shared_ptr<FsNode> node, uint32_t mask) {
	auto file = static_cast<OpenFile *>(base);
	return file->addWatch(std::move(node), mask);
}

bool removeWatch(File *base, int wd) {
	auto file = static_cast<OpenFile *>(base);
	return file->removeWatch(wd);
}

} // namespace inotify
