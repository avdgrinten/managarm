#pragma once

#include <assert.h>
#include <tuple>
#include <array>
#include <vector>
#include <span>

#include <async/basic.hpp>
#include <async/oneshot-event.hpp>

// This is here since ipc-structs.hpp needs ElementHandle
namespace helix {

struct Dispatcher;

struct ElementHandle {
	friend void swap(ElementHandle &u, ElementHandle &v) {
		using std::swap;
		swap(u._dispatcher, v._dispatcher);
		swap(u._cn, v._cn);
		swap(u._data, v._data);
	}

	ElementHandle()
	: _dispatcher{nullptr}, _cn{-1}, _data{nullptr} { }

	explicit ElementHandle(Dispatcher *dispatcher, int cn, void *data)
	: _dispatcher{dispatcher}, _cn{cn}, _data{data} { }

	ElementHandle(const ElementHandle &other);

	ElementHandle(ElementHandle &&other)
	: ElementHandle{} {
		swap(*this, other);
	}

	~ElementHandle();

	ElementHandle &operator= (ElementHandle other) {
		swap(*this, other);
		return *this;
	}

	void *data() {
		return _data;
	}

private:
	Dispatcher *_dispatcher;
	int _cn;
	void *_data;
};

} // namespace helix

#include "ipc-structs.hpp"

namespace helix {

template<typename Tag>
struct UniqueResource : UniqueDescriptor {
	UniqueResource() = default;

	explicit UniqueResource(HelHandle handle)
	: UniqueDescriptor(handle) { }

	UniqueResource(UniqueDescriptor descriptor)
	: UniqueDescriptor(std::move(descriptor)) { }
};

template<typename Tag>
struct BorrowedResource : BorrowedDescriptor {
	BorrowedResource() = default;

	explicit BorrowedResource(HelHandle handle)
	: BorrowedDescriptor(handle) { }

	BorrowedResource(BorrowedDescriptor descriptor)
	: BorrowedDescriptor(descriptor) { }

	BorrowedResource(const UniqueResource<Tag> &other)
	: BorrowedDescriptor(other) { }

	UniqueResource<Tag> dup() const {
		HelHandle new_handle;
		HEL_CHECK(helTransferDescriptor(
			getHandle(),
			kHelThisUniverse,
			kHelTransferDescriptorOut,
			kHelRightsMax,
			kHelRightNull,
			&new_handle
		));
		return UniqueResource<Tag>(new_handle);
	}
};

struct Lane { };
using UniqueLane = UniqueResource<Lane>;
using BorrowedLane = BorrowedResource<Lane>;

inline std::pair<UniqueLane, UniqueLane> createStream(bool attach_credentials = false) {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateStream(&first_handle, &second_handle, attach_credentials));
	return { UniqueLane(first_handle), UniqueLane(second_handle) };
}

struct Irq { };
using UniqueIrq = UniqueResource<Irq>;
using BorrowedIrq = BorrowedResource<Irq>;

struct OperationBase {
	friend struct Dispatcher;

	OperationBase()
	: _asyncId(0) { }

	virtual ~OperationBase() { }

protected:
	int64_t _asyncId;
};

struct Operation : OperationBase {
	Operation()
	: _asyncId{0} { }

	uint64_t asyncId() {
		return _asyncId;
	}

	void setAsyncId(uint64_t async_id) {
		_asyncId = async_id;
	}

	virtual void parse(const void *) = 0;

private:
	uint64_t _asyncId;
};

struct Context {
	virtual ~Context() = default;

	virtual void complete(ElementHandle element) = 0;
};

struct CurrentDispatcherToken {
	void wait();
	async::run_queue *get_run_queue() const;
};

inline constexpr CurrentDispatcherToken currentDispatcher;

struct Dispatcher {
	friend struct ElementHandle;

private:
	struct Item {
		Item(HelQueue *queue)
		: queue(queue), progress(0) { }

		Item(const Item &other) = delete;

		Item &operator= (const Item &other) = delete;

		HelQueue *queue;

		size_t progress;
	};

	struct RunQueue final : async::run_queue {
		RunQueue(Dispatcher *dispatcher)
		: dispatcher_{dispatcher} { }

	private:
		void wakeup() override {
			HEL_CHECK(helAlertQueue(dispatcher_->_handle));
		}

		Dispatcher *dispatcher_;
	};

public:
	static Dispatcher &global();

	Dispatcher()
	: _handle{kHelNullHandle}, _queue{nullptr},
			_numCqChunks{8}, _numSqChunks{8}, _chunkSize{4096},
			_retrieveChunk{0}, _tailChunk{0}, _lastProgress{0},
			_sqCurrentChunk{0}, _sqProgress{0}, _runQueue{this} {
		HelQueueParameters params {
			.flags = 0,
			.numChunks = _numCqChunks,
			.chunkSize = _chunkSize,
			.numSqChunks = _numSqChunks,
		};
		HEL_CHECK(helCreateQueue(&params, &_handle));
		_nextAsyncId = 1;

		auto totalChunks = _numCqChunks + _numSqChunks;
		auto chunksOffset = (sizeof(HelQueue) + 63) & ~size_t(63);
		auto reservedPerChunk = (sizeof(HelChunk) + _chunkSize + 63) & ~size_t(63);
		auto overallSize = chunksOffset + totalChunks * reservedPerChunk;

		void *mapping;
		HEL_CHECK(helMapMemory(_handle, kHelNullHandle, nullptr,
				0, (overallSize + 0xFFF) & ~size_t(0xFFF),
				kHelMapProtRead | kHelMapProtWrite, &mapping));

		_queue = reinterpret_cast<HelQueue *>(mapping);
		auto chunksPtr = reinterpret_cast<std::byte *>(mapping) + chunksOffset;
		for(unsigned int i = 0; i < totalChunks; ++i)
			_chunks[i] = reinterpret_cast<HelChunk *>(chunksPtr + i * reservedPerChunk);

		// Reset all CQ chunks.
		for (unsigned int i = 0; i < _numCqChunks; ++i)
			_resetChunk(i);

		// Set up CQ: chunks 0 to numCqChunks-1.
		__atomic_store_n(&_queue->cqFirst, 0 | kHelNextPresent, __ATOMIC_RELEASE);

		// Supply the remaining CQ chunks.
		_tailChunk = 0;
		for (unsigned int i = 1; i < _numCqChunks; ++i)
			_supplyChunk(i);
		_retrieveChunk = 0;

		// SQ is initialized by the kernel. Read sqFirst to get the first SQ chunk.
		if (_numSqChunks > 0) {
			auto sqFirst = __atomic_load_n(&_queue->sqFirst, __ATOMIC_ACQUIRE);
			_sqCurrentChunk = sqFirst & ~kHelNextPresent;
			_sqProgress = 0;
		}

		_wakeHeadFutex();
	}

	Dispatcher(const Dispatcher &) = delete;

	Dispatcher &operator= (const Dispatcher &) = delete;

	async::run_queue *runQueue() {
		return &_runQueue;
	}

	HelHandle queueHandle() {
		return _handle;
	}

	uint64_t makeAsyncId() {
		return _nextAsyncId++;
	}

	void wait() {
		assert(_onOwnerThread());

		while(true) {
			if(_runQueue.check()) {
				_runQueue.run_iteration();
				return;
			}

			bool done;
			bool rqPending;
			_waitProgressFutex(&done, &rqPending);
			if(rqPending)
				continue;
			if(done) {
				auto cn = _retrieveChunk;
				auto next = __atomic_load_n(&_chunks[cn]->next, __ATOMIC_ACQUIRE);
				_surrender(cn);

				_lastProgress = 0;
				_retrieveChunk = next & ~kHelNextPresent;
				continue;
			}

			// Dequeue the next element.
			auto ptr = (char *)_chunks[_retrieveChunk] + sizeof(HelChunk) + _lastProgress;
			auto element = reinterpret_cast<HelElement *>(ptr);
			_lastProgress += sizeof(HelElement) + element->length;

			auto context = reinterpret_cast<Context *>(element->context);
			_refCounts[_retrieveChunk]++;
			context->complete(ElementHandle{this, _retrieveChunk,
					ptr + sizeof(HelElement)});
			return;
		}
	}

private:
	// Check if we are on the owning thread of this dispatcher.
	// submitSq(), wait() and related functions must only be driven if this returns true;
	// in particular, the dispatcher's SQ and CQ state is not thread-safe.
	bool _onOwnerThread() {
		return _runQueue.context() == async::current_run_queue_context();
	}

	void _surrender(int cn) {
		assert(_onOwnerThread());
		assert(_refCounts[cn] > 0);
		if(_refCounts[cn]-- > 1)
			return;
		_resetChunk(cn);
		_supplyChunk(cn);
	}

	void _resetChunk(int cn) {
		// Reset the chunk's state.
		_chunks[cn]->next = 0;
		_chunks[cn]->progressFutex = 0;

		// Internal bookkeeping.
		_refCounts[cn] = 1;
	}

	void _supplyChunk(int cn) {
		__atomic_store_n(&_chunks[_tailChunk]->next, cn | kHelNextPresent, __ATOMIC_RELEASE);
		_tailChunk = cn;
		_wakeHeadFutex();
	}

	void _reference(int cn) {
		assert(_onOwnerThread());
		_refCounts[cn]++;
	}

public:
	// Push an element to the SQ using a gather list.
	void pushSq(uint32_t opcode, uintptr_t context,
			std::span<const std::span<const std::byte>> segments) {
		assert(_onOwnerThread());

		size_t dataLength = 0;
		for (auto seg : segments)
			dataLength += seg.size();

		auto elementSize = sizeof(HelElement) + dataLength;

		// Check if we need to move to the next chunk.
		if (_sqProgress + elementSize > _chunkSize) {
			// Wait for next chunk to become available.
			int nextWord;
			while (true) {
				nextWord = __atomic_load_n(&_chunks[_sqCurrentChunk]->next, __ATOMIC_ACQUIRE);
				if (nextWord & kHelNextPresent)
					break;
				auto notify = __atomic_load_n(&_queue->userNotify, __ATOMIC_RELAXED);
				if (!(notify & kHelUserNotifySupplySqChunks)) {
					HEL_CHECK(helDriveQueue(_handle, 0, 0));
				} else {
					__atomic_fetch_and(&_queue->userNotify, ~kHelUserNotifySupplySqChunks, __ATOMIC_ACQUIRE);
				}
			}

			// Mark current chunk as done.
			__atomic_store_n(&_chunks[_sqCurrentChunk]->progressFutex,
					_sqProgress | kHelProgressDone, __ATOMIC_RELEASE);

			// Signal the kernel.
			// Note: We do not call helDriveQueue() here; instead this is done at the next wait().
			__atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySqProgress, __ATOMIC_RELEASE);

			_sqCurrentChunk = nextWord & ~kHelNextPresent;
			_sqProgress = 0;
		}

		auto ptr = reinterpret_cast<char *>(_chunks[_sqCurrentChunk]) + sizeof(HelChunk) + _sqProgress;
		auto element = reinterpret_cast<HelElement *>(ptr);
		element->length = dataLength;
		element->opcode = opcode;
		element->context = reinterpret_cast<void *>(context);

		// Copy each segment.
		size_t offset = 0;
		for (auto seg : segments) {
			memcpy(ptr + sizeof(HelElement) + offset, seg.data(), seg.size());
			offset += seg.size();
		}

		_sqProgress += elementSize;

		// Signal the kernel that new SQ elements are available.
		// Note: We do not call helDriveQueue() here; instead this is done at the next wait().
		__atomic_store_n(&_chunks[_sqCurrentChunk]->progressFutex, _sqProgress, __ATOMIC_RELEASE);
		__atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySqProgress, __ATOMIC_RELEASE);
	}

	inline void cancel(uint64_t cancellationTag) {
		HelSqCancel sqData{};
		sqData.cancellationTag = cancellationTag;
		std::array segments{std::as_bytes(std::span{&sqData, 1})};
		pushSq(kHelSubmitCancel, 0, segments);
	}

private:
	void _wakeHeadFutex() {
		auto futex = __atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySupplyCqChunks, __ATOMIC_RELEASE);
		if(!(futex & kHelKernelNotifySupplyCqChunks))
			HEL_CHECK(helDriveQueue(_handle, 0, 0));
	}

	void _waitProgressFutex(bool *done, bool *rqPending) {
		*done = false;
		*rqPending = false;

		// userNotify bits checked by this function (these MUST be checked in the loop below!).
		const auto relevantNotify = kHelUserNotifyCqProgress | kHelUserNotifyAlert;
		// userNotify bits ignored by this function.
		const auto maskedNotify = kHelUserNotifySupplySqChunks;

		// Relaxed is enough here: if a relevant bit in notify is set, we will always go through
		// the load-acquire on the fetch_and() code path and re-check a notification afterwards
		// before we conclude that there is truly nothing pending anymore.
		auto notify = __atomic_load_n(&_queue->userNotify, __ATOMIC_RELAXED);
		while(true) {
			// Note: notify is reloaded at the end of each iteration below.
			_pendingNotify |= notify;

			if (_pendingNotify & kHelUserNotifyCqProgress) {
				auto progress = __atomic_load_n(&_chunks[_retrieveChunk]->progressFutex, __ATOMIC_ACQUIRE);
				assert(!(progress & ~(kHelProgressMask | kHelProgressFull | kHelProgressDone)));
				if (progress & kHelProgressFull)
					assert(_retrieveChunk != _tailChunk);
				if(_lastProgress != (progress & kHelProgressMask)) {
					*done = false;
					return;
				}else if(progress & kHelProgressDone) {
					assert(progress & kHelProgressFull);
					*done = true;
					return;
				}
			}

			// If we get here, no relevant notifications are pending.
			// Clear all relevant bits or wait in the kernel if all of them are already clear.
			auto notifyToClear = notify & relevantNotify;
			_pendingNotify &= ~relevantNotify;
			if (!notifyToClear) {
				// The only remaining bits must be masked ones (otherwise we are missing checks above).
				assert(!(_pendingNotify & ~maskedNotify));

				if (_runQueue.check()) {
					*rqPending = true;
					return;
				}

				auto e = helDriveQueue(_handle, kHelDriveWait, maskedNotify);
				if (e != kHelErrCancelled)
					HEL_CHECK(e);
				// Relaxed is enough (same reasoning as for the initial load).
				notify = __atomic_load_n(&_queue->userNotify, __ATOMIC_RELAXED);
			} else {
				// Note that we will check all cleared notifications again in the next iteration.
				// This RMW happens before the next progressFutex wait due to the load-acquire here.
				notify = __atomic_fetch_and(&_queue->userNotify, ~notifyToClear, __ATOMIC_ACQUIRE);
			}
		}
	}

private:
	HelHandle _handle;
	HelQueue *_queue;
	HelChunk *_chunks[16];

	// Queue parameters.
	unsigned int _numCqChunks;
	unsigned int _numSqChunks;
	size_t _chunkSize;

	uint64_t _nextAsyncId{0};

	// General state.
	int _pendingNotify{0};

	// CQ state.
	// Chunk that we are currently retrieving from.
	int _retrieveChunk;
	// Tail of the CQ chunk list (where we append new chunks).
	int _tailChunk;
	// Progress into the current CQ chunk.
	int _lastProgress;
	// Per-chunk reference counts.
	int _refCounts[16];

	// SQ state.
	// Chunk that we are currently writing to.
	int _sqCurrentChunk;
	// Progress into the current SQ chunk.
	int _sqProgress;

	RunQueue _runQueue;
};

inline void CurrentDispatcherToken::wait() {
	Dispatcher::global().wait();
}

inline async::run_queue *CurrentDispatcherToken::get_run_queue() const {
	return Dispatcher::global().runQueue();
}

inline ElementHandle::~ElementHandle() {
	if(_dispatcher)
		_dispatcher->_surrender(_cn);
}

inline ElementHandle::ElementHandle(const ElementHandle &other) {
	_dispatcher = other._dispatcher;
	_cn = other._cn;
	_data = other._data;

	_dispatcher->_reference(_cn);
}

struct AwaitClock : Operation {
	HelError error() {
		return result_.error;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelSimpleResult result_;
};

struct ProtectMemory : Operation {
	HelError error() {
		return result_.error;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelSimpleResult result_;
};

struct ManageMemory : Operation {
	HelError error() {
		return result_.error;
	}

	int type() {
		return result_.type;
	}

	uintptr_t offset() {
		return result_.offset;
	}

	size_t length() {
		return result_.length;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelManageResult result_;
};

struct LockMemoryView : Operation {
	HelError error() {
		return result_.error;
	}

	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));

		if(!error())
			_descriptor = UniqueDescriptor{result_.handle};
	}

	HelHandleResult result_;

	UniqueDescriptor _descriptor;
};

struct Observe : Operation {
	HelError error() {
		return result_.error;
	}

	unsigned int observation() {
		return result_.observation;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelObserveResult result_;
};

// ----------------------------------------------------------------------------
// Experimental: submitAsync
// ----------------------------------------------------------------------------

struct Submission : private Context {
	Submission(AwaitClock *operation,
			uint64_t counter, Dispatcher &dispatcher)
	: _result(operation) {
		auto asyncId = dispatcher.makeAsyncId();

		HelSqAwaitClock sqData;
		sqData.counter = counter;
		sqData.cancellationTag = asyncId;
		std::array segments{std::as_bytes(std::span{&sqData, 1})};
		dispatcher.pushSq(kHelSubmitAwaitClock,
				reinterpret_cast<uintptr_t>(context()), segments);

		operation->setAsyncId(asyncId);
	}

	Submission(BorrowedDescriptor space, ProtectMemory *operation,
			void *pointer, size_t length, uint32_t flags,
			Dispatcher &dispatcher)
	: _result(operation) {
		HelSqProtectMemory header;
		header.spaceHandle = space.getHandle();
		header.pointer = pointer;
		header.size = length;
		header.flags = flags;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitProtectMemory,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor memory, ManageMemory *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HelSqManageMemory header;
		header.handle = memory.getHandle();

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitManageMemory,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor memory, LockMemoryView *operation,
			uintptr_t offset, size_t size, Dispatcher &dispatcher)
	: _result(operation) {
		HelSqLockMemoryView header;
		header.handle = memory.getHandle();
		header.offset = offset;
		header.size = size;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitLockMemoryView,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor thread, Observe *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HelSqObserve header;
		header.handle = thread.getHandle();

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitObserve,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(const Submission &) = delete;

	Submission &operator= (Submission &other) = delete;

	auto async_wait() {
		return _ev.wait();
	}

private:
	Context *context() {
		return this;
	}

	void complete(ElementHandle element) override {
		auto ptr = element.data();
		_result->parse(ptr);
		_ev.raise();
	}

	Operation *_result;
	async::oneshot_primitive _ev;
};

inline Submission submitAwaitClock(AwaitClock *operation, uint64_t counter,
		Dispatcher &dispatcher) {
	return {operation, counter, dispatcher};
}

inline Submission submitProtectMemory(BorrowedDescriptor memory, ProtectMemory *operation,
		void *pointer, size_t length, uint32_t flags,
		Dispatcher &dispatcher) {
	return {memory, operation, pointer, length, flags, dispatcher};
}

inline Submission submitManageMemory(BorrowedDescriptor memory, ManageMemory *operation,
		Dispatcher &dispatcher) {
	return {memory, operation, dispatcher};
}

inline Submission submitLockMemoryView(BorrowedDescriptor memory, LockMemoryView *operation,
		uintptr_t offset, size_t size, Dispatcher &dispatcher) {
	return {memory, operation, offset, size, dispatcher};
}

inline Submission submitObserve(BorrowedDescriptor thread, Observe *operation,
		Dispatcher &dispatcher) {
	return {thread, operation, dispatcher};
}

} // namespace helix

namespace helix_ng {

using namespace helix;

// --------------------------------------------------------------------
// ExchangeMsgsSender
// --------------------------------------------------------------------

template <typename Results, typename Actions, typename Receiver>
struct ExchangeMsgsOperation : private Context {
	ExchangeMsgsOperation(BorrowedDescriptor lane, Actions actions, Receiver receiver)
	: lane_{std::move(lane)}, actions_{std::move(actions)}, receiver_{std::move(receiver)} { }

	void start() {
		auto helActions = frg::apply(chainActionArrays, actions_);

		HelSqExchangeMsgs header;
		header.lane = lane_.getHandle();
		header.count = helActions.size();
		header.flags = 0;

		std::array segments{
			std::as_bytes(std::span{&header, 1}),
			std::as_bytes(std::span{helActions.data(), helActions.size()})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitExchangeMsgs,
				reinterpret_cast<uintptr_t>(context), segments);
	}

private:
	void complete(ElementHandle element) override {
		Results results;
		void *ptr = element.data();

		[&]<size_t ...p>(std::index_sequence<p...>) {
			(results.template get<p>().parse(ptr, element), ...);
		} (std::make_index_sequence<std::tuple_size<Results>::value>{});

		async::execution::set_value(receiver_, std::move(results));
	}

	BorrowedDescriptor lane_;
	Actions actions_;
	Receiver receiver_;
};

template <typename Results, typename Actions>
struct [[nodiscard]] ExchangeMsgsSender {
	using value_type = Results;

	ExchangeMsgsSender(BorrowedDescriptor lane, Results, Actions actions)
	: lane_{std::move(lane)}, actions_{std::move(actions)} { }

	template<typename Receiver>
	ExchangeMsgsOperation<Results, Actions, Receiver> connect(Receiver receiver) {
		return {std::move(lane_), std::move(actions_), std::move(receiver)};
	}

private:
	BorrowedDescriptor lane_;
	Actions actions_;
};

template <typename Results, typename Actions>
async::sender_awaiter<ExchangeMsgsSender<Results, Actions>, Results>
operator co_await (ExchangeMsgsSender<Results, Actions> sender) {
	return {std::move(sender)};
}

template <typename ...Args>
auto exchangeMsgs(BorrowedDescriptor descriptor, Args &&...args) {
	return ExchangeMsgsSender{
		std::move(descriptor),
		createResultsTuple(args...),
		frg::tuple{std::forward<Args>(args)...}
	};
}

// --------------------------------------------------------------------
// Operations other than exchangeMsgs().
// --------------------------------------------------------------------

template <typename Receiver>
struct AsyncNopOperation : private Context {
	AsyncNopOperation(Receiver receiver)
	: receiver_{std::move(receiver)} { }

	void start() {
		auto context = static_cast<Context *>(this);

		std::span<const std::span<const std::byte>> segments;
		Dispatcher::global().pushSq(kHelSubmitAsyncNop,
				reinterpret_cast<uintptr_t>(context), segments);
	}

private:
	void complete(ElementHandle element) override {
		AsyncNopResult result;
		void *ptr = element.data();

		result.parse(ptr, element);

		async::execution::set_value(receiver_, std::move(result));
	}

	Receiver receiver_;
};

struct [[nodiscard]] AsyncNopSender {
	using value_type = AsyncNopResult;

	AsyncNopSender() = default;

	template<typename Receiver>
	AsyncNopOperation<Receiver> connect(Receiver receiver) {
		return {std::move(receiver)};
	}
};

inline async::sender_awaiter<AsyncNopSender, AsyncNopResult>
operator co_await (AsyncNopSender sender) {
	return {std::move(sender)};
}

inline auto asyncNop() {
	return AsyncNopSender{};
}

// --------------------------------------------------------------------

struct SynchronizeSpaceResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct SynchronizeSpaceOperation : private Context {
	SynchronizeSpaceOperation(BorrowedDescriptor space,
			void *pointer, size_t size, Receiver r)
	: space_{std::move(space)}, pointer_{pointer}, size_{size}, r_{std::move(r)} {}

	void start() {
		HelSqSynchronizeSpace header;
		header.spaceHandle = space_.getHandle();
		header.pointer = pointer_;
		header.size = size_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitSynchronizeSpace,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	SynchronizeSpaceOperation(const SynchronizeSpaceOperation &) = delete;
	SynchronizeSpaceOperation &operator= (const SynchronizeSpaceOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
	Receiver r_;
};

struct [[nodiscard]] SynchronizeSpaceSender {
	using value_type = SynchronizeSpaceResult;

	SynchronizeSpaceSender(BorrowedDescriptor space, void *pointer, size_t size)
	: space_{std::move(space)}, pointer_{pointer}, size_{size} { }

	template<typename Receiver>
	SynchronizeSpaceOperation<Receiver> connect(Receiver receiver) {
		return {std::move(space_), pointer_, size_, std::move(receiver)};
	}

private:
	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
};

inline async::sender_awaiter<SynchronizeSpaceSender, SynchronizeSpaceResult>
operator co_await (SynchronizeSpaceSender sender) {
	return {std::move(sender)};
}

inline auto synchronizeSpace(BorrowedDescriptor space, void *pointer, size_t size) {
	return SynchronizeSpaceSender{std::move(space), pointer, size};
}

// --------------------------------------------------------------------
// Read/WriteMemory
// --------------------------------------------------------------------

template <typename Receiver>
struct ReadMemoryOperation : private Context {
	ReadMemoryOperation(BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, void *buffer, Receiver r)
	: descriptor_{std::move(descriptor)}, address_{address}, length_{length},
		buffer_{buffer}, r_{std::move(r)} {}

	void start() {
		HelSqReadMemory header;
		header.handle = descriptor_.getHandle();
		header.address = address_;
		header.length = length_;
		header.buffer = buffer_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitReadMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ReadMemoryOperation(const ReadMemoryOperation &) = delete;
	ReadMemoryOperation &operator= (const ReadMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	void *buffer_;
	Receiver r_;
};

struct [[nodiscard]] ReadMemorySender {
	using value_type = SynchronizeSpaceResult;

	ReadMemorySender(BorrowedDescriptor descriptor, uintptr_t address,
			size_t length, void *buffer)
	: descriptor_{std::move(descriptor)}, address_{address},
		length_{length}, buffer_{buffer} { }

	template<typename Receiver>
	ReadMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(descriptor_), address_, length_, buffer_, std::move(receiver)};
	}

private:
	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	void *buffer_;
};

inline async::sender_awaiter<ReadMemorySender, SynchronizeSpaceResult>
operator co_await (ReadMemorySender sender) {
	return {std::move(sender)};
}

inline auto readMemory(BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, void *buffer) {
	return ReadMemorySender{descriptor, address, length, buffer};
}

template <typename Receiver>
struct WriteMemoryOperation : private Context {
	WriteMemoryOperation(BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, const void *buffer, Receiver r)
	: descriptor_{std::move(descriptor)}, address_{address}, length_{length},
		buffer_{buffer}, r_{std::move(r)} { }

	void start() {
		HelSqWriteMemory header;
		header.handle = descriptor_.getHandle();
		header.address = address_;
		header.length = length_;
		header.buffer = buffer_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitWriteMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	WriteMemoryOperation(const WriteMemoryOperation &) = delete;
	WriteMemoryOperation &operator= (const WriteMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	const void *buffer_;
	Receiver r_;
};

struct [[nodiscard]] WriteMemorySender {
	using value_type = SynchronizeSpaceResult;

	WriteMemorySender(BorrowedDescriptor descriptor, uintptr_t address,
			size_t length, const void *buffer)
	: descriptor_{std::move(descriptor)}, address_{address},
		length_{length}, buffer_{buffer} { }

	template<typename Receiver>
	WriteMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(descriptor_), address_, length_, buffer_, std::move(receiver)};
	}

private:
	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	const void *buffer_;
};

inline async::sender_awaiter<WriteMemorySender, SynchronizeSpaceResult>
operator co_await (WriteMemorySender sender) {
	return {std::move(sender)};
}

inline auto writeMemory(BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, const void *buffer) {
	return WriteMemorySender{descriptor, address, length, buffer};
}

// --------------------------------------------------------------------
// AwaitEvent
// --------------------------------------------------------------------

template <typename Receiver>
struct AwaitEventOperation : private Context {
	AwaitEventOperation(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct, Receiver receiver)
	: event_{std::move(event)}, sequence_{sequence}, ct_{ct}, receiver_{std::move(receiver)} { }

	void start() {
		asyncId_ = Dispatcher::global().makeAsyncId();

		HelSqAwaitEvent header;
		header.handle = event_.getHandle();
		header.sequence = sequence_;
		header.cancellationTag = asyncId_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitAwaitEvent,
				reinterpret_cast<uintptr_t>(context), segments);

		cb_.emplace(ct_, this);
	}

private:
	void complete(ElementHandle element) override {
		cb_ = std::nullopt;

		AwaitEventResult result;
		void *ptr = element.data();

		result.parse(ptr, element);

		async::execution::set_value(receiver_, std::move(result));
	}

	void cancel() {
		Dispatcher::global().cancel(asyncId_);
	}

	BorrowedDescriptor event_;
	uint64_t sequence_;
	async::cancellation_token ct_;
	std::optional<async::cancellation_callback<frg::bound_mem_fn<&AwaitEventOperation::cancel>>> cb_ = std::nullopt;
	uint64_t asyncId_;
	Receiver receiver_;
};

struct [[nodiscard]] AwaitEventSender {
	using value_type = AwaitEventResult;

	AwaitEventSender(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct)
	: event_{std::move(event)}, sequence_{sequence}, ct_{ct} { }

	template<typename Receiver>
	AwaitEventOperation<Receiver> connect(Receiver receiver) {
		return {std::move(event_), sequence_, ct_, std::move(receiver)};
	}

private:
	BorrowedDescriptor event_;
	uint64_t sequence_;
	async::cancellation_token ct_;
};

inline async::sender_awaiter<AwaitEventSender, AwaitEventResult>
operator co_await (AwaitEventSender sender) {
	return {std::move(sender)};
}

inline auto awaitEvent(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct = {}) {
	return AwaitEventSender{
		std::move(event),
		sequence,
		ct
	};
}

// --------------------------------------------------------------------
// ResizeMemory
// --------------------------------------------------------------------

struct ResizeMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct ResizeMemoryOperation : private Context {
	ResizeMemoryOperation(BorrowedDescriptor memory, size_t newSize, Receiver r)
	: memory_{std::move(memory)}, newSize_{newSize}, r_{std::move(r)} {}

	void start() {
		HelSqResizeMemory header;
		header.handle = memory_.getHandle();
		header.newSize = newSize_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitResizeMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ResizeMemoryOperation(const ResizeMemoryOperation &) = delete;
	ResizeMemoryOperation &operator= (const ResizeMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		ResizeMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	size_t newSize_;
	Receiver r_;
};

struct [[nodiscard]] ResizeMemorySender {
	using value_type = ResizeMemoryResult;

	ResizeMemorySender(BorrowedDescriptor memory, size_t newSize)
	: memory_{std::move(memory)}, newSize_{newSize} { }

	template<typename Receiver>
	ResizeMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), newSize_, std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
	size_t newSize_;
};

inline async::sender_awaiter<ResizeMemorySender, ResizeMemoryResult>
operator co_await (ResizeMemorySender sender) {
	return {std::move(sender)};
}

inline auto resizeMemory(BorrowedDescriptor memory, size_t newSize) {
	return ResizeMemorySender{std::move(memory), newSize};
}

// --------------------------------------------------------------------
// ForkMemory
// --------------------------------------------------------------------

struct ForkMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	UniqueDescriptor descriptor() {
		assert(valid_);
		HEL_CHECK(error());
		return std::move(descriptor_);
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		error_ = result->error;
		if(!error_)
			descriptor_ = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
	UniqueDescriptor descriptor_;
};

template <typename Receiver>
struct ForkMemoryOperation : private Context {
	ForkMemoryOperation(BorrowedDescriptor hierarchy, BorrowedDescriptor memory, Receiver r)
	: hierarchy_{std::move(hierarchy)}, memory_{std::move(memory)}, r_{std::move(r)} {}

	void start() {
		HelSqForkMemory header;
		header.hierarchyHandle = hierarchy_.getHandle();
		header.handle = memory_.getHandle();

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitForkMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ForkMemoryOperation(const ForkMemoryOperation &) = delete;
	ForkMemoryOperation &operator= (const ForkMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		ForkMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor hierarchy_;
	BorrowedDescriptor memory_;
	Receiver r_;
};

struct [[nodiscard]] ForkMemorySender {
	using value_type = ForkMemoryResult;

	ForkMemorySender(BorrowedDescriptor hierarchy, BorrowedDescriptor memory)
	: hierarchy_{std::move(hierarchy)}, memory_{std::move(memory)} { }

	template<typename Receiver>
	ForkMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(hierarchy_), std::move(memory_), std::move(receiver)};
	}

private:
	BorrowedDescriptor hierarchy_;
	BorrowedDescriptor memory_;
};

inline async::sender_awaiter<ForkMemorySender, ForkMemoryResult>
operator co_await (ForkMemorySender sender) {
	return {std::move(sender)};
}

inline auto forkMemory(BorrowedDescriptor hierarchy, BorrowedDescriptor memory) {
	return ForkMemorySender{std::move(hierarchy), std::move(memory)};
}

// --------------------------------------------------------------------
// WritebackFence
// --------------------------------------------------------------------

struct WritebackFenceResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct WritebackFenceOperation : private Context {
	WritebackFenceOperation(BorrowedDescriptor memory, uintptr_t offset, size_t size, Receiver r)
	: memory_{std::move(memory)}, offset_{offset}, size_{size}, r_{std::move(r)} {}

	void start() {
		HelSqWritebackFence header;
		header.handle = memory_.getHandle();
		header.offset = offset_;
		header.size = size_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitWritebackFence,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	WritebackFenceOperation(const WritebackFenceOperation &) = delete;
	WritebackFenceOperation &operator= (const WritebackFenceOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		WritebackFenceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	uintptr_t offset_;
	size_t size_;
	Receiver r_;
};

struct [[nodiscard]] WritebackFenceSender {
	using value_type = WritebackFenceResult;

	WritebackFenceSender(BorrowedDescriptor memory, uintptr_t offset, size_t size)
	: memory_{std::move(memory)}, offset_{offset}, size_{size} { }

	template<typename Receiver>
	WritebackFenceOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), offset_, size_, std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
	uintptr_t offset_;
	size_t size_;
};

inline async::sender_awaiter<WritebackFenceSender, WritebackFenceResult>
operator co_await (WritebackFenceSender sender) {
	return {std::move(sender)};
}

inline auto writebackFence(BorrowedDescriptor memory, uintptr_t offset, size_t size) {
	return WritebackFenceSender{std::move(memory), offset, size};
}

// --------------------------------------------------------------------
// InvalidateMemory
// --------------------------------------------------------------------

struct InvalidateMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct InvalidateMemoryOperation : private Context {
	InvalidateMemoryOperation(BorrowedDescriptor memory, uintptr_t offset, size_t size,
			uint32_t flags, Receiver r)
	: memory_{std::move(memory)}, offset_{offset}, size_{size}, flags_{flags},
			r_{std::move(r)} {}

	void start() {
		HelSqInvalidateMemory header;
		header.handle = memory_.getHandle();
		header.offset = offset_;
		header.size = size_;
		header.flags = flags_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitInvalidateMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	InvalidateMemoryOperation(const InvalidateMemoryOperation &) = delete;
	InvalidateMemoryOperation &operator= (const InvalidateMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		InvalidateMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	uintptr_t offset_;
	size_t size_;
	uint32_t flags_;
	Receiver r_;
};

struct [[nodiscard]] InvalidateMemorySender {
	using value_type = InvalidateMemoryResult;

	InvalidateMemorySender(BorrowedDescriptor memory, uintptr_t offset, size_t size,
			uint32_t flags)
	: memory_{std::move(memory)}, offset_{offset}, size_{size}, flags_{flags} { }

	template<typename Receiver>
	InvalidateMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), offset_, size_, flags_, std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
	uintptr_t offset_;
	size_t size_;
	uint32_t flags_;
};

inline async::sender_awaiter<InvalidateMemorySender, InvalidateMemoryResult>
operator co_await (InvalidateMemorySender sender) {
	return {std::move(sender)};
}

inline auto invalidateMemory(BorrowedDescriptor memory, uintptr_t offset, size_t size,
		uint32_t flags = 0) {
	return InvalidateMemorySender{std::move(memory), offset, size, flags};
}

// --------------------------------------------------------------------
// AlterMemoryIndirection
// --------------------------------------------------------------------

struct AlterMemoryIndirectionResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct AlterMemoryIndirectionOperation : private Context {
	AlterMemoryIndirectionOperation(BorrowedDescriptor indirect, uintptr_t offset,
			BorrowedDescriptor memory, uintptr_t memoryOffset, size_t size, Receiver r)
	: indirect_{std::move(indirect)}, offset_{offset}, memory_{std::move(memory)},
			memoryOffset_{memoryOffset}, size_{size}, r_{std::move(r)} {}

	void start() {
		HelSqAlterMemoryIndirection header;
		header.indirectHandle = indirect_.getHandle();
		header.memoryHandle = memory_.getHandle();
		header.offset = offset_;
		header.memoryOffset = memoryOffset_;
		header.size = size_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitAlterMemoryIndirection,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	AlterMemoryIndirectionOperation(const AlterMemoryIndirectionOperation &) = delete;
	AlterMemoryIndirectionOperation &operator= (const AlterMemoryIndirectionOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		AlterMemoryIndirectionResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor indirect_;
	uintptr_t offset_;
	BorrowedDescriptor memory_;
	uintptr_t memoryOffset_;
	size_t size_;
	Receiver r_;
};

struct [[nodiscard]] AlterMemoryIndirectionSender {
	using value_type = AlterMemoryIndirectionResult;

	AlterMemoryIndirectionSender(BorrowedDescriptor indirect, uintptr_t offset,
			BorrowedDescriptor memory, uintptr_t memoryOffset, size_t size)
	: indirect_{std::move(indirect)}, offset_{offset}, memory_{std::move(memory)},
			memoryOffset_{memoryOffset}, size_{size} { }

	template<typename Receiver>
	AlterMemoryIndirectionOperation<Receiver> connect(Receiver receiver) {
		return {std::move(indirect_), offset_, std::move(memory_), memoryOffset_, size_,
				std::move(receiver)};
	}

private:
	BorrowedDescriptor indirect_;
	uintptr_t offset_;
	BorrowedDescriptor memory_;
	uintptr_t memoryOffset_;
	size_t size_;
};

inline async::sender_awaiter<AlterMemoryIndirectionSender, AlterMemoryIndirectionResult>
operator co_await (AlterMemoryIndirectionSender sender) {
	return {std::move(sender)};
}

inline auto installMemoryIndirection(BorrowedDescriptor indirect, uintptr_t offset,
		BorrowedDescriptor memory, uintptr_t memoryOffset, size_t size) {
	return AlterMemoryIndirectionSender{std::move(indirect), offset,
			std::move(memory), memoryOffset, size};
}

inline auto removeMemoryIndirection(BorrowedDescriptor indirect, uintptr_t offset, size_t size) {
	return AlterMemoryIndirectionSender{std::move(indirect), offset,
			BorrowedDescriptor{kHelNullHandle}, 0, size};
}

// --------------------------------------------------------------------
// PopulateSpace
// --------------------------------------------------------------------

struct PopulateSpaceResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct PopulateSpaceOperation : private Context {
	PopulateSpaceOperation(BorrowedDescriptor space, uintptr_t address, size_t length, Receiver r)
	: space_{std::move(space)}, address_{address}, length_{length}, r_{std::move(r)} {}

	void start() {
		HelSqPopulateSpace header;
		header.handle = space_.getHandle();
		header.address = address_;
		header.length = length_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitPopulateSpace,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	PopulateSpaceOperation(const PopulateSpaceOperation &) = delete;
	PopulateSpaceOperation &operator= (const PopulateSpaceOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		PopulateSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor space_;
	uintptr_t address_;
	size_t length_;
	Receiver r_;
};

struct [[nodiscard]] PopulateSpaceSender {
	using value_type = PopulateSpaceResult;

	PopulateSpaceSender(BorrowedDescriptor space, uintptr_t address, size_t length)
	: space_{std::move(space)}, address_{address}, length_{length} { }

	template<typename Receiver>
	PopulateSpaceOperation<Receiver> connect(Receiver receiver) {
		return {std::move(space_), address_, length_, std::move(receiver)};
	}

private:
	BorrowedDescriptor space_;
	uintptr_t address_;
	size_t length_;
};

inline async::sender_awaiter<PopulateSpaceSender, PopulateSpaceResult>
operator co_await (PopulateSpaceSender sender) {
	return {std::move(sender)};
}

inline auto populateSpace(BorrowedDescriptor space, uintptr_t address, size_t length) {
	return PopulateSpaceSender{std::move(space), address, length};
}

// --------------------------------------------------------------------
// MapMemory
// --------------------------------------------------------------------

struct MapMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void *pointer() {
		assert(valid_);
		HEL_CHECK(error_);
		return pointer_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelPointerResult *>(ptr);
		error_ = result->error;
		pointer_ = result->pointer;
		ptr = (char *)ptr + sizeof(HelPointerResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
	void *pointer_;
};

template <typename Receiver>
struct MapMemoryOperation : private Context {
	MapMemoryOperation(BorrowedDescriptor memory, BorrowedDescriptor space, void *pointer,
			uintptr_t offset, size_t size, uint32_t flags, Receiver r)
	: memory_{std::move(memory)}, space_{std::move(space)}, pointer_{pointer},
			offset_{offset}, size_{size}, flags_{flags}, r_{std::move(r)} {}

	void start() {
		HelSqMapMemory header;
		header.memoryHandle = memory_.getHandle();
		header.spaceHandle = space_.getHandle();
		header.pointer = pointer_;
		header.offset = offset_;
		header.size = size_;
		header.flags = flags_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitMapMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	MapMemoryOperation(const MapMemoryOperation &) = delete;
	MapMemoryOperation &operator= (const MapMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		MapMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	BorrowedDescriptor space_;
	void *pointer_;
	uintptr_t offset_;
	size_t size_;
	uint32_t flags_;
	Receiver r_;
};

struct [[nodiscard]] MapMemorySender {
	using value_type = MapMemoryResult;

	MapMemorySender(BorrowedDescriptor memory, BorrowedDescriptor space, void *pointer,
			uintptr_t offset, size_t size, uint32_t flags)
	: memory_{std::move(memory)}, space_{std::move(space)}, pointer_{pointer},
			offset_{offset}, size_{size}, flags_{flags} { }

	template<typename Receiver>
	MapMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), std::move(space_), pointer_, offset_, size_, flags_,
				std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
	BorrowedDescriptor space_;
	void *pointer_;
	uintptr_t offset_;
	size_t size_;
	uint32_t flags_;
};

inline async::sender_awaiter<MapMemorySender, MapMemoryResult>
operator co_await (MapMemorySender sender) {
	return {std::move(sender)};
}

inline auto mapMemory(BorrowedDescriptor memory, BorrowedDescriptor space, void *pointer,
		uintptr_t offset, size_t size, uint32_t flags) {
	return MapMemorySender{std::move(memory), std::move(space), pointer, offset, size, flags};
}

// --------------------------------------------------------------------
// UnmapMemory
// --------------------------------------------------------------------

struct UnmapMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

template <typename Receiver>
struct UnmapMemoryOperation : private Context {
	UnmapMemoryOperation(BorrowedDescriptor space, void *pointer, size_t size, Receiver r)
	: space_{std::move(space)}, pointer_{pointer}, size_{size}, r_{std::move(r)} {}

	void start() {
		HelSqUnmapMemory header;
		header.spaceHandle = space_.getHandle();
		header.pointer = pointer_;
		header.size = size_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitUnmapMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	UnmapMemoryOperation(const UnmapMemoryOperation &) = delete;
	UnmapMemoryOperation &operator= (const UnmapMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		UnmapMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
	Receiver r_;
};

struct [[nodiscard]] UnmapMemorySender {
	using value_type = UnmapMemoryResult;

	UnmapMemorySender(BorrowedDescriptor space, void *pointer, size_t size)
	: space_{std::move(space)}, pointer_{pointer}, size_{size} { }

	template<typename Receiver>
	UnmapMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(space_), pointer_, size_, std::move(receiver)};
	}

private:
	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
};

inline async::sender_awaiter<UnmapMemorySender, UnmapMemoryResult>
operator co_await (UnmapMemorySender sender) {
	return {std::move(sender)};
}

inline auto unmapMemory(BorrowedDescriptor space, void *pointer, size_t size) {
	return UnmapMemorySender{std::move(space), pointer, size};
}

} // namespace helix_ng
