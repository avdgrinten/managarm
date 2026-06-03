#include <expected>
#include <span>
#include <string.h>
#include <cstddef>

#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <frg/container_of.hpp>
#include <frg/formatting.hpp>
#include <frg/dyn_array.hpp>
#include <frg/safe_int.hpp>
#include <frg/small_vector.hpp>
#include <thor-internal/cancel.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/ipc-queue.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#ifdef __x86_64__
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/arch/npt.hpp>
#include <thor-internal/arch/svm.hpp>
#include <thor-internal/arch/pic.hpp>
#endif
#include <hel.h>

using namespace thor;


namespace {
	// TODO: Replace this by a function that returns the type of special descriptor.
	bool isSpecialMemoryView(HelHandle handle) {
		return handle == kHelZeroMemory;
	}

	smarter::shared_ptr<MemoryView> getSpecialMemoryView(HelHandle handle) {
		assert(handle == kHelZeroMemory);
		return getZeroMemory();
	}
}

extern "C" int doCopyFromUser(void *dest, const void *src, size_t size);
extern "C" int doCopyToUser(void *dest, const void *src, size_t size);
extern "C" int doAtomicUserLoad(unsigned int *out, const unsigned int *p);

[[nodiscard]] bool readUserMemory(void *kernelPtr, const void *userPtr, size_t size) {
	uintptr_t limit;
	if(!(frg::safe_int{reinterpret_cast<uintptr_t>(userPtr)} + frg::safe_int{size}).into(limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyFromUser(kernelPtr, userPtr, size);
	disableUserAccess();
	return !e;
}

[[nodiscard]] bool writeUserMemory(void *userPtr, const void *kernelPtr, size_t size) {
	uintptr_t limit;
	if(!(frg::safe_int{reinterpret_cast<uintptr_t>(userPtr)} + frg::safe_int{size}).into(limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyToUser(userPtr, kernelPtr, size);
	disableUserAccess();
	return !e;
}

template<typename T>
[[nodiscard]] bool readUserObject(const T *pointer, T &object) {
	return readUserMemory(static_cast<void *>(&object), static_cast<const void *>(pointer), sizeof(T));
}

template<typename T>
[[nodiscard]] bool writeUserObject(T *pointer, T object) {
	return writeUserMemory(pointer, &object, sizeof(T));
}

template<typename T>
[[nodiscard]] bool readUserArray(const T *pointer, T *array, size_t count) {
	size_t size;
	if(!(frg::safe_int{sizeof(T)} * frg::safe_int{count}).into(size))
		return false;
	return readUserMemory(array, pointer, size);
}

template<typename T>
[[nodiscard]] bool writeUserArray(T *pointer, const T *array, size_t count) {
	size_t size;
	if(!(frg::safe_int{sizeof(T)} * frg::safe_int{count}).into(size))
		return false;
	return writeUserMemory(pointer, array, size);
}

size_t ipcSourceSize(size_t size) {
	return (size + 7) & ~size_t(7);
}

HelError translateError(Error error) {
	switch(error) {
	case Error::success: return kHelErrNone;
	case Error::illegalArgs: return kHelErrIllegalArgs;
	case Error::illegalObject: return kHelErrIllegalObject;
	case Error::illegalState: return kHelErrIllegalState;
	case Error::outOfBounds: return kHelErrOutOfBounds;
	case Error::cancelled: return kHelErrCancelled;
	case Error::futexRace: return kHelErrFutexRace;
	case Error::bufferTooSmall: return kHelErrBufferTooSmall;
	case Error::threadExited: return kHelErrThreadTerminated;
	case Error::transmissionMismatch: return kHelErrTransmissionMismatch;
	case Error::laneShutdown: return kHelErrLaneShutdown;
	case Error::endOfLane: return kHelErrEndOfLane;
	case Error::dismissed: return kHelErrDismissed;
	case Error::fault: return kHelErrFault;
	case Error::remoteFault: return kHelErrRemoteFault;
	case Error::noMemory: return kHelErrNoMemory;
	case Error::noHardwareSupport: return kHelErrNoHardwareSupport;
	case Error::alreadyExists: return kHelErrAlreadyExists;
	case Error::badPermissions: return kHelErrBadPermissions;
	case Error::noDescriptor: return kHelErrNoDescriptor;
	case Error::badDescriptor: return kHelErrBadDescriptor;
	case Error::other: return kHelErrOther;

	// Thor-internal error cases that should not be passed down to userspace.
	case Error::hardwareBroken: [[fallthrough]];
	case Error::protocolViolation: [[fallthrough]];
	case Error::spuriousOperation:
		warningLogger() << "thor: Encountered unexpected internal error "
				<< std::to_underlying(error) << " during translation to HelError" << frg::endlog;
		return kHelErrOther;
	}

	// The switch above should handle all cases due to -Wswitch.
	// If we still get here, something is most likely broken.
	warningLogger() << "thor: Encountered broken error "
			<< std::to_underlying(error) << " during translation to HelError" << frg::endlog;
	return kHelErrOther;
}

namespace {

template<typename Sink>
std::optional<size_t> printLog(Sink &p, const char *log, size_t chunk) {
	for(size_t i = 0; i < chunk && log[i]; i++) {
		if(log[i] == '\n') {
			p << frg::endlog;
			return i + 1;
		} else if(log[i] == '\0') {
			p << frg::endlog;
			return std::nullopt;
		} else {
			p << frg::char_fmt(log[i]);
		}
	}
	p << frg::endlog;
	return chunk;
};

} // namespace

HelError helLog(HelLogSeverity severity, const char *string, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		char log[logLineLength];
		auto chunk = frg::min(length - offset, size_t{logLineLength});

		if(!readUserArray(string + offset, log, chunk))
			return kHelErrFault;

		switch(severity) {
			case kHelLogSeverityEmergency:
			case kHelLogSeverityAlert:
			case kHelLogSeverityCritical:
			case kHelLogSeverityError: {
				auto p = urgentLogger();
				auto ret = printLog(p, log, chunk);
				if(ret)
					chunk = ret.value();
				else
					return kHelErrNone;
				break;
			}
			case kHelLogSeverityWarning: {
				auto p = warningLogger();
				auto ret = printLog(p, log, chunk);
				if(ret)
					chunk = ret.value();
				else
					return kHelErrNone;
				break;
			}
			case kHelLogSeverityNotice:
			case kHelLogSeverityInfo:
			default: {
				auto p = infoLogger();
				auto ret = printLog(p, log, chunk);
				if(ret)
					chunk = ret.value();
				else
					return kHelErrNone;
				break;
			}
			case kHelLogSeverityDebug: {
				auto p = debugLogger();
				auto ret = printLog(p, log, chunk);
				if(ret)
					chunk = ret.value();
				else
					return kHelErrNone;
				break;
			}
		}

		offset += chunk;
	}

	return kHelErrNone;
}

HelError helNop() {
	return kHelErrNone;
}

HelError doSubmitAsyncNop(smarter::shared_ptr<IpcQueue> queue, uintptr_t context) {
	[] (smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helSubmitAsyncNop(HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto queueOutcome = thisUniverse->inspectDescriptor(queueHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IpcQueue>, Error> {
		if(!desc.is<QueueDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<QueueDescriptor>().queue;
	});
	if(!queueOutcome)
		return translateError(queueOutcome.error());

	doSubmitAsyncNop(std::move(*queueOutcome), context);

	return kHelErrNone;
}

HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto new_universe = smarter::allocate_shared<Universe>(*kernelAlloc);

	*handle = this_universe->attachDescriptor(
			UniverseDescriptor(std::move(new_universe)));

	return kHelErrNone;
}

HelError
helTransferDescriptor(HelHandle handle, HelHandle universeHandle, HelTransferDescriptorFlags direction, HelHandle *outHandle) {
	if(direction != kHelTransferDescriptorOut && direction != kHelTransferDescriptorIn)
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<Universe> universe;
	if(universeHandle == kHelThisUniverse) {
		universe = thisUniverse.lock();
	}else{
		auto universeOutcome = thisUniverse->inspectDescriptor(universeHandle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Universe>, Error> {
			if(!desc.is<UniverseDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<UniverseDescriptor>().universe;
		});
		if(!universeOutcome)
			return translateError(universeOutcome.error());
		universe = std::move(*universeOutcome);
	}

	smarter::shared_ptr<Universe> srcUniverse;
	smarter::shared_ptr<Universe> dstUniverse;
	if(direction == kHelTransferDescriptorOut) {
		srcUniverse = thisUniverse.lock();
		dstUniverse = universe;
	} else {
		assert(direction == kHelTransferDescriptorIn);
		dstUniverse = thisUniverse.lock();
		srcUniverse = universe;
	}

	auto maybeDescriptor = srcUniverse->getDescriptor(handle);
	if (!maybeDescriptor)
		return kHelErrNoDescriptor;

	// TODO: make sure the descriptor is copyable.

	*outHandle = dstUniverse->attachDescriptor(std::move(*maybeDescriptor));
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto infoOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<void, Error> {
		switch(desc.tag()) {
		default:
			return std::unexpected{Error::other};
		}
	});
	if(!infoOutcome)
		return translateError(infoOutcome.error());

	return kHelErrNone;
}

HelError helGetCredentials(HelHandle handle, uint32_t flags, char *credentials) {
	if (flags)
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	std::array<char, 16> creds;
	if(handle == kHelThisThread) {
		creds = thisThread->credentials();
	}else{
		auto outcome = thisUniverse->inspectDescriptor(handle,
				[&](AnyDescriptor &desc) -> std::expected<void, Error> {
			if(desc.is<ThreadDescriptor>())
				creds = desc.get<ThreadDescriptor>().thread->credentials();
			else if(desc.is<LaneDescriptor>())
				creds = desc.get<LaneDescriptor>().handle.getStream()->credentials().credentials();
			else
				return std::unexpected{Error::badDescriptor};
			return {};
		});
		if(!outcome)
			return translateError(outcome.error());
	}

	if(!writeUserMemory(credentials, creds.data(), creds.size()))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle universeHandle, HelHandle handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<Universe> universe;
	if(universeHandle == kHelThisUniverse) {
		universe = thisUniverse.lock();
	}else{
		auto universeOutcome = thisUniverse->inspectDescriptor(universeHandle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Universe>, Error> {
			if(!desc.is<UniverseDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<UniverseDescriptor>().universe;
		});
		if(!universeOutcome)
			return translateError(universeOutcome.error());
		universe = std::move(*universeOutcome);
	}

	auto descriptor = universe->detachDescriptor(handle);
	if(!descriptor)
		return kHelErrNoDescriptor;

	// Note that the descriptor is released outside of the locks.

	return kHelErrNone;
}

HelError helCreateQueue(const HelQueueParameters *paramsPtr, HelHandle *handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	HelQueueParameters params;
	if(!readUserObject(paramsPtr, params))
		return kHelErrFault;

	if(params.flags)
		return kHelErrIllegalArgs;

	auto queueOutcome = IpcQueue::create(params.numChunks, params.chunkSize,
			params.numSqChunks);
	if(!queueOutcome)
		return translateError(queueOutcome.error());
	*handle = thisUniverse->attachDescriptor(
			QueueDescriptor(std::move(*queueOutcome)));

	return kHelErrNone;
}

HelError helDriveQueue(HelHandle handle, uint32_t flags, uint32_t notifyMask) {
	if (flags & ~kHelDriveWait)
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto queueOutcome = thisUniverse->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IpcQueue>, Error> {
		if(!desc.is<QueueDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<QueueDescriptor>().queue;
	});
	if(!queueOutcome)
		return translateError(queueOutcome.error());
	auto queue = std::move(*queueOutcome);

	// Always raise cqEvent to indicate that kernelNotify may have changed.
	queue->raiseCqEvent();

	// Process any pending SQ elements.
	queue->processSq();

	// If requested, wait until userNotify & kNotifyProgress is non-zero.
	if(flags & kHelDriveWait) {
		if (!queue->checkUserNotify((int)notifyMask)) {
			auto outcome = Thread::asyncBlockCurrentInterruptible(
				async::lambda([&](async::cancellation_token ct) {
					return queue->waitUserEvent((int)notifyMask, ct);
				}),
				thisThread->mainWorkQueue().get()
			);
			if (!outcome) {
				return kHelErrCancelled;
			}
		}
	}

	return kHelErrNone;
}

HelError helAlertQueue(HelHandle handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto queueOutcome = thisUniverse->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IpcQueue>, Error> {
		if(!desc.is<QueueDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<QueueDescriptor>().queue;
	});
	if(!queueOutcome)
		return translateError(queueOutcome.error());
	auto queue = std::move(*queueOutcome);

	queue->alert();

	return kHelErrNone;
}

// Resolves a hierarchy handle that owns newly allocated memory.
// A null handle is rejected: every allocation must name an owning hierarchy.
static std::expected<smarter::shared_ptr<Hierarchy>, HelError>
resolveHierarchy(Universe *universe, HelHandle handle) {
	if(handle == kHelNullHandle)
		return std::unexpected{kHelErrIllegalArgs};
	auto outcome = universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Hierarchy>, Error> {
		if(!desc.is<HierarchyDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<HierarchyDescriptor>().hierarchy;
	});
	if(!outcome)
		return std::unexpected{translateError(outcome.error())};
	return std::move(*outcome);
}

HelError helAllocateMemory(HelHandle hierarchyHandle, size_t size, uint32_t flags,
		const HelAllocRestrictions *restrictions, HelHandle *handle) {
	if(!size)
		return kHelErrIllegalArgs;
	if(size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto hierarchy = resolveHierarchy(thisUniverse.get(), hierarchyHandle);
	if(!hierarchy)
		return hierarchy.error();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frg::endlog;

	HelAllocRestrictions effective{
		.addressBits = 64
	};
	if(restrictions)
		if(!readUserMemory(&effective, restrictions, sizeof(HelAllocRestrictions)))
			return kHelErrFault;

	smarter::shared_ptr<AllocatedMemory> memory;
	if(flags & kHelAllocContinuous) {
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, *hierarchy, size,
				effective.addressBits, size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, *hierarchy, size,
				effective.addressBits);
	}else{
		// TODO:
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, *hierarchy, size,
				effective.addressBits);
	}
	memory->selfPtr = memory;

	*handle = thisUniverse->attachDescriptor(
			MemoryViewDescriptor(std::move(memory)));

	return kHelErrNone;
}

HelError doSubmitResizeMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		size_t newSize, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<MemoryView> memory, size_t newSize,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(memory->resize(newSize));

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		if (!outcome)
			helResult.error = translateError(outcome.error());
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(memory), newSize, std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helCreateManagedMemory(HelHandle hierarchyHandle, size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	if(flags & ~uint32_t{kHelManagedReadahead})
		return kHelErrIllegalArgs;
	if(size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto hierarchy = resolveHierarchy(thisUniverse.get(), hierarchyHandle);
	if(!hierarchy)
		return hierarchy.error();

	auto managed = smarter::allocate_shared<ManagedSpace>(*kernelAlloc, *hierarchy, size,
			flags & kHelManagedReadahead);
	managed->selfPtr = managed;
	auto backingMemory = smarter::allocate_shared<BackingMemory>(*kernelAlloc, managed);
	auto frontalMemory = smarter::allocate_shared<FrontalMemory>(*kernelAlloc, std::move(managed));
	frontalMemory->selfPtr = frontalMemory;

	*backing_handle = thisUniverse->attachDescriptor(
			MemoryViewDescriptor(std::move(backingMemory)));
	*frontal_handle = thisUniverse->attachDescriptor(
			MemoryViewDescriptor(std::move(frontalMemory)));

	return kHelErrNone;
}

HelError helCopyOnWrite(HelHandle hierarchyHandle, HelHandle memoryHandle,
		uintptr_t offset, size_t size, HelHandle *outHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto hierarchy = resolveHierarchy(this_universe.get(), hierarchyHandle);
	if(!hierarchy)
		return hierarchy.error();

	smarter::shared_ptr<MemoryView> view;

	if(memoryHandle < 0) {
		if(!isSpecialMemoryView(memoryHandle))
			return kHelErrBadDescriptor;
		view = getSpecialMemoryView(memoryHandle);
	} else {
		auto viewOutcome = this_universe->inspectDescriptor(memoryHandle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
			if(!desc.is<MemoryViewDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<MemoryViewDescriptor>().memory;
		});
		if(!viewOutcome)
			return translateError(viewOutcome.error());
		view = std::move(*viewOutcome);
	}

	auto slice = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc, *hierarchy,
			std::move(view), offset, size);
	slice->selfPtr = slice;
	*outHandle = this_universe->attachDescriptor(
			MemoryViewDescriptor(std::move(slice)));

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	if (physical & (kPageSize - 1))
		return kHelErrIllegalArgs;
	if (size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc, physical, size,
			CachingMode::null);
	*handle = this_universe->attachDescriptor(
			MemoryViewDescriptor(std::move(memory)));

	return kHelErrNone;
}

HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = smarter::allocate_shared<IndirectMemory>(*kernelAlloc, numSlots);
	*handle = this_universe->attachDescriptor(
			MemoryViewDescriptor(std::move(memory)));

	return kHelErrNone;
}

HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slot,
		HelHandle memoryHandle, uintptr_t offset, size_t size) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto indirectOutcome = thisUniverse->inspectDescriptor(indirectHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!indirectOutcome)
		return translateError(indirectOutcome.error());
	auto indirectView = std::move(*indirectOutcome);

	smarter::shared_ptr<MemoryView> memoryView;
	CachingFlags cacheFlags = 0;
	auto memoryOutcome = thisUniverse->inspectDescriptor(memoryHandle,
			[&](AnyDescriptor &desc) -> std::expected<void, Error> {
		if(desc.is<MemoryViewDescriptor>()) {
			memoryView = desc.get<MemoryViewDescriptor>().memory;
		} else if(desc.is<MemorySliceDescriptor>()) {
			memoryView = desc.get<MemorySliceDescriptor>().slice->getView();
			offset += desc.get<MemorySliceDescriptor>().slice->offset();
			cacheFlags = desc.get<MemorySliceDescriptor>().slice->getCachingFlags();
		} else {
			return std::unexpected{Error::badDescriptor};
		}
		return {};
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());

	if(auto e = indirectView->setIndirection(slot, std::move(memoryView), offset, size, cacheFlags);
			e != Error::success) {
		if(e == Error::illegalObject) {
			return kHelErrUnsupportedOperation;
		}else{
			assert(e == Error::outOfBounds);
			return kHelErrOutOfBounds;
		}
	}
	return kHelErrNone;
}

HelError helCreateSliceView(HelHandle memoryHandle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	if (offset & (kPageSize - 1))
		return kHelErrIllegalArgs;
	if (size & (kPageSize - 1))
		return kHelErrIllegalArgs;
	if (flags & ~kHelSliceCacheWriteCombine)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto viewOutcome = this_universe->inspectDescriptor(memoryHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!viewOutcome)
		return translateError(viewOutcome.error());
	auto view = std::move(*viewOutcome);

	CachingFlags cachingFlags = 0;
	if(flags & kHelSliceCacheWriteCombine)
		cachingFlags = cacheWriteCombine;

	auto slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
			std::move(view), offset, size, cachingFlags);
	*handle = this_universe->attachDescriptor(
			MemorySliceDescriptor(std::move(slice)));

	return kHelErrNone;
}

HelError doSubmitForkMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto viewOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!viewOutcome)
		return translateError(viewOutcome.error());
	auto view = std::move(*viewOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelHandleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::weak_ptr<Universe> weakUniverse,
			smarter::shared_ptr<MemoryView> view,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(view->fork());

		if(!outcome) {
			HelHandleResult helResult{.error = translateError(outcome.error())};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		auto universe = weakUniverse.lock();
		if (!universe) {
			HelHandleResult helResult{.error = kHelErrThreadTerminated};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		HelHandle forkedHandle = universe->attachDescriptor(
				MemoryViewDescriptor(outcome.value()));

		HelHandleResult helResult{.error = kHelErrNone, .handle = forkedHandle};
		QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(this_universe.lock(), std::move(view), std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError doSubmitWritebackFence(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t offset, size_t size, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<MemoryView> memory, uintptr_t offset, size_t size,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(memory->writebackFence(offset, size));

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		if (!outcome)
			helResult.error = translateError(outcome.error());
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(memory), offset, size, std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError doSubmitInvalidateMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t offset, size_t size, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<MemoryView> memory, uintptr_t offset, size_t size,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(memory->invalidateRange(offset, size));

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		if (!outcome)
			helResult.error = translateError(outcome.error());
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(memory), offset, size, std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helExtendHierarchy(HelHandle hierarchyHandle,
		const HelHierarchyParameters *paramsPtr, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	HelHierarchyParameters params;
	if(!readUserObject(paramsPtr, params))
		return kHelErrFault;

	size_t tagLen = 0;
	while(tagLen < sizeof(params.tag) && params.tag[tagLen])
		++tagLen;
	if(tagLen == sizeof(params.tag))
		return kHelErrIllegalArgs;

	frg::string<KernelAlloc> tagStr{*kernelAlloc};
	tagStr.resize(tagLen);
	if(tagLen)
		memcpy(tagStr.data(), params.tag, tagLen);

	auto parentOutcome = this_universe->inspectDescriptor(hierarchyHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Hierarchy>, Error> {
		if(!desc.is<HierarchyDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<HierarchyDescriptor>().hierarchy;
	});
	if(!parentOutcome)
		return translateError(parentOutcome.error());

	auto childResult = Hierarchy::extend(std::move(*parentOutcome), std::move(tagStr));
	if(!childResult) {
		switch(childResult.error()) {
			case Error::noMemory: return kHelErrNoMemory;
			case Error::illegalArgs: return kHelErrIllegalArgs;
			default: return kHelErrOther;
		}
	}

	*handle = this_universe->attachDescriptor(
			HierarchyDescriptor(std::move(*childResult)));

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle hierarchyHandle, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto hierarchyOutcome = this_universe->inspectDescriptor(hierarchyHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Hierarchy>, Error> {
		if(!desc.is<HierarchyDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<HierarchyDescriptor>().hierarchy;
	});
	if(!hierarchyOutcome)
		return translateError(hierarchyOutcome.error());

	auto space = AddressSpace::create(std::move(*hierarchyOutcome));

	*handle = this_universe->attachDescriptor(
			AddressSpaceDescriptor(std::move(space)));

	return kHelErrNone;
}

HelError helCreateVirtualizedSpace(HelHandle hierarchyHandle, HelHandle *handle) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto hierarchyOutcome = this_universe->inspectDescriptor(hierarchyHandle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Hierarchy>, Error> {
		if(!desc.is<HierarchyDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<HierarchyDescriptor>().hierarchy;
	});
	if(!hierarchyOutcome)
		return translateError(hierarchyOutcome.error());
	auto hierarchy = std::move(*hierarchyOutcome);

	PhysicalAddr pml4e = physicalAllocator->allocate(kPageSize);
	if(pml4e == static_cast<PhysicalAddr>(-1)) {
		return kHelErrNoMemory;
	}
	PageAccessor paccessor{pml4e};
	memset(paccessor.get(), 0, kPageSize);

	smarter::shared_ptr<VirtualizedPageSpace> vspace;
	if(getGlobalCpuFeatures()->haveVmx) {
		vspace = thor::vmx::EptSpace::create(pml4e, std::move(hierarchy));
	} else if(getGlobalCpuFeatures()->haveSvm) {
		vspace = thor::svm::NptSpace::create(pml4e, std::move(hierarchy));
	} else {
		physicalAllocator->free(pml4e, kPageSize);
		return kHelErrNoHardwareSupport;
	}

	*handle = this_universe->attachDescriptor(
			VirtualizedSpaceDescriptor(std::move(vspace)));
	return kHelErrNone;
#else
	(void)hierarchyHandle;
	(void)handle;
	return kHelErrNoHardwareSupport;
#endif
}

HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto vspaceOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<VirtualizedPageSpace>, Error> {
		if(!desc.is<VirtualizedSpaceDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<VirtualizedSpaceDescriptor>().space;
	});
	if(!vspaceOutcome)
		return translateError(vspaceOutcome.error());
	auto vspace = std::move(*vspaceOutcome);

	smarter::shared_ptr<VirtualizedCpu> vcpu;
	if(getGlobalCpuFeatures()->haveVmx)
		vcpu = smarter::allocate_shared<vmx::Vmcs>(Allocator{}, (smarter::static_pointer_cast<thor::vmx::EptSpace>(vspace)));
	else if(getGlobalCpuFeatures()->haveSvm)
		vcpu = smarter::allocate_shared<svm::Vcpu>(Allocator{}, (smarter::static_pointer_cast<thor::svm::NptSpace>(vspace)));
	else
		return kHelErrNoHardwareSupport;

	*out = this_universe->attachDescriptor(
			VirtualizedCpuDescriptor(std::move(vcpu)));
	return kHelErrNone;
#else
	(void)handle;
	(void)out;
	return kHelErrNoHardwareSupport;
#endif
}

HelError helRunVirtualizedCpu(HelHandle handle, HelVmexitReason *exitInfo) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto vcpuOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<VirtualizedCpu>, Error> {
		if(!desc.is<VirtualizedCpuDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<VirtualizedCpuDescriptor>().vcpu;
	});
	if(!vcpuOutcome)
		return translateError(vcpuOutcome.error());
	auto vcpu = std::move(*vcpuOutcome);

	auto info = vcpu->run();
	if(!writeUserObject(exitInfo, info))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helGetRandomBytes(void *buffer, size_t wantedSize, size_t *actualSize) {
	char bounceBuffer[128];
	size_t generatedSize = generateRandomBytes(bounceBuffer,
			frg::min(wantedSize, size_t{128}));

	if(!writeUserMemory(buffer, bounceBuffer, generatedSize))
		return kHelErrFault;

	*actualSize = generatedSize;
	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actualPointer) {
	if(length == 0)
		return kHelErrIllegalArgs;
	if((uintptr_t)pointer % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(offset % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(length % kPageSize != 0)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t map_flags = 0;
	if(flags & kHelMapFixed) {
		map_flags |= AddressSpace::kMapFixed;
	}else if(flags & kHelMapFixedNoReplace) {
		map_flags |= AddressSpace::kMapFixedNoReplace;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	if(flags & kHelMapProtRead)
		map_flags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		map_flags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		map_flags |= AddressSpace::kMapProtExecute;

	if(flags & kHelMapDontRequireBacking)
		map_flags |= AddressSpace::kMapDontRequireBacking;

	smarter::shared_ptr<MemorySlice> slice;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	smarter::shared_ptr<VirtualSpace> vspace;
	bool isVspace = false;

	auto memoryOutcome = this_universe->inspectDescriptor(memory_handle,
			[&](AnyDescriptor &desc) -> std::expected<void, Error> {
		if(desc.is<MemorySliceDescriptor>()) {
			slice = desc.get<MemorySliceDescriptor>().slice;
		}else if(desc.is<MemoryViewDescriptor>()) {
			auto memory = desc.get<MemoryViewDescriptor>().memory;
			auto sliceLength = memory->getLength();
			slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, sliceLength);
		}else if(desc.is<QueueDescriptor>()) {
			auto memory = desc.get<QueueDescriptor>().queue->getMemory();
			auto sliceLength = memory->getLength();
			slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, sliceLength);
		}else{
			return std::unexpected{Error::badDescriptor};
		}
		return {};
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());

	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().lock();
	}else{
		auto spaceOutcome = this_universe->inspectDescriptor(space_handle,
				[&](AnyDescriptor &desc) -> std::expected<void, Error> {
			if(desc.is<AddressSpaceDescriptor>()) {
				space = desc.get<AddressSpaceDescriptor>().space;
			} else if(desc.is<VirtualizedSpaceDescriptor>()) {
				isVspace = true;
				vspace = desc.get<VirtualizedSpaceDescriptor>().space;
			} else {
				return std::unexpected{Error::badDescriptor};
			}
			return {};
		});
		if(!spaceOutcome)
			return translateError(spaceOutcome.error());
	}

	// TODO: check proper alignment

	frg::expected<Error, VirtualAddr> mapResult;
	if(!isVspace) {
		if(map_flags & AddressSpace::kMapFixed && !pointer)
			return kHelErrIllegalArgs; // Non-vspaces aren't allowed to map at NULL

		mapResult = Thread::asyncBlockCurrent(
			space->map(slice, (VirtualAddr)pointer, offset, length, map_flags),
			getCurrentThread()->pagingWorkQueue().get()
		);
	} else {
		mapResult = Thread::asyncBlockCurrent(
			vspace->map(slice, (VirtualAddr)pointer, offset, length, map_flags),
			getCurrentThread()->pagingWorkQueue().get()
		);
	}

	if(!mapResult) {
		assert(mapResult.error() == Error::bufferTooSmall || mapResult.error() == Error::alreadyExists || mapResult.error() == Error::noMemory);

		if(mapResult.error() == Error::bufferTooSmall)
			return kHelErrBufferTooSmall;
		else if(mapResult.error() == Error::noMemory)
			return kHelErrNoMemory;
		else if(mapResult.error() == Error::alreadyExists)
			return kHelErrAlreadyExists;
	}

	*actualPointer = (void *)mapResult.value();
	return kHelErrNone;
}

HelError doSubmitProtectMemory(HelHandle space_handle, smarter::shared_ptr<IpcQueue> queue,
		void *pointer, size_t length, uint32_t flags, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t protectFlags = 0;
	if(flags & kHelMapProtRead)
		protectFlags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		protectFlags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		protectFlags |= AddressSpace::kMapProtExecute;

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().lock();
	}else{
		auto spaceOutcome = this_universe->inspectDescriptor(space_handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<AddressSpace, BindableHandle>, Error> {
			if(!desc.is<AddressSpaceDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<AddressSpaceDescriptor>().space;
		});
		if(!spaceOutcome)
			return translateError(spaceOutcome.error());
		space = std::move(*spaceOutcome);
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<Thread> thisThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			smarter::shared_ptr<IpcQueue> queue,
			VirtualAddr pointer, size_t length,
			uint32_t protectFlags, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(space->protect(pointer, length, protectFlags));
		// TODO: handle errors after propagating them through VirtualSpace::protect.
		assert(outcome);

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(this_thread.lock(), std::move(space), std::move(queue), reinterpret_cast<VirtualAddr>(pointer),
			length, protectFlags, context,
			enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().lock();
	}else{
		auto spaceOutcome = this_universe->inspectDescriptor(space_handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<AddressSpace, BindableHandle>, Error> {
			if(!desc.is<AddressSpaceDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<AddressSpaceDescriptor>().space;
		});
		if(!spaceOutcome)
			return translateError(spaceOutcome.error());
		space = std::move(*spaceOutcome);
	}

	auto outcome = Thread::asyncBlockCurrent(
		space->unmap((VirtualAddr)pointer, length),
		getCurrentThread()->pagingWorkQueue().get()
	);
	if(!outcome) {
		assert(outcome.error() == Error::illegalArgs);
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError doSubmitSynchronizeSpace(HelHandle spaceHandle, smarter::shared_ptr<IpcQueue> queue,
		void *pointer, size_t length, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	if(spaceHandle == kHelNullHandle) {
		space = thisThread->getAddressSpace().lock();
	}else{
		auto spaceOutcome = thisUniverse->inspectDescriptor(spaceHandle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<AddressSpace, BindableHandle>, Error> {
			if(!desc.is<AddressSpaceDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<AddressSpaceDescriptor>().space;
		});
		if(!spaceOutcome)
			return translateError(spaceOutcome.error());
		space = std::move(*spaceOutcome);
	}

	[] (smarter::shared_ptr<Thread> thisThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			void *pointer, size_t length,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto outcome = co_await onExceptionalWq(space->synchronize((VirtualAddr)pointer, length));

		HelSimpleResult helResult{
			.error = outcome ? kHelErrNone : translateError(outcome.error()),
			.reserved = {},
		};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(thisThread.lock(), std::move(space), pointer, length, std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helPointerPhysical(const void *pointer, uintptr_t *physical) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace().lock();

	auto disp = (reinterpret_cast<uintptr_t>(pointer) & (kPageSize - 1));
	auto pageAddress = reinterpret_cast<VirtualAddr>(pointer) - disp;

	auto physicalOrError = Thread::asyncBlockCurrent(
		space->retrievePhysical(pageAddress),
		thisThread->pagingWorkQueue().get()
	);
	if(!physicalOrError) {
		assert(physicalOrError.error() == Error::fault);
		return kHelErrFault;
	}

	*physical = physicalOrError.value() + disp;

	return kHelErrNone;
}

HelError doSubmitReadMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t address, size_t length, void *buffer, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	auto wrapper = thisUniverse->getDescriptor(handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	descriptor = std::move(*wrapper);

	auto readMemoryView = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<MemoryView> view,
			uintptr_t address, size_t length, void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(!(frg::safe_int{reinterpret_cast<uintptr_t>(buffer)} + frg::safe_int{length}).into(limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[4096]; // TODO: Use a temporarily allocated page?
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{4096});
				auto copyOutcome = co_await onExceptionalWq(view->copyFrom(address + progress, temp, chunk));
				if(!copyOutcome) {
					error = copyOutcome.error();
					break;
				}

				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto readVirtualSpace = []<typename Space, typename Token> (
			smarter::shared_ptr<Space, Token> space,
			uintptr_t address, size_t length, void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(!(frg::safe_int{reinterpret_cast<uintptr_t>(buffer)} + frg::safe_int{length}).into(limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[4096]; // TODO: Use a temporarily allocated page?
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{4096});

				auto outcome = co_await onExceptionalWq(space->readSpace(address + progress, temp, chunk));
				if(!outcome) {
					error = Error::fault;
					break;
				}

				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		readMemoryView(thisThread.lock(),
				std::move(view), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		readVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		readVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		readVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError doSubmitWriteMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t address, size_t length, const void *buffer, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	auto wrapper = thisUniverse->getDescriptor(handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	descriptor = std::move(*wrapper);

	auto writeMemoryView = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<MemoryView> view,
			uintptr_t address, size_t length, const void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(!(frg::safe_int{reinterpret_cast<uintptr_t>(buffer)} + frg::safe_int{length}).into(limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[4096]; // TODO: Use a temporarily allocated page?
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{4096});

				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}

				auto copyOutcome = co_await onExceptionalWq(view->copyTo(address + progress, temp, chunk));
				if(!copyOutcome) {
					error = copyOutcome.error();
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto writeVirtualSpace = []<typename Space, typename Token> (
			smarter::shared_ptr<Space, Token> space,
			uintptr_t address, size_t length, const void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(!(frg::safe_int{reinterpret_cast<uintptr_t>(buffer)} + frg::safe_int{length}).into(limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[4096]; // TODO: Use a temporarily allocated page?
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{4096});

				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}

				auto outcome = co_await onExceptionalWq(space->writeSpace(address + progress, temp, chunk));
				if(!outcome) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		writeMemoryView(thisThread.lock(),
				std::move(view), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		writeVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		writeVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		writeVirtualSpace(
				std::move(space), address, length, buffer, std::move(queue), context,
				enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	*size = memory->getLength();
	return kHelErrNone;
}

HelError doSubmitManageMemory(HelHandle handle, smarter::shared_ptr<IpcQueue> queue, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelManageResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<IpcQueue> queue,
			smarter::shared_ptr<MemoryView> memory,
			uintptr_t context,
			enable_detached_coroutine) -> void {
		auto resultOrError = co_await memory->pollNotification();

		HelManageResult helResult;
		if(!resultOrError) {
			helResult = HelManageResult{
				translateError(resultOrError.error()), 0, 0, 0
			};
		} else {
			auto result = resultOrError.value();
			int manageRequest = 0;
			switch (result.type) {
				case ManageRequest::null:
					// This should not be returned from pollNotification().
					break;
				case ManageRequest::initialize:
					manageRequest = kHelManageInitialize;
					break;
				case ManageRequest::writeback:
					manageRequest = kHelManageWriteback;
					break;
			}
			assert(manageRequest); // The switch needs to be exhaustive.
			helResult = HelManageResult{
				translateError(Error::success), manageRequest, result.offset, result.size
			};
		}
		QueueSource ipcSource{&helResult, sizeof(HelManageResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(queue), std::move(memory), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helUpdateMemory(HelHandle handle, int type,
		uintptr_t offset, size_t length) {
	if (offset & (kPageSize - 1))
		return kHelErrIllegalArgs;
	if (length & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	Error error;
	switch(type) {
	case kHelManageInitialize:
		error = memory->updateRange(ManageRequest::initialize, offset, length);
		break;
	case kHelManageWriteback:
		error = memory->updateRange(ManageRequest::writeback, offset, length);
		break;
	default:
		return kHelErrIllegalArgs;
	}

	if(error == Error::illegalObject)
		return kHelErrUnsupportedOperation;
	else if(error == Error::illegalArgs)
		return kHelErrIllegalArgs;

	assert(error == Error::success);
	return kHelErrNone;
}

HelError doSubmitLockMemoryView(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t offset, size_t size, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());
	auto memory = std::move(*memoryOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelHandleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::weak_ptr<thor::Universe> weakUniverse,
			smarter::shared_ptr<MemoryView> memory,
			smarter::shared_ptr<IpcQueue> queue,
			uintptr_t offset, size_t size,
			uintptr_t context,
			enable_detached_coroutine) -> void {
		MemoryViewLockHandle lockHandle{memory, offset, size};
		lockHandle.acquire();
		if(!lockHandle) {
			// TODO: Return a better error.
			HelHandleResult helResult{.error = kHelErrFault};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		// Touch the memory range.
		// TODO: this should be optional (it is only really useful for no-backing mappings).
		auto touchOutcome = co_await onExceptionalWq(memory->touchFullRange(offset, size, fetchRequireMutable));
		if(!touchOutcome) {
			HelHandleResult helResult{.error = translateError(touchOutcome.error())};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		// Attach the descriptor.
		auto universe = weakUniverse.lock();
		if (!universe) {
			HelHandleResult helResult{.error = kHelErrThreadTerminated};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		HelHandle handle;
		handle = universe->attachDescriptor(
				MemoryViewLockDescriptor{
					smarter::allocate_shared<NamedMemoryViewLock>(
						*kernelAlloc, std::move(lockHandle))});

		HelHandleResult helResult{.error = kHelErrNone, .handle = handle};
		QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(this_universe.lock(), std::move(memory), std::move(queue),
		offset, size, context,
		enable_detached_coroutine{this_thread->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	if (offset & (kPageSize - 1))
		return kHelErrIllegalArgs;
	if (length & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memoryOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
		if(!desc.is<MemoryViewDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<MemoryViewDescriptor>().memory;
	});
	if(!memoryOutcome)
		return translateError(memoryOutcome.error());

/*	auto handle_load = smarter::allocate_shared<AsyncInitiateLoad>(*kernelAlloc,
			NullCompleter(), offset, length);
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(std::move(handle_load));
	}*/

	return kHelErrNone;
}

std::atomic<unsigned int> globalNextCpu = 0;

HelError helCreateThread(HelHandle universe_handle, HelHandle space_handle,
		int abi, void *ip, void *sp, uint32_t flags, HelHandle *handle) {
	(void)abi;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(flags & ~(kHelThreadStopped))
		return kHelErrIllegalArgs;

	smarter::shared_ptr<Universe> universe;
	if(universe_handle == kHelNullHandle) {
		universe = this_thread->getUniverse().lock();
	}else{
		auto universeOutcome = this_universe->inspectDescriptor(universe_handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Universe>, Error> {
			if(!desc.is<UniverseDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<UniverseDescriptor>().universe;
		});
		if(!universeOutcome)
			return translateError(universeOutcome.error());
		universe = std::move(*universeOutcome);
	}

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().lock();
	}else{
		auto spaceOutcome = this_universe->inspectDescriptor(space_handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<AddressSpace, BindableHandle>, Error> {
			if(!desc.is<AddressSpaceDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return desc.get<AddressSpaceDescriptor>().space;
		});
		if(!spaceOutcome)
			return translateError(spaceOutcome.error());
		space = std::move(*spaceOutcome);
	}

	AbiParameters params;
	params.ip = (uintptr_t)ip;
	params.sp = (uintptr_t)sp;

	auto new_thread = Thread::create(std::move(universe), std::move(space), params);

	// Adding a large prime (coprime to getCpuCount()) should yield a good distribution.
	auto cpuIndex = globalNextCpu.fetch_add(4099, std::memory_order_relaxed) % getCpuCount();
//	infoLogger() << "thor: New thread on CPU #" << cpu << frg::endlog;
	auto *cpu = getCpuData(cpuIndex);
	LoadBalancer::singleton().connect(new_thread.get(), cpu);
	Scheduler::associate(new_thread.get(), &localScheduler.get(cpu));
	Scheduler::resume(new_thread.get());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(smarter::rc_policy_downcast<smarter::default_rc_policy>(new_thread));

	*handle = this_universe->attachDescriptor(
			ThreadDescriptor(std::move(new_thread)));

	return kHelErrNone;
}

HelError helQueryThreadStats(HelHandle handle, HelThreadStats *user_stats) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.lock();
	}else{
		auto threadOutcome = this_universe->inspectDescriptor(handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
			if(!desc.is<ThreadDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
		});
		if(!threadOutcome)
			return translateError(threadOutcome.error());
		thread = std::move(*threadOutcome);
	}

	HelThreadStats stats;
	memset(&stats, 0, sizeof(HelThreadStats));
	stats.userTime = thread->runTime();

	if(!writeUserObject(user_stats, stats))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helSetPriority(HelHandle handle, int priority) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.lock();
	}else{
		auto threadOutcome = this_universe->inspectDescriptor(handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
			if(!desc.is<ThreadDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
		});
		if(!threadOutcome)
			return translateError(threadOutcome.error());
		thread = std::move(*threadOutcome);
	}

	Scheduler::setPriority(thread.get(), priority);

	return kHelErrNone;
}

HelError helYield() {
	Thread::deferCurrent();

	return kHelErrNone;
}

HelError doSubmitObserve(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto threadOutcome = thisUniverse->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
		if(!desc.is<ThreadDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
	});
	if(!threadOutcome)
		return translateError(threadOutcome.error());
	auto thread = std::move(*threadOutcome);

	if(!queue->validSize(ipcSourceSize(sizeof(HelObserveResult))))
		return kHelErrQueueTooSmall;

	[] (smarter::shared_ptr<Thread> thread,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine) -> void {
		auto [error, interrupt] = co_await thread->observe();

		HelObserveResult helResult{translateError(error), 0};
		if(interrupt == kIntrNull) {
			helResult.observation = kHelObserveNull;
		}else if(interrupt == kIntrDivByZero) {
			helResult.observation = kHelObserveDivByZero;
		}else if(interrupt == kIntrRequested) {
			helResult.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			helResult.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			helResult.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrPageFault) {
			helResult.observation = kHelObservePageFault;
		}else if(interrupt == kIntrGeneralFault) {
			helResult.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrIllegalInstruction) {
			helResult.observation = kHelObserveIllegalInstruction;
		}else if(interrupt >= kIntrSuperCall) {
			helResult.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			thor::panicLogger() << "Unexpected interrupt" << frg::endlog;
			__builtin_unreachable();
		}
		QueueSource ipcSource{&helResult, sizeof(HelObserveResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(thread), std::move(queue), context,
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helKillThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto threadOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
		if(!desc.is<ThreadDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
	});
	if(!threadOutcome)
		return translateError(threadOutcome.error());
	auto thread = std::move(*threadOutcome);

	Thread::killOther(thread);

	return kHelErrNone;
}

HelError helInterruptThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto threadOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
		if(!desc.is<ThreadDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
	});
	if(!threadOutcome)
		return translateError(threadOutcome.error());
	auto thread = std::move(*threadOutcome);

	Thread::interruptOther(thread);

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto threadOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
		if(!desc.is<ThreadDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
	});
	if(!threadOutcome)
		return translateError(threadOutcome.error());
	auto thread = std::move(*threadOutcome);

	if(auto e = Thread::resumeOther(thread); e != Error::success) {
		if(e == Error::threadExited)
			return kHelErrThreadTerminated;
		assert(e == Error::illegalState);
		return kHelErrIllegalState;
	}

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	VirtualizedCpuDescriptor vcpu;
	auto outcome = this_universe->inspectDescriptor(handle,
			[&](AnyDescriptor &desc) -> std::expected<void, Error> {
		if(desc.is<ThreadDescriptor>()) {
			thread = smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
		} else if(desc.is<VirtualizedCpuDescriptor>()) {
			vcpu = desc.get<VirtualizedCpuDescriptor>();
		}else{
			return std::unexpected{Error::badDescriptor};
		}
		return {};
	});
	if(!outcome)
		return translateError(outcome.error());

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = *executor->ip();
			regs[1] = *executor->sp();
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[15];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->rax;
			regs[1] = executor->general()->rbx;
			regs[2] = executor->general()->rcx;
			regs[3] = executor->general()->rdx;
			regs[4] = executor->general()->rdi;
			regs[5] = executor->general()->rsi;
			regs[6] = executor->general()->r8;
			regs[7] = executor->general()->r9;
			regs[8] = executor->general()->r10;
			regs[9] = executor->general()->r11;
			regs[10] = executor->general()->r12;
			regs[11] = executor->general()->r13;
			regs[12] = executor->general()->r14;
			regs[13] = executor->general()->r15;
			regs[14] = executor->general()->rbp;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 15))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[31];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			for (int i = 0; i < 31; i++)
				regs[i] = executor->general()->x[i];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 31))
			return kHelErrFault;
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[30];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->x(1);
			// Skip x(2) as it corresponds to sp.
			for (int i = 1; i < 30; i++)
				regs[i] = executor->general()->x(i + 2);
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 30))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[2];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->clientFs;
			regs[1] = executor->general()->clientGs;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[1];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->tpidr_el0;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 1))
			return kHelErrFault;
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[1];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->tp();
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 1))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsVirtualization) {
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		HelX86VirtualizationRegs regs;
		memset(&regs, 0, sizeof(HelX86VirtualizationRegs));
		vcpu.vcpu->loadRegs(&regs);
		if(!writeUserObject(reinterpret_cast<HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
#else
		return kHelErrNoHardwareSupport;
#endif
	}else if(set == kHelRegsSimd) {
		if(!thread)
			return kHelErrIllegalArgs;

		size_t simdSize;
#if defined(__x86_64__)
		simdSize = Executor::determineSimdSize();
#elif defined(__aarch64__)
		simdSize = sizeof(FpRegisters);
#elif defined(__riscv) && __riscv_xlen == 64
		simdSize = Executor::fpStateSize;
#else
		return kHelErrUnsupportedOperation;
#endif

		frg::unique_memory<KernelAlloc> buffer{*kernelAlloc, simdSize};
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
#if defined(__x86_64__)
			memcpy(buffer.data(), executor->_fxState(), simdSize);
#elif defined(__aarch64__)
			memcpy(buffer.data(), executor->fp(), simdSize);
#elif defined(__riscv) && __riscv_xlen == 64
			memcpy(buffer.data(), executor->fpRegisters(), simdSize);
#else
			__builtin_trap(); // Should not be reached due to return above.
#endif
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserMemory(image, buffer.data(), simdSize))
			return kHelErrFault;
	}else if(set == kHelRegsSignal) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[19];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->r8;
			regs[1] = executor->general()->r9;
			regs[2] = executor->general()->r10;
			regs[3] = executor->general()->r11;
			regs[4] = executor->general()->r12;
			regs[5] = executor->general()->r13;
			regs[6] = executor->general()->r14;
			regs[7] = executor->general()->r15;
			regs[8] = executor->general()->rdi;
			regs[9] = executor->general()->rsi;
			regs[10] = executor->general()->rbp;
			regs[11] = executor->general()->rbx;
			regs[12] = executor->general()->rdx;
			regs[13] = executor->general()->rax;
			regs[14] = executor->general()->rcx;
			regs[15] = *executor->sp();
			regs[16] = *executor->ip();
			regs[17] = *executor->rflags();
			regs[18] = *executor->cs();
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 19))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[35];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->far;
			for (int i = 0; i < 31; i++)
				regs[1 + i] = executor->general()->x[i];
			regs[32] = executor->general()->sp;
			regs[33] = executor->general()->elr;
			regs[34] = executor->general()->spsr;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 35))
			return kHelErrFault;
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[32];
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			regs[0] = executor->general()->ip;
			for(int i = 1; i < 32; i++)
				regs[i] = executor->general()->x(i);
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 32))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsPageFault) {
		if(!thread)
			return kHelErrIllegalArgs;

		auto infoOutcome = thread->getInterruptInfo();
		if (!infoOutcome)
			return translateError(infoOutcome.error());

		uintptr_t regs[2];
		regs[0] = infoOutcome->offendingAddress;

		switch (infoOutcome->pageFaultType) {
			case thor::PageFaultType::NotMapped:
				regs[1] = kHelPageFaultMapError;
				break;
			case thor::PageFaultType::BadPermissions:
				regs[1] = kHelPageFaultAccessError;
				break;
			case thor::PageFaultType::None:
				regs[1] = 0;
				break;
			default:
				infoLogger() << "hel: unhandled page fault type " << frg::hex_fmt{std::to_underlying(infoOutcome->pageFaultType)} << frg::endlog;
				regs[1] = 0;
				break;
		}

		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	VirtualizedCpuDescriptor vcpu{nullptr};
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.lock();
	}else{
		auto outcome = this_universe->inspectDescriptor(handle,
				[&](AnyDescriptor &desc) -> std::expected<void, Error> {
			if(desc.is<ThreadDescriptor>()) {
				thread = smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
			}else if(desc.is<VirtualizedCpuDescriptor>()) {
				vcpu = desc.get<VirtualizedCpuDescriptor>();
			}else{
				return std::unexpected{Error::badDescriptor};
			}
			return {};
		});
		if(!outcome)
			return translateError(outcome.error());
	}

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			*executor->ip() = regs[0];
			*executor->sp() = regs[1];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[15];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 15))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->rax = regs[0];
			executor->general()->rbx = regs[1];
			executor->general()->rcx = regs[2];
			executor->general()->rdx = regs[3];
			executor->general()->rdi = regs[4];
			executor->general()->rsi = regs[5];
			executor->general()->r8 = regs[6];
			executor->general()->r9 = regs[7];
			executor->general()->r10 = regs[8];
			executor->general()->r11 = regs[9];
			executor->general()->r12 = regs[10];
			executor->general()->r13 = regs[11];
			executor->general()->r14 = regs[12];
			executor->general()->r15 = regs[13];
			executor->general()->rbp = regs[14];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__aarch64__)
		uintptr_t regs[31];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 31))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			for (int i = 0; i < 31; i++)
				executor->general()->x[i] = regs[i];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[30];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 30))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->x(1) = regs[0];
			// Skip x(2) as it corresponds to sp.
			for (int i = 1; i < 30; i++)
				executor->general()->x(i + 2) = regs[i];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[2];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->clientFs = regs[0];
			executor->general()->clientGs = regs[1];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__aarch64__)
		uintptr_t regs[1];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 1))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->tpidr_el0 = regs[0];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[1];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 1))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->tp() = regs[0];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsDebug) {
#if 0 // x86_64
		// TODO: If we want to re-enable debug registers on x86_64, we have to validate
		//       that they only affect userspace and not the kernel.
		// FIXME: Make those registers thread-specific.
		uint32_t *reg;
		if(!readUserObject(reinterpret_cast<uint32_t *const *>(image), reg))
			return kHelErrFault;
		breakOnWrite(reg);
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsVirtualization) {
#ifdef __x86_64__
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
		HelX86VirtualizationRegs regs;
		if(!readUserObject(reinterpret_cast<const HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
		vcpu.vcpu->storeRegs(&regs);
#else
		return kHelErrNoHardwareSupport;
#endif
	}else if(set == kHelRegsSimd) {
		if(!thread)
			return kHelErrIllegalArgs;

		size_t simdSize;
#if defined(__x86_64__)
		simdSize = Executor::determineSimdSize();
#elif defined(__aarch64__)
		simdSize = sizeof(FpRegisters);
#elif defined(__riscv) && __riscv_xlen == 64
		simdSize = Executor::fpStateSize;
#else
		return kHelErrUnsupportedOperation;
#endif

		frg::unique_memory<KernelAlloc> buffer{*kernelAlloc, simdSize};
		if(!readUserMemory(buffer.data(), image, simdSize))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
#if defined(__x86_64__)
			memcpy(executor->_fxState(), buffer.data(), simdSize);
#elif defined(__aarch64__)
			memcpy(executor->fp(), buffer.data(), simdSize);
#elif defined(__riscv) && __riscv_xlen == 64
			memcpy(executor->fpRegisters(), buffer.data(), simdSize);
#else
			__builtin_trap(); // Should not be reached due to return above.
#endif
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
	}else if(set == kHelRegsSignal) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[19];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 19))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->r8 = regs[0];
			executor->general()->r9 = regs[1];
			executor->general()->r10 = regs[2];
			executor->general()->r11 = regs[3];
			executor->general()->r12 = regs[4];
			executor->general()->r13 = regs[5];
			executor->general()->r14 = regs[6];
			executor->general()->r15 = regs[7];
			executor->general()->rdi = regs[8];
			executor->general()->rsi = regs[9];
			executor->general()->rbp = regs[10];
			executor->general()->rbx = regs[11];
			executor->general()->rdx = regs[12];
			executor->general()->rax = regs[13];
			executor->general()->rcx = regs[14];
			*executor->sp() = regs[15];
			*executor->ip() = regs[16];

			// Allow modifying the normal non-privileged flags.
			constexpr uintptr_t allowedFlagsMask = 0b1000011000110111111111;
			*executor->rflags() &= ~allowedFlagsMask;
			*executor->rflags() |= regs[17] & allowedFlagsMask;

			// Make sure that the cs is in usermode by or'ing it with 3.
			*executor->cs() = regs[18] | 3;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__aarch64__)
		uintptr_t regs[35];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 35))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->far = regs[0];
			for (int i = 0; i < 31; i++)
				executor->general()->x[i] = regs[1 + i];
			executor->general()->sp = regs[32];
			executor->general()->elr = regs[33];

			// Allow N, Z, C, V and SS modifications.
			constexpr uintptr_t allowedFlagsMask = 0b1111U << 28 | 1U << 21;
			executor->general()->spsr &= ~allowedFlagsMask;
			executor->general()->spsr |= regs[34] & allowedFlagsMask;
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#elif defined(__riscv) && __riscv_xlen == 64
		uintptr_t regs[32];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 32))
			return kHelErrFault;
		auto accessOutcome = thread->accessRegisters([&](Executor *executor) {
			executor->general()->ip = regs[0];
			for(int i = 1; i < 32; i++)
				executor->general()->x(i) = regs[i];
		});
		if(!accessOutcome)
			return translateError(accessOutcome.error());
#else
		return kHelErrUnsupportedOperation;
#endif
	}else {
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
#ifdef __x86_64__
	common::x86::wrmsr(common::x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
#else
	(void)pointer;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helReadFsBase(void **pointer) {
#ifdef __x86_64__
	*pointer = (void *)common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	return kHelErrNone;
#else
	(void)pointer;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helWriteGsBase(void *pointer) {
#ifdef __x86_64__
	common::x86::wrmsr(common::x86::kMsrIndexKernelGsBase, (uintptr_t)pointer);
	return kHelErrNone;
#else
	(void)pointer;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helReadGsBase(void **pointer) {
#ifdef __x86_64__
	*pointer = (void *)common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	return kHelErrNone;
#else
	(void)pointer;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helGetClock(uint64_t *counter) {
	*counter = getClockNanos();
	return kHelErrNone;
}

HelError doSubmitAwaitClock(smarter::shared_ptr<IpcQueue> queue, uint64_t counter,
		uintptr_t context, CancelGuard cg) {
	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<IpcQueue> queue, uint64_t counter, uintptr_t context,
			CancelGuard cg,
			enable_detached_coroutine) -> void {
		bool succeeded = co_await generalTimerEngine()->sleep(counter, cg.token());

		queue->unregisterTag(std::move(cg));

		HelError error = succeeded ? kHelErrNone : kHelErrCancelled;
		HelSimpleResult helResult{.error = error, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(queue), counter, context, std::move(cg),
		enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle, uint32_t attach_credentials) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto lanes = createStream(attach_credentials);
	*lane1_handle = this_universe->attachDescriptor(
			LaneDescriptor(std::move(lanes.get<0>())));
	*lane2_handle = this_universe->attachDescriptor(
			LaneDescriptor(std::move(lanes.get<1>())));

	return kHelErrNone;
}

HelError doSubmitExchangeMsgs(HelHandle laneHandle, smarter::shared_ptr<IpcQueue> queue,
		std::span<std::byte> payloadSpan,
		size_t count, uintptr_t context, uint32_t flags) {
	if(flags)
		return kHelErrIllegalArgs;
	if(!count)
		return kHelErrIllegalArgs;

	if(payloadSpan.size() < count * sizeof(HelAction)) {
		infoLogger() << "Bad length for kSubmitExchangeMsgs payload" << frg::endlog;
		return kHelErrBufferTooSmall;
	}

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto laneOutcome = thisUniverse->inspectDescriptor(laneHandle,
			[](AnyDescriptor &desc) -> std::expected<LaneHandle, Error> {
		if(!desc.is<LaneDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<LaneDescriptor>().handle;
	});
	if(!laneOutcome)
		return translateError(laneOutcome.error());
	auto lane = std::move(*laneOutcome);

	struct Item {
		HelAction recipe;
		size_t link;
		StreamNode transmit;
		QueueSource mainSource;
		QueueSource dataSource;
		union {
			HelSimpleResult helSimpleResult;
			HelHandleResult helHandleResult;
			HelCredentialsResult helCredentialsResult;
			HelInlineResultNoFlex helInlineResult;
			HelLengthResult helLengthResult;
		};
	};

	frg::dyn_array<Item, KernelAlloc> items{count, *kernelAlloc};

	// Identifies the root chain on the stack below.
	constexpr size_t noIndex = static_cast<size_t>(-1);

	// Auxiliary stack to compute the list that each item will be linked into.
	frg::small_vector<size_t, 4, KernelAlloc> linkStack{*kernelAlloc};
	linkStack.push_back(noIndex);

	// Read the message items.
	size_t ipcSize = 0;
	size_t numFlows = 0;
	for(size_t i = 0; i < count; i++) {
		HelAction *recipe = &items[i].recipe;
		auto node = &items[i].transmit;

		// Note: this is in-bounds due to the check at function entry.
		memcpy(recipe, payloadSpan.data() + i * sizeof(HelAction), sizeof(HelAction));

		switch(recipe->type) {
			case kHelActionDismiss:
				node->_tag = kTagDismiss;
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			case kHelActionOffer:
				node->_tag = kTagOffer;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			case kHelActionAccept:
				node->_tag = kTagAccept;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			case kHelActionImbueCredentials: {
				std::array<char, 16> creds;

				if(recipe->handle == kHelThisThread) {
					creds = thisThread->credentials();
				} else {
					auto credsOutcome = thisUniverse->inspectDescriptor(recipe->handle,
							[&](AnyDescriptor &desc) -> std::expected<void, Error> {
						if(desc.is<ThreadDescriptor>())
							creds = desc.get<ThreadDescriptor>().thread->credentials();
						else if(desc.is<TokenDescriptor>())
							creds = desc.get<TokenDescriptor>().credentials->credentials();
						else if(desc.is<LaneDescriptor>())
							creds = desc.get<LaneDescriptor>().handle.getStream()->credentials().credentials();
						else
							return std::unexpected{Error::badDescriptor};
						return {};
					});
					if(!credsOutcome)
						return translateError(credsOutcome.error());
				}

				node->_tag = kTagImbueCredentials;
				memcpy(node->_inCredentials.data(), creds.data(), creds.size());
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionExtractCredentials:
				node->_tag = kTagExtractCredentials;
				ipcSize += ipcSourceSize(sizeof(HelCredentialsResult));
				break;
			case kHelActionSendFromBuffer:
				if(recipe->length <= kPageSize) {
					frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, recipe->length);
					if(!readUserMemory(reinterpret_cast<char *>(buffer.data()),
							reinterpret_cast<char *>(recipe->buffer), recipe->length))
						return kHelErrFault;

					node->_tag = kTagSendKernelBuffer;
					node->_inBuffer = std::move(buffer);
				}else{
					node->_tag = kTagSendFlow;
					node->_maxLength = recipe->length;
					++numFlows;
				}
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			case kHelActionSendFromBufferSg: {
				size_t length = 0;
				frg::small_vector<HelSgItem, 8, KernelAlloc> sgItems{*kernelAlloc};
				auto sglist = reinterpret_cast<HelSgItem *>(recipe->buffer);
				for(size_t j = 0; j < recipe->length; j++) {
					HelSgItem item;
					if(!readUserObject(sglist + j, item))
						return kHelErrFault;
					if (!(frg::safe_int{length} + frg::safe_int{item.length}).into(length))
						return kHelErrIllegalArgs;
					sgItems.push_back(item);
				}

				frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, length);
				size_t offset = 0;
				for(size_t j = 0; j < recipe->length; j++) {
					const auto &item = sgItems[j];
					if(!readUserMemory(reinterpret_cast<char *>(buffer.data()) + offset,
							reinterpret_cast<char *>(item.buffer), item.length))
						return kHelErrFault;
					offset += item.length;
				}

				node->_tag = kTagSendKernelBuffer;
				node->_inBuffer = std::move(buffer);
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionRecvInline:
				// TODO: For now, we hardcode a size of 128 bytes.
				node->_tag = kTagRecvKernelBuffer;
				node->_maxLength = 128;
				ipcSize += ipcSourceSize(sizeof(HelLengthResult));
				ipcSize += ipcSourceSize(128);
				break;
			case kHelActionRecvToBuffer:
				node->_tag = kTagRecvFlow;
				node->_maxLength = recipe->length;
				++numFlows;
				ipcSize += ipcSourceSize(sizeof(HelLengthResult));
				break;
			case kHelActionPushDescriptor: {
				AnyDescriptor operand;
				auto wrapper = thisUniverse->getDescriptor(recipe->handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				operand = std::move(*wrapper);

				node->_tag = kTagPushDescriptor;
				node->_inDescriptor = std::move(operand);
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionPullDescriptor:
				node->_tag = kTagPullDescriptor;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			default:
				return kHelErrIllegalArgs;
		}

		// Items at the root must be chained.
		if(linkStack.empty())
			return kHelErrIllegalArgs;

		items[i].link = linkStack.back();

		if(!(recipe->flags & kHelItemChain))
			linkStack.pop_back();
		if(recipe->flags & kHelItemAncillary)
			linkStack.push_back(i);
	}

	// All chains must terminate properly.
	if(!linkStack.empty())
		return kHelErrIllegalArgs;

	if(!queue->validSize(ipcSize))
		return kHelErrQueueTooSmall;

	// From this point on, the function must not fail, since we now link our items
	// into intrusive linked lists.

	[](frg::dyn_array<Item, KernelAlloc> items, size_t count,
			smarter::weak_ptr<Universe> weakUniverse,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			LaneHandle lane, size_t numFlows, smarter::shared_ptr<Thread> thread,
			enable_detached_coroutine) -> void {
		StreamPacket packet;
		packet.setup(count);

		// Identifies the root chain on the stack below.
		constexpr size_t noIndex = static_cast<size_t>(-1);

		// Now, build up the messages that we submit to the stream.
		StreamList rootChain;
		for(size_t i = 0; i < count; i++) {
			// Setup the packet pointer.
			items[i].transmit._packet = &packet;

			// Link the nodes together.
			auto l = items[i].link;
			if(l == noIndex) {
				rootChain.push_back(&items[i].transmit);
			}else{
				// Add the item to an ancillary list of another item.
				items[l].transmit.ancillaryChain.push_back(&items[i].transmit);
			}
		}

		Stream::transmit(lane, rootChain);

		if(numFlows) {
			// We exit once we processed numFlows-many items.
			// This guarantees that we do not access the closure object after it is freed.
			// Below, we need to ensure that we always complete our own nodes
			// before completing peer nodes.

			// The size of this array must be a power of two.
			frg::array<frg::unique_memory<KernelAlloc>, 2> xferBuffers;

			size_t i = 0;
			size_t seenFlows = 0; // Iterates through flows.
			while(seenFlows < numFlows) {
				assert(i < count);
				auto item = &items[i++];
				auto recipe = &item->recipe;
				auto node = &item->transmit;

				if(!usesFlowProtocol(node->tag()))
					continue;
				++seenFlows;

				co_await node->issueFlow.wait();
				auto peer = node->peerNode;

				// Check for transmission errors (transmission errors or zero-size transfers).
				if(!peer) {
					node->complete();
					continue;
				}

				if(recipe->type == kHelActionSendFromBuffer
						&& node->tag() == kTagSendFlow
						&& peer->tag() == kTagRecvKernelBuffer) {
					frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, recipe->length);

					auto outcome = readUserMemory(buffer.data(), recipe->buffer, recipe->length);
					if(!outcome) {
						// We complete with fault; the remote with success.
						// TODO: it probably makes sense to introduce a "remote fault" error.
						peer->_error = Error::success;
						node->_error = Error::fault;
						peer->complete();
						node->complete();
						continue;
					}

					// Both nodes complete successfully.
					peer->_transmitBuffer = std::move(buffer);
					peer->complete();
					node->complete();
				}else if(recipe->type == kHelActionSendFromBuffer
						&& node->tag() == kTagSendFlow
						&& peer->tag() == kTagRecvFlow) {
					// Empty packets are handled by the generic stream code.
					assert(recipe->length);

					size_t progress = 0;
					size_t numSent = 0;
					size_t numAcked = 0;
					bool lastTransferSent = false;
					// Each iteration of this loop sends one transfer packet (or terminates).
					while(true) {
						bool anyRemoteFault = false;
						while(numSent != numAcked) {
							// If there is anything more to send, we only need to wait until
							// at least one buffer is not in-flight (otherwise, we wait for all).
							if(!lastTransferSent && numSent - numAcked < xferBuffers.size())
								break;
							auto ackPacket = co_await node->flowQueue.async_get();
							assert(ackPacket);
							if(ackPacket->fault)
								anyRemoteFault = true;
							++numAcked;
						}

						if(lastTransferSent) {
							if(anyRemoteFault) {
								node->_error = Error::remoteFault;
							}else{
								node->_error = Error::success;
							}
							break;
						}

						// If we encounter remote faults, we terminate.
						if(anyRemoteFault) {
							// Send the packet (may deallocate the peer!).
							peer->flowQueue.put({ .terminate = true });
							++numSent;

							// Retrieve but ignore all acks.
							assert(numSent > numAcked);
							while(numSent != numAcked) {
								auto ackPacket = co_await node->flowQueue.async_get();
								assert(ackPacket);
								++numAcked;
							}

							node->_error = Error::remoteFault;
							break;
						}

						// Prepare a buffer an send it.
						assert(numSent - numAcked < xferBuffers.size());
						auto &xb = xferBuffers[numSent & (xferBuffers.size() - 1)];
						if(!xb.size())
							xb = frg::unique_memory<KernelAlloc>{*kernelAlloc, 4096};

						auto chunkSize = frg::min(recipe->length - progress, xb.size());
						assert(chunkSize);

						auto outcome = readUserMemory(xb.data(),
								reinterpret_cast<std::byte *>(recipe->buffer) + progress, chunkSize);
						if(!outcome) {
							// Send the packet (may deallocate the peer!).
							peer->flowQueue.put({ .terminate = true, .fault = true });
							++numSent;

							// Retrieve but ignore all acks.
							assert(numSent > numAcked);
							while(numSent != numAcked) {
								auto ackPacket = co_await node->flowQueue.async_get();
								assert(ackPacket);
								++numAcked;
							}

							node->_error = Error::fault;
							break;
						}

						lastTransferSent = (progress + chunkSize == recipe->length);
						// Send the packet (may deallocate the peer!).
						peer->flowQueue.put({
							.data = xb.data(),
							.size = chunkSize,
							.terminate = lastTransferSent
						});
						++numSent;
						progress += chunkSize;
					}

					node->complete();
				}else if(recipe->type == kHelActionRecvToBuffer
						&& peer->tag() == kTagSendKernelBuffer) {
					auto outcome = writeUserMemory(recipe->buffer,
							peer->_inBuffer.data(), peer->_inBuffer.size());
					if(!outcome) {
						// We complete with fault; the remote with success.
						// TODO: it probably makes sense to introduce a "remote fault" error.
						peer->_error = Error::success;
						node->_error = Error::fault;
						peer->complete();
						node->complete();
						continue;
					}

					// Both nodes complete successfully.
					node->_actualLength = peer->_inBuffer.size();
					peer->complete();
					node->complete();
				}else{
					assert(recipe->type == kHelActionRecvToBuffer
							&& peer->tag() == kTagSendFlow);

					size_t progress = 0;
					bool didFault = false;
					// Each iteration of this loop sends one ack packet.
					while(true) {
						auto xferPacket = co_await node->flowQueue.async_get();
						assert(xferPacket);

						if(xferPacket->data && !didFault) {
							// Otherwise, there would have been a transmission error.
							assert(progress + xferPacket->size <= recipe->length);

							auto outcome = writeUserMemory(
									reinterpret_cast<std::byte *>(recipe->buffer) + progress,
									xferPacket->data, xferPacket->size);
							if(outcome) {
								progress += xferPacket->size;
							}else{
								didFault = true;
							}
						}

						if(xferPacket->terminate) {
							if(didFault) {
								// Ack the packet (may deallocate the peer!).
								peer->flowQueue.put({ .terminate = true, .fault = true, });
								node->_error = Error::fault;
							}else{
								// Ack the packet (may deallocate the peer!).
								peer->flowQueue.put({ .terminate = true });
								if(xferPacket->fault) {
									node->_error = Error::remoteFault;
								}else{
									node->_actualLength = progress;
								}
							}

							// This node is finished.
							break;
						}

						// This is the only non-terminating case.
						// Senders should always terminate if they fault.
						assert(!xferPacket->fault);

						// Ack the packet (may deallocate the peer!).
						if(didFault) {
							peer->flowQueue.put({ .fault = true });
						}else{
							peer->flowQueue.put({});
						}
					}

					node->complete();
				}
			}
		}

		co_await packet.completion.wait();

		QueueSource *tail = nullptr;
		auto link = [&] (QueueSource *source) {
			if(tail)
				tail->link = source;
			tail = source;
		};

		for(size_t i = 0; i < count; i++) {
			auto item = &items[i];
			HelAction *recipe = &item->recipe;
			auto node = &item->transmit;

			if(recipe->type == kHelActionDismiss) {
				item->helSimpleResult = {translateError(node->error()), 0};
				item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionOffer) {
				HelHandle handle = kHelNullHandle;

				if(node->error() == Error::success
						&& (recipe->flags & kHelItemWantLane)) {
					auto universe = weakUniverse.lock();
					if (!universe) {
						item->helHandleResult = {kHelErrBadDescriptor, 0, handle};
						item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
						link(&item->mainSource);
						continue;
					}
					assert(universe);

					handle = universe->attachDescriptor(
							LaneDescriptor{node->lane()});
				}

				item->helHandleResult = {translateError(node->error()), 0, handle};
				item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionAccept) {
				// TODO: This condition should be replaced. Just test if lane is valid.
				HelHandle handle = kHelNullHandle;
				if(node->error() == Error::success) {
					auto universe = weakUniverse.lock();
					assert(universe);

					handle = universe->attachDescriptor(
							LaneDescriptor{node->lane()});
				}

				item->helHandleResult = {translateError(node->error()), 0, handle};
				item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionImbueCredentials) {
				item->helSimpleResult = {translateError(node->error()), 0};
				item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionExtractCredentials) {
				item->helCredentialsResult = {.error = translateError(node->error()), .reserved = {}, .credentials = {}};
				memcpy(item->helCredentialsResult.credentials,
						node->credentials().data(), 16);
				item->mainSource.setup(&item->helCredentialsResult,
						sizeof(HelCredentialsResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionSendFromBuffer
					|| recipe->type == kHelActionSendFromBufferSg) {
				item->helSimpleResult = {translateError(node->error()), 0};
				item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionRecvInline) {
				item->helInlineResult = {translateError(node->error()),
						0, node->_transmitBuffer.size()};
				item->mainSource.setup(&item->helInlineResult, sizeof(HelInlineResultNoFlex));
				item->dataSource.setup(node->_transmitBuffer.data(),
						node->_transmitBuffer.size());
				link(&item->mainSource);
				link(&item->dataSource);
			}else if(recipe->type == kHelActionRecvToBuffer) {
				item->helLengthResult = {translateError(node->error()),
						0, node->actualLength()};
				item->mainSource.setup(&item->helLengthResult, sizeof(HelLengthResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionPushDescriptor) {
				item->helSimpleResult = {translateError(node->error()), 0};
				item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
				link(&item->mainSource);
			}else if(recipe->type == kHelActionPullDescriptor) {
				// TODO: This condition should be replaced. Just test if lane is valid.
				HelHandle handle = kHelNullHandle;
				if(node->error() == Error::success) {
					auto universe = weakUniverse.lock();
					assert(universe);

					handle = universe->attachDescriptor(node->descriptor());
				}

				item->helHandleResult = {translateError(node->error()), 0, handle};
				item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
				link(&item->mainSource);
			}else{
				// This cannot happen since we validate recipes at submit time.
				__builtin_trap();
			}
		}

		co_await queue->submit(&items[0].mainSource, context);
	}(std::move(items), count, thisUniverse.lock(), std::move(queue), context,
			lane, numFlows, thisThread.lock(),
			enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});

	return kHelErrNone;
}

HelError helShutdownLane(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto laneOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<LaneHandle, Error> {
		if(!desc.is<LaneDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<LaneDescriptor>().handle;
	});
	if(!laneOutcome)
		return translateError(laneOutcome.error());
	auto lane = std::move(*laneOutcome);

	lane.getStream()->shutdownLane(lane.getLane());

	return kHelErrNone;
}

HelError helFutexWait(int *pointer, int expected, int64_t deadline) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace();
	auto address = reinterpret_cast<uintptr_t>(pointer);

	if(deadline < 0) {
		if(deadline != -1)
			return kHelErrIllegalArgs;

		return translateError(Thread::asyncBlockCurrentInterruptible(
			async::lambda([&](async::cancellation_token ct) {
				return getGlobalFutexRealm()->wait(
					space->globalFutexSpace(), address, expected, ct
				);
			}),
			thisThread->pagingWorkQueue().get()
		));
	}else{
		Error waitErr;
		bool timeout = false;

		Thread::asyncBlockCurrentInterruptible(async::lambda([&](async::cancellation_token ct) {
			return async::race_and_cancel(
			    async::lambda([&](async::cancellation_token cancellation) -> coroutine<void> {
				    waitErr = co_await getGlobalFutexRealm()->wait(
				        space->globalFutexSpace(), address, expected, cancellation
				    );
			    }),
			    async::lambda([&](async::cancellation_token cancellation) -> coroutine<void> {
				    timeout = co_await generalTimerEngine()->sleep(deadline, cancellation);
			    }),
			    async::lambda([ct](async::cancellation_token cancellation) {
				    return async::suspend_indefinitely(ct, cancellation);
			    })
			);
		}), thisThread->pagingWorkQueue().get());

		if (waitErr == Error::cancelled)
			return timeout ? kHelErrTimeout : kHelErrCancelled;

		return translateError(waitErr);
	}

	return kHelErrNone;
}

HelError helFutexWake(int *pointer, unsigned int count) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace();
	auto address = reinterpret_cast<uintptr_t>(pointer);

	auto result = Thread::asyncBlockCurrent(
		getGlobalFutexRealm()->wake(
			space->globalFutexSpace(), address, count
		),
		thisThread->pagingWorkQueue().get()
	);
	if(!result)
		return kHelErrFault;

	return kHelErrNone;
}

HelError helCreateOneshotEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = smarter::allocate_shared<OneshotEvent>(*kernelAlloc);

	*handle = this_universe->attachDescriptor(
			OneshotEventDescriptor(std::move(event)));

	return kHelErrNone;
}

HelError helCreateBitsetEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = smarter::allocate_shared<BitsetEvent>(*kernelAlloc);

	*handle = this_universe->attachDescriptor(
			BitsetEventDescriptor(std::move(event)));

	return kHelErrNone;
}

HelError helRaiseEvent(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto eventOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<OneshotEvent>, Error> {
		if(!desc.is<OneshotEventDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<OneshotEventDescriptor>().event;
	});
	if(!eventOutcome)
		return translateError(eventOutcome.error());

	auto outcome = (*eventOutcome)->trigger();
	if(!outcome)
		return translateError(outcome.error());

	return kHelErrNone;
}

HelError helAccessIrq(int number, HelHandle *handle) {
#ifdef __x86_64__
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto pin = acpi::getGlobalSystemIrq(number);
	if (!pin)
		return kHelErrOutOfBounds;

	auto irq = smarter::allocate_shared<GenericIrqObject>(*kernelAlloc,
			frg::string<KernelAlloc>{*kernelAlloc, "generic-irq-object"});
	IrqPin::attachSink(pin, irq.get());

	*handle = this_universe->attachDescriptor(
			IrqDescriptor(std::move(irq)));

	return kHelErrNone;
#else
	(void)number;
	(void)handle;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	if(flags & ~(kHelAckAcknowledge | kHelAckNack | kHelAckKick | kHelAckClear))
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto mode = flags & (kHelAckAcknowledge | kHelAckNack | kHelAckKick);
	if(mode != kHelAckAcknowledge && mode != kHelAckNack && mode != kHelAckKick)
		return kHelErrIllegalArgs;

	auto irqOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IrqObject>, Error> {
		if(!desc.is<IrqDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<IrqDescriptor>().irq;
	});
	if(!irqOutcome)
		return translateError(irqOutcome.error());
	auto irq = std::move(*irqOutcome);

	Error error;
	if(mode == kHelAckAcknowledge) {
		error = IrqPin::ackSink(irq.get(), sequence);
	}else if(mode == kHelAckNack) {
		error = IrqPin::nackSink(irq.get(), sequence);
	}else{
 		assert(mode == kHelAckKick);
		error = IrqPin::kickSink(irq.get(), flags & kHelAckClear);
	}

	if(error == Error::illegalArgs) {
		return kHelErrIllegalArgs;
	}else{
		assert(error == Error::success);
		return kHelErrNone;
	}
}

HelError doSubmitAwaitEvent(HelHandle handle, smarter::shared_ptr<IpcQueue> queue,
		uint64_t sequence, uintptr_t context,
		CancelGuard cg) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	AnyDescriptor descriptor;
	auto wrapper = this_universe->getDescriptor(handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	descriptor = std::move(*wrapper);

	if(!queue->validSize(ipcSourceSize(sizeof(HelEventResult))))
		return kHelErrQueueTooSmall;

	if(descriptor.is<IrqDescriptor>()) {
		auto irq = descriptor.get<IrqDescriptor>().irq;

		[](smarter::shared_ptr<IrqObject> irq, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
				CancelGuard cg,
				enable_detached_coroutine edc) -> void {
			auto result = co_await irq->awaitIrq(sequence, edc.wq.get());

			queue->unregisterTag(std::move(cg));

			HelEventResult helResult{
				.error = kHelErrNone,
				.bitset = 0,
				.sequence = 0,
			};
			if(result) {
				helResult.sequence = result.value();
			} else {
				helResult.error = translateError(result.error());
			}
			QueueSource ipcSource{&helResult, sizeof(HelEventResult), nullptr};
			co_await queue->submit(&ipcSource, context);
		}(std::move(irq), sequence, std::move(queue), context, std::move(cg),
			enable_detached_coroutine{this_thread->mainWorkQueue().lock()});
	}else if(descriptor.is<OneshotEventDescriptor>()) {
		auto event = descriptor.get<OneshotEventDescriptor>().event;

		[](smarter::shared_ptr<OneshotEvent> event, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
				CancelGuard cg,
				enable_detached_coroutine edc) -> void {
			auto result = co_await event->awaitEvent(sequence, cg.token(), edc.wq.get());

			queue->unregisterTag(std::move(cg));

			HelEventResult helResult{
				.error = translateError(result.error),
				.bitset = result.bitset,
				.sequence = result.sequence,
			};
			QueueSource ipcSource{&helResult, sizeof(HelEventResult), nullptr};
			co_await queue->submit(&ipcSource, context);
		}(std::move(event), sequence, std::move(queue), context, std::move(cg),
				enable_detached_coroutine{this_thread->mainWorkQueue().lock()});
	}else if(descriptor.is<BitsetEventDescriptor>()) {
		auto event = descriptor.get<BitsetEventDescriptor>().event;

		[](smarter::shared_ptr<BitsetEvent> event, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
				CancelGuard cg,
				enable_detached_coroutine edc) -> void {
			auto result = co_await event->awaitEvent(sequence, cg.token(), edc.wq.get());

			queue->unregisterTag(std::move(cg));

			HelEventResult helResult{
				.error = translateError(result.error),
				.bitset = result.bitset,
				.sequence = result.sequence,
			};
			QueueSource ipcSource{&helResult, sizeof(HelEventResult), nullptr};
			co_await queue->submit(&ipcSource, context);
		}(std::move(event), sequence, std::move(queue), context, std::move(cg),
				enable_detached_coroutine{this_thread->mainWorkQueue().lock()});
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet_handle) {
	if (flags)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irqOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IrqObject>, Error> {
		if(!desc.is<IrqDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<IrqDescriptor>().irq;
	});
	if(!irqOutcome)
		return translateError(irqOutcome.error());
	auto irq = std::move(*irqOutcome);

	auto kernletOutcome = this_universe->inspectDescriptor(kernlet_handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<BoundKernlet>, Error> {
		if(!desc.is<BoundKernletDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<BoundKernletDescriptor>().boundKernlet;
	});
	if(!kernletOutcome)
		return translateError(kernletOutcome.error());
	auto kernlet = std::move(*kernletOutcome);

	irq->automate(std::move(kernlet));

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto io_space = smarter::allocate_shared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++) {
		uintptr_t port;
		if(!readUserObject<uintptr_t>(port_array + i, port))
			return kHelErrFault;
		if (auto outcome = io_space->addPort(port); !outcome)
			return translateError(outcome.error());
	}

	*handle = this_universe->attachDescriptor(
			IoDescriptor(std::move(io_space)));

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
#ifdef THOR_ARCH_SUPPORTS_PIO
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto ioOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<IoSpace>, Error> {
		if(!desc.is<IoDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<IoDescriptor>().ioSpace;
	});
	if(!ioOutcome)
		return translateError(ioOutcome.error());
	auto io_space = std::move(*ioOutcome);

	io_space->enableInThread(this_thread);

	return kHelErrNone;
#else
	(void)handle;
	return kHelErrUnsupportedOperation;
#endif
}

HelError helEnableFullIo() {
#ifdef THOR_ARCH_SUPPORTS_PIO
	auto this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helBindKernlet(HelHandle handle, const HelKernletData *data, size_t num_data,
		HelHandle *bound_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto kernletOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<KernletObject>, Error> {
		if(!desc.is<KernletObjectDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return desc.get<KernletObjectDescriptor>().kernletObject;
	});
	if(!kernletOutcome)
		return translateError(kernletOutcome.error());
	auto kernlet = std::move(*kernletOutcome);

	auto object = kernlet.get();
	if (num_data != object->numberOfBindParameters())
		return kHelErrIllegalArgs;

	auto bound = smarter::allocate_shared<BoundKernlet>(*kernelAlloc,
			std::move(kernlet));
	for(size_t i = 0; i < object->numberOfBindParameters(); i++) {
		const auto &defn = object->defnOfBindParameter(i);

		HelKernletData d;
		if(!readUserObject(data + i, d))
			return kHelErrFault;

		if(defn.type == KernletParameterType::offset) {
			bound->setupOffsetBinding(i, d.handle);
		}else if(defn.type == KernletParameterType::memoryView) {
			auto memoryOutcome = this_universe->inspectDescriptor(d.handle,
					[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<MemoryView>, Error> {
				if(!desc.is<MemoryViewDescriptor>())
					return std::unexpected{Error::badDescriptor};
				return desc.get<MemoryViewDescriptor>().memory;
			});
			if(!memoryOutcome)
				return translateError(memoryOutcome.error());
			auto memory = std::move(*memoryOutcome);

			auto memorySize = memory->getLength();
			if (memorySize > 0x10000)
				return kHelErrIllegalArgs;

			if(auto e = memory->lockRange(0, memorySize); e != Error::success)
				return translateError(e);

			auto touchOutcome = Thread::asyncBlockCurrent(
					memory->touchFullRange(0, memorySize, fetchNone),
					this_thread->pagingWorkQueue().get());
			if(!touchOutcome) {
				memory->unlockRange(0, memorySize);
				return translateError(touchOutcome.error());
			}

			auto window = reinterpret_cast<char *>(KernelVirtualMemory::global().allocate(0x10000));

			for(size_t off = 0; off < memorySize; off += kPageSize) {
				auto range = memory->peekRange(off, fetchNone);
				assert(range.physical != PhysicalAddr(-1));
				PageFlags pageFlags = page_access::read;
				if (range.isMutable)
					pageFlags |= page_access::write;
				KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(window + off),
						range.physical, pageFlags, range.cachingMode);
			}

			bound->setupMemoryViewBinding(i, window);
		}else{
			assert(defn.type == KernletParameterType::bitsetEvent);

			auto eventOutcome = this_universe->inspectDescriptor(d.handle,
					[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<BitsetEvent>, Error> {
				if(!desc.is<BitsetEventDescriptor>())
					return std::unexpected{Error::badDescriptor};
				return desc.get<BitsetEventDescriptor>().event;
			});
			if(!eventOutcome)
				return translateError(eventOutcome.error());
			auto event = std::move(*eventOutcome);

			bound->setupBitsetEventBinding(i, std::move(event));
		}
	}

	*bound_handle = this_universe->attachDescriptor(
			BoundKernletDescriptor(std::move(bound)));

	return kHelErrNone;
}

HelError helGetAffinity(HelHandle handle, uint8_t *mask, size_t size, size_t *actualSize) {
	auto maskSize = LbControlBlock::affinityMaskSize();
	if(size < maskSize)
		return kHelErrBufferTooSmall;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto threadOutcome = this_universe->inspectDescriptor(handle,
			[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
		if(!desc.is<ThreadDescriptor>())
			return std::unexpected{Error::badDescriptor};
		return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
	});
	if(!threadOutcome)
		return translateError(threadOutcome.error());
	auto thread = std::move(*threadOutcome);

	frg::vector<uint8_t, KernelAlloc> buf{*kernelAlloc};
	buf.resize(maskSize);
	thread->_lbCb->getAffinityMask({buf.data(), maskSize});

	size_t used_size = size > buf.size() ? buf.size() : size;

	if (!writeUserArray(mask, buf.data(), used_size))
		return kHelErrFault;

	if (actualSize != nullptr)
		if (!writeUserObject<size_t>(actualSize, used_size))
			return kHelErrFault;

	return kHelErrNone;
}

HelError helSetAffinity(HelHandle handle, uint8_t *mask, size_t size) {
	auto maskSize = LbControlBlock::affinityMaskSize();
	if (size > maskSize)
		return kHelErrOutOfBounds;

	frg::vector<uint8_t, KernelAlloc> buf{*kernelAlloc};
	buf.resize(maskSize);
	if (!readUserArray(mask, buf.data(), size))
		return kHelErrFault;

	size_t n = 0;
	for (auto i : buf) {
		n += __builtin_popcount(i);
	}

	if (n < 1) {
		return kHelErrIllegalArgs;
	}

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(handle == kHelThisThread) {
		this_thread->_lbCb->setAffinityMask({buf.data(), maskSize});
		Thread::migrateCurrent();
	} else {
		auto threadOutcome = this_universe->inspectDescriptor(handle,
				[](AnyDescriptor &desc) -> std::expected<smarter::shared_ptr<Thread>, Error> {
			if(!desc.is<ThreadDescriptor>())
				return std::unexpected{Error::badDescriptor};
			return smarter::rc_policy_downcast<smarter::default_rc_policy>(desc.get<ThreadDescriptor>().thread);
		});
		if(!threadOutcome)
			return translateError(threadOutcome.error());
		auto thread = std::move(*threadOutcome);

		thread->_lbCb->setAffinityMask({buf.data(), maskSize});
		infoLogger() << "thor: TODO: helSetAffinity does not migrate other threads!" << frg::endlog;
	}

	return kHelErrNone;
}

HelError helQueryRegisterInfo(int set, HelRegisterInfo *info) {
	HelRegisterInfo outInfo;

	switch (set) {
		case kHelRegsProgram:
			outInfo.setSize = 2 * sizeof(uintptr_t);
			break;

		case kHelRegsGeneral:
#if defined (__x86_64__)
			static_assert(kHelNumGprs == 15);
			outInfo.setSize = 15 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			static_assert(kHelNumGprs == 31);
			outInfo.setSize = 31 * sizeof(uintptr_t);
#elif defined (__riscv) && __riscv_xlen == 64
			static_assert(kHelNumGprs == 30);
			outInfo.setSize = 30 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

		case kHelRegsThread:
#if defined (__x86_64__)
			outInfo.setSize = 2 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			outInfo.setSize = 1 * sizeof(uintptr_t);
#elif defined (__riscv) && __riscv_xlen == 64
			outInfo.setSize = 1 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

#if defined (__x86_64__)
		case kHelRegsVirtualization:
			outInfo.setSize = sizeof(HelX86VirtualizationRegs);
			break;
#endif

		case kHelRegsSimd:
#if defined (__x86_64__)
			outInfo.setSize = Executor::determineSimdSize();
#elif defined (__aarch64__)
			outInfo.setSize = sizeof(FpRegisters);
#elif defined (__riscv) && __riscv_xlen == 64
			outInfo.setSize = Executor::fpStateSize;
#else
#			error Unknown architecture
#endif
			break;

		case kHelRegsSignal:
#if defined (__x86_64__)
			outInfo.setSize = 19 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			outInfo.setSize = 35 * sizeof(uintptr_t);
#elif defined (__riscv) && __riscv_xlen == 64
			// PC + 31 GP registers (X0 is constant zero, no need to save it).
			outInfo.setSize = 32 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

		default:
			return kHelErrIllegalArgs;
	}

	if (!writeUserObject(info, outInfo))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helGetCurrentCpu(int *cpu) {
	*cpu = getCpuData()->cpuIndex;
	return kHelErrNone;
}

HelError helCreateToken(HelHandle *handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto creds = smarter::allocate_shared<Credentials>(*kernelAlloc);

	*handle = thisUniverse->attachDescriptor(
			TokenDescriptor(std::move(creds)));

	return kHelErrNone;
}

// Called from IpcQueue::processSq() to handle SQ elements.
void thor::submitFromSq(smarter::shared_ptr<IpcQueue> queue, uint32_t opcode,
		std::span<std::byte> sqSpan, uintptr_t context) {
	HelError error;
	switch(opcode) {
	case kHelSubmitCancel: {
		if(sqSpan.size() < sizeof(HelSqCancel)) {
			infoLogger() << "Bad length for kSubmitCancel" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqCancel sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		queue->cancel(sqData.cancellationTag);
		error = kHelErrNone;
		break;
	}
	case kHelSubmitAsyncNop:
		error = doSubmitAsyncNop(queue, context);
		break;
	case kHelSubmitExchangeMsgs: {
		if(sqSpan.size() < sizeof(HelSqExchangeMsgs)) {
			infoLogger() << "Bad length for kSubmitExchangeMsgs" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqExchangeMsgs sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitExchangeMsgs(sqData.lane, queue,
				sqSpan.subspan(sizeof(HelSqExchangeMsgs)),
				sqData.count, context, sqData.flags);
		break;
	}
	case kHelSubmitAwaitClock: {
		if(sqSpan.size() < sizeof(HelSqAwaitClock)) {
			infoLogger() << "Bad length for kSubmitAwaitClock" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqAwaitClock sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		auto cg = queue->registerTag(sqData.cancellationTag);
		error = doSubmitAwaitClock(queue, sqData.counter, context, std::move(cg));
		break;
	}
	case kHelSubmitAwaitEvent: {
		if(sqSpan.size() < sizeof(HelSqAwaitEvent)) {
			infoLogger() << "Bad length for kHelSubmitAwaitEvent" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqAwaitEvent sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		auto cg = queue->registerTag(sqData.cancellationTag);
		error = doSubmitAwaitEvent(sqData.handle, queue, sqData.sequence, context, std::move(cg));
		break;
	}
	case kHelSubmitProtectMemory: {
		if(sqSpan.size() < sizeof(HelSqProtectMemory)) {
			infoLogger() << "Bad length for kHelSubmitProtectMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqProtectMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitProtectMemory(sqData.spaceHandle, queue,
				sqData.pointer, sqData.size, sqData.flags, context);
		break;
	}
	case kHelSubmitSynchronizeSpace: {
		if(sqSpan.size() < sizeof(HelSqSynchronizeSpace)) {
			infoLogger() << "Bad length for kHelSubmitSynchronizeSpace" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqSynchronizeSpace sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitSynchronizeSpace(sqData.spaceHandle, queue,
				sqData.pointer, sqData.size, context);
		break;
	}
	case kHelSubmitReadMemory: {
		if(sqSpan.size() < sizeof(HelSqReadMemory)) {
			infoLogger() << "Bad length for kHelSubmitReadMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqReadMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitReadMemory(sqData.handle, queue,
				sqData.address, sqData.length, sqData.buffer, context);
		break;
	}
	case kHelSubmitWriteMemory: {
		if(sqSpan.size() < sizeof(HelSqWriteMemory)) {
			infoLogger() << "Bad length for kHelSubmitWriteMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqWriteMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitWriteMemory(sqData.handle, queue,
				sqData.address, sqData.length, sqData.buffer, context);
		break;
	}
	case kHelSubmitManageMemory: {
		if(sqSpan.size() < sizeof(HelSqManageMemory)) {
			infoLogger() << "Bad length for kHelSubmitManageMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqManageMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitManageMemory(sqData.handle, queue, context);
		break;
	}
	case kHelSubmitLockMemoryView: {
		if(sqSpan.size() < sizeof(HelSqLockMemoryView)) {
			infoLogger() << "Bad length for kHelSubmitLockMemoryView" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqLockMemoryView sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitLockMemoryView(sqData.handle, queue,
				sqData.offset, sqData.size, context);
		break;
	}
	case kHelSubmitObserve: {
		if(sqSpan.size() < sizeof(HelSqObserve)) {
			infoLogger() << "Bad length for kHelSubmitObserve" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqObserve sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitObserve(sqData.handle, queue, context);
		break;
	}
	case kHelSubmitResizeMemory: {
		if(sqSpan.size() < sizeof(HelSqResizeMemory)) {
			infoLogger() << "Bad length for kHelSubmitResizeMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqResizeMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitResizeMemory(sqData.handle, queue, sqData.newSize, context);
		break;
	}
	case kHelSubmitForkMemory: {
		if(sqSpan.size() < sizeof(HelSqForkMemory)) {
			infoLogger() << "Bad length for kHelSubmitForkMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqForkMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitForkMemory(sqData.handle, queue, context);
		break;
	}
	case kHelSubmitWritebackFence: {
		if(sqSpan.size() < sizeof(HelSqWritebackFence)) {
			infoLogger() << "Bad length for kHelSubmitWritebackFence" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqWritebackFence sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitWritebackFence(sqData.handle, queue, sqData.offset, sqData.size, context);
		break;
	}
	case kHelSubmitInvalidateMemory: {
		if(sqSpan.size() < sizeof(HelSqInvalidateMemory)) {
			infoLogger() << "Bad length for kHelSubmitInvalidateMemory" << frg::endlog;
			error = kHelErrBufferTooSmall;
			break;
		}
		HelSqInvalidateMemory sqData;
		memcpy(&sqData, sqSpan.data(), sizeof(sqData));
		error = doSubmitInvalidateMemory(sqData.handle, queue, sqData.offset, sqData.size, context);
		break;
	}
	default:
		error = kHelErrIllegalSyscall;
		infoLogger() << "thor: Bad opcode " << opcode << " in submission queue" << frg::endlog;
	}

	if (error) {
		// Right now, we are emitting a CQ element with context set to ~0 on submission failures.
		// TODO: Return the correct context but use the HelElement opcode field to distinguish
		//       submission failures and genuine completions.
		infoLogger() << "thor: Submission failure with error: " << error << frg::endlog;
		[] (smarter::shared_ptr<IpcQueue> queue, HelError error, enable_detached_coroutine) -> void {
			HelSimpleResult helResult{.error = error, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, ~uintptr_t{0});
		}(std::move(queue), error,
			enable_detached_coroutine{getCurrentThread()->mainWorkQueue().lock()});
	}
}
