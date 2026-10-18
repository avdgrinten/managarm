
#include <string.h>
#include <mutex>
#include <print>
#include <vector>

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
			// _awaitPoll() destructs the operation that contains this receiver.
			auto self = std::move(item);
			self->pollOutcome.emplace(std::move(outcome));
			_awaitPoll(std::move(self));
		}

		smarter::shared_ptr<Item> item;
	};

	struct Item {
		Item(smarter::shared_ptr<OpenFile> epoll, Process *process,
				smarter::shared_ptr<File> file, int mask, uint64_t cookie)
		: epoll{epoll}, state{stateAlive}, process{process->weak_from_this()},
				file{std::move(file)}, eventMask{mask}, cookie{cookie} { }

		smarter::shared_ptr<OpenFile> epoll;
		// Protected by the epoll's _mutex.
		State state;

		// Basic data of this item.
		// The thread that registered the item. Like on Linux, the item outlives it;
		// files are then polled with a null process.
		std::weak_ptr<Process> process;
		smarter::shared_ptr<File> file;
		// Protected by the epoll's _mutex.
		int eventMask;
		uint64_t cookie;

		// Cancels the current pollOperation. Each operation gets a fresh event, such that
		// cancelling it never races with resetting it for the next operation.
		// Protected by the epoll's _mutex; cancelled after releasing it.
		std::shared_ptr<async::cancellation_event> cancelPoll;

		// The following two fields are owned by whoever set statePolling.
		frg::manual_box<
			async::execution::operation_t<
					async::result<frg::expected<Error, PollWaitResult>>,
					Receiver
			>
		> pollOperation;

		std::optional<frg::expected<Error, PollWaitResult>> pollOutcome;

		smarter::borrowed_ptr<Item> self;

		// Protected by the epoll's _mutex.
		frg::default_list_hook<Item> hook_;
	};

	// Starts a poll operation on an item for which the caller set statePolling.
	// Returns true if the operation completed inline.
	// The process only has to outlive the pollWait() call: only the signalfd uses it, and it pins
	// it itself, such that waiting on other files does not keep exited threads alive.
	static bool _startPoll(smarter::shared_ptr<Item> item, uint64_t sequence, int mask,
			std::shared_ptr<Process> process, std::shared_ptr<async::cancellation_event> cancel) {
		item->pollOperation.construct_with([&] {
			return async::execution::connect(
				item->file->pollWait(process.get(), sequence, mask, *cancel),
				Receiver{item}
			);
		});
		return async::execution::start_inline(*item->pollOperation);
	}

	static void _awaitPoll(smarter::shared_ptr<Item> item) {
		auto self = item->epoll.get();

		// Poll should not return immediately; we loop in case it does.
		while(true) {
			// First, destruct the operation so that we can re-use it later.
			item->pollOperation.destruct();
			auto resultOrError = std::move(*item->pollOutcome);
			item->pollOutcome.reset();

			bool raise = false;
			bool rearm = false;
			uint64_t sequence;
			int mask;
			std::shared_ptr<Process> process;
			std::shared_ptr<async::cancellation_event> cancel;
			{
				std::lock_guard lock{self->_mutex};
				assert(item->state & statePolling);

				// Discard non-active and closed items.
				if(!(item->state & stateAlive)) {
					item->state &= ~statePolling;
					return;
				}

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

						item.policy().increment();
						self->_pendingQueue.push_back(item.get());
						self->_currentSeq++;
						raise = true;
					}
				}else{
					if(logEpoll)
						std::println("posix.epoll \e[1;34m{}\e[0m: Item \e[1;34m{}\e[0m still not pending after pollWait(). "
							"Mask is {:#x}, while edges are {:#x}",
							item->epoll->structName(), item->file->structName(), item->eventMask, std::get<1>(result));

					cancel = std::make_shared<async::cancellation_event>();
					item->cancelPoll = cancel;
					sequence = std::get<0>(result);
					mask = (item->eventMask & epollEvents) | EPOLLERR | EPOLLHUP;
					process = item->process.lock();
					rearm = true;
				}
			}

			if(raise)
				self->_statusBell.raise();
			if(!rearm)
				return;

			if(!_startPoll(item, sequence, mask, std::move(process), std::move(cancel)))
				return;
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

		// Dropped after releasing the mutex if the item already exists.
		auto item = smarter::make_shared<Item>(smarter::static_pointer_cast<OpenFile>(weakFile().lock()),
				process, std::move(file), mask, cookie);
		item->self = item;

		{
			std::lock_guard lock{_mutex};
			// TODO: Fix the memory-leak.
			if(_fileMap.find({item->file.get(), fd}) != _fileMap.end()) {
				return Error::alreadyExists;
			}

			item->state |= statePending | stateActive;

			_fileMap.insert({{item->file.get(), fd}, item});

			item.policy().increment();
			_pendingQueue.push_back(item.get());
			_currentSeq++;
		}
		_statusBell.raise();
		return Error::success;
	}

	Error modifyItem(File *file, int fd, int mask, uint64_t cookie) {
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Modifying item \e[1;34m{}\e[0m. New mask is {:#x}",
				structName(), file->structName(), mask);

		std::shared_ptr<async::cancellation_event> cancel;
		bool raise = false;
		{
			std::lock_guard lock{_mutex};
			auto it = _fileMap.find({file, fd});
			if(it == _fileMap.end()) {
				return Error::noSuchFile;
			}
			auto &item = it->second;

			if((item->eventMask & EPOLLEXCLUSIVE) || (mask & EPOLLEXCLUSIVE))
				return Error::illegalArguments;

			item->eventMask = mask;
			item->cookie = cookie;
			cancel = item->cancelPoll;

			// Mark the item as pending.
			if(!(item->state & statePending)) {
				item->state |= statePending | stateActive;

				item.policy().increment();
				_pendingQueue.push_back(item.get());
				_currentSeq++;
				raise = true;
			}
		}
		if(cancel)
			cancel->cancel();
		if(raise)
			_statusBell.raise();
		return Error::success;
	}

	Error deleteItem(File *file, int fd) {
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Deleting item \e[1;34m{}\e[0m",
				structName(), file->structName());

		// Dropped after releasing the mutex.
		smarter::shared_ptr<Item> item;
		std::shared_ptr<async::cancellation_event> cancel;
		{
			std::lock_guard lock{_mutex};
			auto it = _fileMap.find({file, fd});
			if(it == _fileMap.end()) {
				return Error::noSuchFile;
			}
			item = it->second;
			cancel = item->cancelPoll;

			_fileMap.erase(it);
			item->state &= ~stateAlive;
		}
		if(cancel)
			cancel->cancel();
		return Error::success;
	}

	async::result<size_t>
	waitForEvents(struct epoll_event *events, size_t max_events,
			async::cancellation_token cancellation) {
		assert(max_events);
		if(logEpoll)
			std::println("posix.epoll \e[1;34m{}\e[0m: Entering wait; cancellation is {}",
				structName(), cancellation.is_cancellation_requested() ? "active" : "inactive");

		size_t k = 0;
		// Only accessed by this waiter, but the hooks are protected by _mutex.
		frg::intrusive_list<
			Item,
			frg::locate_member<Item, frg::default_list_hook<Item>, &Item::hook_>
		> repoll_queue;
		while(true) {
			// TODO: Stop waiting in this case.
			assert(isOpen());

			while(true) {
				// Dropped after releasing the mutex.
				smarter::shared_ptr<Item> item;
				std::shared_ptr<Process> process;
				int itemEvents;
				{
					std::lock_guard lock{_mutex};
					if(_pendingQueue.empty())
						break;
					item = _pendingQueue.front()->self.lock();
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

					process = item->process.lock();
				}

				if(logEpoll)
					std::println("posix.epoll \e[1;34m{}\e[0m: Checking item \e[1;34m{}\e[0m",
						structName(), item->file->structName());
				auto result_or_error = co_await item->file->pollStatus(process.get());

				bool arm = false;
				uint64_t sequence;
				std::shared_ptr<async::cancellation_event> cancel;
				{
					std::lock_guard lock{_mutex};

					// Discard items that were deleted while we polled them.
					if(!(item->state & stateAlive)) {
						item->state &= ~statePending;
						continue;
					}

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

					// The mask may have changed while we polled the item.
					itemEvents = item->eventMask & epollEvents;
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
							// Once an item is not pending anymore, we continue watching it.
							item->state |= statePolling;
							cancel = std::make_shared<async::cancellation_event>();
							item->cancelPoll = cancel;
							sequence = std::get<0>(result);
							arm = true;
						}
					} else {
						item.policy().increment();
						repoll_queue.push_back(item.get());
					}
				}

				if(arm && _startPoll(item, sequence, itemEvents | EPOLLERR | EPOLLHUP,
						std::move(process), std::move(cancel)))
					_awaitPoll(item);

				if(k == max_events)
					break;
			}

			if(k)
				break;

			// Block and re-check if there are pending events.
			auto waitResult = co_await _statusBell.async_wait_if([&] {
				std::lock_guard lock{_mutex};
				return _pendingQueue.empty() && isOpen();
			}, cancellation);
			if(!waitResult)
				break;
		}

		// Before returning, we have to reinsert the level-triggered events that we report.
		if(!repoll_queue.empty()) {
			{
				std::lock_guard lock{_mutex};
				_pendingQueue.splice(_pendingQueue.end(), repoll_queue);
				// We have to increment the sequence again as concurrent waiters
				// might have seen an empty _pendingQueue.
				_currentSeq++;
			}
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
		// Dropped after releasing the mutex.
		std::vector<smarter::shared_ptr<Item>> items;
		std::vector<std::shared_ptr<async::cancellation_event>> cancels;
		{
			std::lock_guard lock{_mutex};
			for(auto &[key, item] : _fileMap) {
				assert(item->state & stateAlive);
				item->state &= ~stateAlive;

				if(item->state & statePolling)
					cancels.push_back(item->cancelPoll);
				items.push_back(item);
			}
			_fileMap.clear();

			while(!_pendingQueue.empty()) {
				auto item = _pendingQueue.front()->self.lock();
				_pendingQueue.pop_front();
				item.policy().decrement();
				assert(item->state & statePending);
				item->state &= ~statePending;
				items.push_back(std::move(item));
			}
		}

		for(auto &cancel : cancels)
			cancel->cancel();
		_statusBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override {
		uint64_t sequence;
		int edges = 0;
		while(true) {
			if(!isOpen())
				co_return Error::fileClosed;

			{
				std::lock_guard lock{_mutex};
				assert(past_seq <= _currentSeq);
				sequence = _currentSeq;
			}

			edges = 0;
			if (sequence > past_seq)
				edges |= EPOLLIN;

			if (edges & mask)
				break;

			auto waitResult = co_await _statusBell.async_wait_if([&] {
				std::lock_guard lock{_mutex};
				return _currentSeq == sequence && isOpen();
			}, cancellation);
			if(!waitResult)
				break;
		}

		co_return PollWaitResult{sequence, edges & mask};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		std::lock_guard lock{_mutex};
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

	// Leaf lock. The bell is raised and poll operations are started and cancelled after
	// releasing it, since both can re-enter _awaitPoll().
	std::mutex _mutex;

	// Protected by _mutex.
	std::unordered_map<Key, smarter::shared_ptr<Item>, KeyHash> _fileMap;

	// Protected by _mutex.
	frg::intrusive_list<
		Item,
		frg::locate_member<Item, frg::default_list_hook<Item>, &Item::hook_>
	> _pendingQueue;
	async::recurring_event _statusBell;
	// Protected by _mutex.
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

