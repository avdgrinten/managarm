
#include <string.h>
#include <print>

#include <async/recurring-event.hpp>
#include <frg/intrusive.hpp>
#include <frg/manual_box.hpp>
#include <helix/ipc.hpp>
#include "common.hpp"
#include "epoll.hpp"
#include "fs.hpp"
#include "process.hpp"

namespace {

constexpr int epollEvents = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLPRI | EPOLLERR | EPOLLHUP;
[[maybe_unused]]
constexpr int epollFlags = EPOLLET | EPOLLONESHOT | EPOLLWAKEUP | EPOLLEXCLUSIVE;

bool logEpoll = false;

struct OpenFile : FileWithDefaults {
	// ------------------------------------------------------------------------
	// Internal API.
	// ------------------------------------------------------------------------
private:
	// Lifetime management: There are the following three state bits for each item.
	// Items are deleted once all state bits are zero.
	// Items must only be accessed while a precondition guarantees that
	// at least one state bit is non-zero.
	using State = uint32_t;
	static constexpr State stateAlive = 1;
	static constexpr State statePolling = 2;
	static constexpr State statePending = 4;
	static constexpr State stateActive = 8;

	struct Item;

	struct Receiver {
		void set_value_inline(frg::expected<Error, PollWaitResult> outcome) {
			item->pollOutcome.emplace(std::move(outcome));
		}

		void set_value_noinline(frg::expected<Error, PollWaitResult> outcome) {
			item->pollOutcome.emplace(std::move(outcome));
			_awaitPoll(item.get());
		}

		smarter::shared_ptr<Item> item;
	};

	struct Item {
		Item(smarter::shared_ptr<OpenFile> epoll, Process *process,
				smarter::shared_ptr<File> file, int mask, uint64_t cookie)
		: epoll{epoll}, state{stateAlive}, process{process->weak_from_this()},
				file{std::move(file)}, eventMask{mask}, cookie{cookie} { }

		smarter::shared_ptr<OpenFile> epoll;
		State state;

		// Basic data of this item.
		// The thread that registered the item. Like on Linux, the item outlives it;
		// files are then polled with a null process.
		std::weak_ptr<Process> process;
		smarter::shared_ptr<File> file;
		int eventMask;
		uint64_t cookie;

		// Cancels the current pollOperation. Each operation gets a fresh event, such that
		// cancelling it never races with resetting it for the next operation.
		std::shared_ptr<async::cancellation_event> cancelPoll;

		frg::manual_box<
			async::execution::operation_t<
					async::result<frg::expected<Error, PollWaitResult>>,
					Receiver
			>
		> pollOperation;

		std::optional<frg::expected<Error, PollWaitResult>> pollOutcome;

		smarter::borrowed_ptr<Item> self;

		frg::default_list_hook<Item> hook_;
	};

	static void _awaitPoll(Item *item) {
	reRunImmediately:
		// First, destruct the operation so that we can re-use it later.
		item->pollOperation.destruct();

		assert(item->state & statePolling);
		auto self = item->epoll.get();

		// Discard non-active and closed items.
		if(!(item->state & stateAlive)) {
			item->state &= ~statePolling;
			return;
		}

		auto resultOrError = std::move(*item->pollOutcome);

		if(!resultOrError) {
			assert(resultOrError.error() == Error::fileClosed);
			item->state &= ~statePolling;
			return;
		}

		// Note that items only become pending if there is an edge.
		// This is the correct behavior for edge-triggered items.
		// Level-triggered items stay pending until the event disappears.
		auto result = resultOrError.value();
		if ((std::get<1>(result) & ~(item->eventMask | EPOLLERR | EPOLLHUP)) != 0)
			if(logEpoll)
				std::println("\e[1;31mposix.epoll {}: Item {} returned result {:#x} that is not contained in mask {:#x}\e[0m",
					item->epoll->structName(), item->file->structName(), std::get<1>(result), (item->eventMask | EPOLLERR | EPOLLHUP));

		if(std::get<1>(result) & (item->eventMask | EPOLLERR | EPOLLHUP)) {
			if(logEpoll)
				std::println("posix.epoll \e[1;34m{}\e[0m: Item \e[1;34m{}\e[0m becomes pending",
					item->epoll->structName(), item->file->structName());

			// Note that we stop watching once an item becomes pending.
			// We do this as we have to pollStatus() again anyway before we report the item.
			item->state &= ~statePolling;
			if(!(item->state & statePending)) {
				item->state |= statePending;

				item->self.lock().policy().increment();
				self->_pendingQueue.push_back(item);
				self->_currentSeq++;
				self->_statusBell.raise();
			}
		}else{
			// Here, we assume that the lambda does not execute on the current stack.
			// TODO: Use some callback queueing mechanism to ensure this.
			if(logEpoll)
				std::println("posix.epoll \e[1;34m{}\e[0m: Item \e[1;34m{}\e[0m still not pending after pollWait(). "
					"Mask is {:#x}, while edges are {:#x}",
					item->epoll->structName(), item->file->structName(), item->eventMask, std::get<1>(result));

			item->cancelPoll = std::make_shared<async::cancellation_event>();
			auto process = item->process.lock();
			item->pollOperation.construct_with([&] {
				return async::execution::connect(
					item->file->pollWait(process.get(), std::get<0>(result),
							(item->eventMask & epollEvents) | EPOLLERR | EPOLLHUP, *item->cancelPoll),
					Receiver{item->self.lock()}
				);
			});
			// Poll should not return immediately; we use an ugly goto here in favor of wrapping
			// the entire function in a loop.
			if(async::execution::start_inline(*item->pollOperation))
				goto reRunImmediately;
		}
	}

public:
	~OpenFile() override {
		// Nothing to do here.
	}

	Error addItem(Process *process, smarter::shared_ptr<File> file, int fd,
			int mask, uint64_t cookie) {
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Adding item \e[1;34m{}\e[0m. Mask is {:#x}",
				structName(), file->structName(), mask);

		// TODO: Fix the memory-leak.
		if(_fileMap.find({file.get(), fd}) != _fileMap.end()) {
			return Error::alreadyExists;
		}

		auto item = smarter::make_shared<Item>(smarter::static_pointer_cast<OpenFile>(weakFile().lock()),
				process, std::move(file), mask, cookie);
		item->self = item;

		item->state |= statePending | stateActive;

		_fileMap.insert({{item->file.get(), fd}, item});

		item.policy().increment();
		_pendingQueue.push_back(item.get());
		_currentSeq++;
		_statusBell.raise();
		return Error::success;
	}

	Error modifyItem(File *file, int fd, int mask, uint64_t cookie) {
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Modifying item \e[1;34m{}\e[0m. New mask is {:#x}",
				structName(), file->structName(), mask);

		auto it = _fileMap.find({file, fd});
		if(it == _fileMap.end()) {
			return Error::noSuchFile;
		}
		auto item = it->second;

		if((item->eventMask & EPOLLEXCLUSIVE) || (mask & EPOLLEXCLUSIVE))
			return Error::illegalArguments;

		item->eventMask = mask;
		item->cookie = cookie;
		if(item->cancelPoll)
			item->cancelPoll->cancel();

		// Mark the item as pending.
		if(!(item->state & statePending)) {
			item->state |= statePending | stateActive;

			item.policy().increment();
			_pendingQueue.push_back(item.get());
			_currentSeq++;
			_statusBell.raise();
		}
		return Error::success;
	}

	Error deleteItem(File *file, int fd) {
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Deleting item \e[1;34m{}\e[0m",
				structName(), file->structName());

		auto it = _fileMap.find({file, fd});
		if(it == _fileMap.end()) {
			return Error::noSuchFile;
		}
		auto item = it->second;

		if(item->cancelPoll)
			item->cancelPoll->cancel();

		_fileMap.erase(it);
		item->state &= ~stateAlive;
		return Error::success;
	}

	async::result<size_t>
	waitForEvents(struct epoll_event *events, size_t max_events,
			async::cancellation_token cancellation) {
		assert(max_events);
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Entering wait. There are {} pending items; cancellation is {}",
				structName(), (_pendingQueue.empty() ? "no " : ""), cancellation.is_cancellation_requested() ? "active" : "inactive");

		size_t k = 0;
		frg::intrusive_list<
			Item,
			frg::locate_member<Item, frg::default_list_hook<Item>, &Item::hook_>
		> repoll_queue;
		while(true) {
			// TODO: Stop waiting in this case.
			assert(isOpen());

			while(!_pendingQueue.empty()) {
				auto item = _pendingQueue.front()->self.lock();
				_pendingQueue.pop_front();
				item.policy().decrement();
				assert(item->state & statePending);

				// Discard dead or inactive items without returning them.
				if(!(item->state & stateAlive) || !(item->state & stateActive)) {
					if(logEpoll)
						std::println("posix.epoll \e[1;34m{}\e[0m: Discarding dead or inactive item \e[1;34m{}\e[0m",
							structName(), item->file->structName());
					item->state &= ~statePending;
					continue;
				}

				if(logEpoll)
					std::println("posix.epoll \e[1;34m{}\e[0m: Checking item \e[1;34m{}\e[0m",
						structName(), item->file->structName());
				auto process = item->process.lock();
				auto result_or_error = co_await item->file->pollStatus(process.get());

				// Discard items that were deleted while we polled them.
				if(!(item->state & stateAlive)) {
					item->state &= ~statePending;
					continue;
				}

				// The mask may have changed while we polled the item.
				auto itemEvents = item->eventMask & epollEvents;

				// Discard closed items.
				if(!result_or_error) {
					// We only expect fileClosed as an error here. For robustness: do not assert() but complain instead.
					// This catches cases such as files that do not support pollStatus().
					if(result_or_error.error() != Error::fileClosed) {
						std::println("posix.epoll \e[1;34m{}\e[0m: Unexpected error {} from pollStatus() on \e[1;34m{}\e[0m",
							structName(), std::to_underlying(result_or_error.error()), item->file->structName());
					}
					if(logEpoll)
						std::println("posix.epoll \e[1;34m{}\e[0m: Discarding closed item \e[1;34m{}\e[0m",
							structName(), item->file->structName());
					item->state &= ~statePending;
					continue;
				}

				auto result = result_or_error.value();
				if(logEpoll)
					std::println("posix.epoll \e[1;34m{}\e[0m: Item \e[1;34m{}\e[0m mask is {:#x}, while {:#x} is active",
						structName(), item->file->structName(), itemEvents, std::get<1>(result));

				// Return pending items to the caller.
				auto status = std::get<1>(result) & (itemEvents | EPOLLERR | EPOLLHUP);
				if(status) {
					if(item->eventMask & (EPOLLWAKEUP | EPOLLEXCLUSIVE))
						std::println("posix.epoll \e[1;34m{}\e[0m: unhandled epoll flag {:#x}",
							structName(), item->eventMask & (EPOLLWAKEUP | EPOLLEXCLUSIVE));

					assert(k < max_events);
					memset(events + k, 0, sizeof(struct epoll_event));
					events[k].events = status;
					events[k].data.u64 = item->cookie;
					k++;

					if(item->eventMask & EPOLLONESHOT)
						item->state &= ~stateActive;
				}

				if(!status || (item->eventMask & EPOLLET)) {
					item->state &= ~statePending;
					if(!(item->state & statePolling)) {
						item->state |= statePolling;

						// Once an item is not pending anymore, we continue watching it.
						item->cancelPoll = std::make_shared<async::cancellation_event>();
						item->pollOperation.construct_with([&] {
							return async::execution::connect(
								item->file->pollWait(process.get(), std::get<0>(result),
										itemEvents | EPOLLERR | EPOLLHUP, *item->cancelPoll),
								Receiver{item}
							);
						});
						if(async::execution::start_inline(*item->pollOperation))
							_awaitPoll(item.get());
					}
				} else {
					item.policy().increment();
					repoll_queue.push_back(item.get());
				}

				if(k == max_events)
					break;
			}

			if(k)
				break;

			// Block and re-check if there are pending events.
			if (!co_await _statusBell.async_wait(cancellation))
				break;
		}

		// Before returning, we have to reinsert the level-triggered events that we report.
		if(!repoll_queue.empty()) {
			_pendingQueue.splice(_pendingQueue.end(), repoll_queue);
			// We have to increment the sequence again as concurrent waiters
			// might have seen an empty _pendingQueue.
			_currentSeq++;
			_statusBell.raise();
		}

		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Return from wait with {} items",
				structName(), k);

		co_return k;
	}

	// ------------------------------------------------------------------------
	// File implementation.
	// ------------------------------------------------------------------------

	void handleClose() override {
		auto it = _fileMap.begin();
		while(it != _fileMap.end()) {
			auto item = it->second;
			assert(item->state & stateAlive);

			it = _fileMap.erase(it);
			item->state &= ~stateAlive;

			if(item->state & statePolling)
				item->cancelPoll->cancel();
		}

		while(!_pendingQueue.empty()) {
			auto item = _pendingQueue.front()->self.lock();
			_pendingQueue.pop_front();
			item.policy().decrement();
			assert(item->state & statePending);
			item->state &= ~statePending;
		}

		_statusBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override {
		assert(past_seq <= _currentSeq);

		int edges = 0;
		while(true) {
			if(!isOpen())
				co_return Error::fileClosed;

			edges = 0;
			if (_currentSeq > past_seq)
				edges |= EPOLLIN;

			if (edges & mask)
				break;

			if (!co_await _statusBell.async_wait(cancellation))
				break;
		}

		co_return PollWaitResult{_currentSeq, edges & mask};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{_currentSeq, _pendingQueue.empty() ? 0 : EPOLLIN};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile()
	: FileWithDefaults{FileKind::unknown,  StructName::get("epoll"), nullptr, SpecialLink::makeSpecialLink(VfsType::regular, 0777), File::defaultPipeLikeSeek}, _currentSeq{0} { }

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// Since Item stores a strong pointer to each File,
	// it is sufficient if Key stores a plain (= non-owning) pointer.
	using Key = std::pair<File *, int>;

	struct KeyHash {
		size_t operator() (const Key &k) const {
			return 13 * reinterpret_cast<uintptr_t>(k.first)
					+ static_cast<size_t>(k.second);
		}
	};

	std::unordered_map<Key, smarter::shared_ptr<Item>, KeyHash> _fileMap;

	frg::intrusive_list<
		Item,
		frg::locate_member<Item, frg::default_list_hook<Item>, &Item::hook_>
	> _pendingQueue;
	async::recurring_event _statusBell;
	uint64_t _currentSeq;
};

} // anonymous namespace

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

Error addItem(File *epfile, Process *process, smarter::shared_ptr<File> file, int fd,
		int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->addItem(process, std::move(file), fd, flags, cookie);
}

Error modifyItem(File *epfile, File *file, int fd, int flags, uint64_t cookie) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->modifyItem(file, fd, flags, cookie);
}

Error deleteItem(File *epfile, File *file, int fd, int flags) {
	assert(!flags);
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->deleteItem(file, fd);
}

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events, async::cancellation_token cancellation) {
	auto epoll = static_cast<OpenFile *>(epfile);
	return epoll->waitForEvents(events, max_events, cancellation);
}

} // namespace epoll

