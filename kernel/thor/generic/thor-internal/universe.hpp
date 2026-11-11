#pragma once

#include <expected>
#include <optional>
#include <stdint.h>
#include <type_traits>
#include <utility>
#include <frg/optional.hpp>
#include <frg/tuple.hpp>
#include <assert.h>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/rcu-base.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor {

typedef int64_t Handle;

struct MemoryView;
struct SwapSpace;
struct AddressSpace;
struct IoSpace;
struct Thread;
struct Universe;
struct IpcQueue;
struct MemorySlice;
struct NamedMemoryViewLock;
struct KernletObject;
struct BoundKernlet;
struct TokenObject;
struct DmaSpace;
struct Iommu;
struct IrqPin;
struct IrqObject;
struct OneshotEvent;
struct BitsetEvent;
struct SequencedEvent;
struct Hierarchy;
#ifdef THOR_HAS_KCOV
struct KcovBuffer;
#endif

inline bool checkRights(uint32_t rights, uint32_t requiredRights) {
	return (rights & requiredRights) == requiredRights;
}

// --------------------------------------------------------
// Objects with two peers.
// --------------------------------------------------------

struct TwoPeerPolicy;

// Base for objects that have two separately counted peers (e.g., the two lanes of a stream)
// such that each peer notices when the other one is gone.
// Handles to a peer use TwoPeerPolicy, i.e., they manipulate the peer's counter.
struct TwoPeerObject {
	// Takes over the reference to the object and returns handles to both peers (in order).
	template<typename T>
	static frg::tuple<
		smarter::shared_ptr<T, TwoPeerPolicy>,
		smarter::shared_ptr<T, TwoPeerPolicy>
	> adoptPeers(smarter::shared_ptr<T> object);

	smarter::counter &peerCounter(int peer) {
		return peerCount_[peer];
	}

	// Called after the counter of a peer reached zero.
	// Drops the reference that the peer holds on the object.
	void handlePeersZero(int peer) {
		onPeersZero(peer);
		objectRef_.decrement();
	}

protected:
	TwoPeerObject() {
		peerCount_[0].setup(smarter::adopt_rc, 1);
		peerCount_[1].setup(smarter::adopt_rc, 1);
	}

	~TwoPeerObject() = default;

	virtual void onPeersZero(int peer) = 0;

private:
	smarter::counter peerCount_[2];
	// Each peer holds a reference to the object through this policy.
	smarter::default_rc_policy objectRef_;
};

// Refcount policy for smarter::shared_ptr.
// A handle is a TwoPeerObject pointer plus the peer that it refers to.
// The refcount that it manipulates is the peer's counter.
struct TwoPeerPolicy {
	TwoPeerPolicy() = default;

	TwoPeerPolicy(TwoPeerObject *object, int peer)
	: object_{object}, peer_{peer} { }

	explicit operator bool () const {
		return object_;
	}

	void increment() const {
		object_->peerCounter(peer_).increment();
	}

	void decrement() const {
		if(object_->peerCounter(peer_).decrement_and_check_if_zero())
			object_->handlePeersZero(peer_);
	}

	TwoPeerObject *object() const {
		return object_;
	}

	int peer() const {
		return peer_;
	}

private:
	TwoPeerObject *object_ = nullptr;
	int peer_ = -1;
};
static_assert(smarter::rc_policy<TwoPeerPolicy>);

template<typename T>
frg::tuple<
	smarter::shared_ptr<T, TwoPeerPolicy>,
	smarter::shared_ptr<T, TwoPeerPolicy>
> TwoPeerObject::adoptPeers(smarter::shared_ptr<T> object) {
	T *ptr = object.get();
	TwoPeerObject *base = ptr;
	base->objectRef_ = object.policy();
	base->objectRef_.increment();
	object.release();
	return frg::make_tuple(
		smarter::shared_ptr<T, TwoPeerPolicy>{smarter::adopt_rc, ptr, TwoPeerPolicy{ptr, 0}},
		smarter::shared_ptr<T, TwoPeerPolicy>{smarter::adopt_rc, ptr, TwoPeerPolicy{ptr, 1}}
	);
}

// --------------------------------------------------------
// Lane handles.
// --------------------------------------------------------

struct StreamControl;
struct Stream;

using LanePolicy = TwoPeerPolicy;

// Extracts the numeric lane index of a lane handle.
inline int laneOf(const smarter::shared_ptr<Stream, LanePolicy> &lane) {
	return lane.policy().peer();
}

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

enum class DescriptorType : uint8_t {
	none,
	universe,
	queue,
	memoryView,
	memorySlice,
	swapSpace,
	addressSpace,
	virtualizedSpace,
	dmaSpace,
	iommu,
	virtualizedCpu,
	memoryViewLock,
	thread,
	lane,
	irqPin,
	irq,
	oneshotEvent,
	bitsetEvent,
	sequencedEvent,
	io,
	kernletObject,
	boundKernlet,
	token,
	hierarchy,
#ifdef THOR_HAS_KCOV
	kcovBuffer,
#endif
};

// Maps a descriptor type to the object type and the smarter::shared_ptr refcount
// policy that a descriptor of that type holds.
template<DescriptorType K>
struct DescriptorTraits;

template<>
struct DescriptorTraits<DescriptorType::universe> {
	using Object = Universe;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::queue> {
	using Object = IpcQueue;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memoryView> {
	using Object = MemoryView;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memorySlice> {
	using Object = MemorySlice;
	using Policy = smarter::default_rc_policy;
};

// Note that this is distinct from the memoryView descriptor of the swap space's
// BackingMemory: allocation calls need the concrete SwapSpace (a generic
// MemoryView cannot be downcast).
template<>
struct DescriptorTraits<DescriptorType::swapSpace> {
	using Object = SwapSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::addressSpace> {
	using Object = AddressSpace;
	using Policy = BindableHandle;
};

template<>
struct DescriptorTraits<DescriptorType::virtualizedSpace> {
	using Object = VirtualizedPageSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::dmaSpace> {
	using Object = DmaSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::iommu> {
	using Object = Iommu;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::virtualizedCpu> {
	using Object = VirtualizedCpu;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memoryViewLock> {
	using Object = NamedMemoryViewLock;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::thread> {
	using Object = Thread;
	using Policy = ActiveHandle;
};

template<>
struct DescriptorTraits<DescriptorType::lane> {
	using Object = Stream;
	using Policy = LanePolicy;
};

template<>
struct DescriptorTraits<DescriptorType::irqPin> {
	using Object = IrqPin;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::irq> {
	using Object = IrqObject;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::oneshotEvent> {
	using Object = OneshotEvent;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::bitsetEvent> {
	using Object = BitsetEvent;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::sequencedEvent> {
	using Object = SequencedEvent;
	using Policy = TwoPeerPolicy;
};

template<>
struct DescriptorTraits<DescriptorType::io> {
	using Object = IoSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::kernletObject> {
	using Object = KernletObject;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::boundKernlet> {
	using Object = BoundKernlet;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::token> {
	using Object = TokenObject;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::hierarchy> {
	using Object = Hierarchy;
	using Policy = smarter::default_rc_policy;
};

#ifdef THOR_HAS_KCOV
template<>
struct DescriptorTraits<DescriptorType::kcovBuffer> {
	using Object = KcovBuffer;
	using Policy = smarter::default_rc_policy;
};
#endif

// smarter::shared_ptr type that a descriptor of type K holds.
template<DescriptorType K>
using DescriptorPointer = smarter::shared_ptr<
	typename DescriptorTraits<K>::Object,
	typename DescriptorTraits<K>::Policy
>;

struct AnyDescriptor {
	friend struct DescriptorView;

	friend void swap(AnyDescriptor &x, AnyDescriptor &y) {
		using std::swap;
		swap(x.type_, y.type_);
		swap(x.extra_, y.extra_);
		swap(x.rights_, y.rights_);
		swap(x.object_, y.object_);
		swap(x.ctr_, y.ctr_);
	}

	// Constructs a descriptor of type K that takes over the given pointer's reference.
	template<DescriptorType K>
	static AnyDescriptor make(DescriptorPointer<K> ptr, uint32_t rights);

	AnyDescriptor() = default;

	AnyDescriptor(smarter::adopt_rc_t, DescriptorType type, uint8_t extra, uint32_t rights,
			void *object, smarter::counter *ctr)
	: type_{type}, extra_{extra}, rights_{rights}, object_{object}, ctr_{ctr} { }

	AnyDescriptor(const AnyDescriptor &other)
	: type_{other.type_}, extra_{other.extra_}, rights_{other.rights_}, object_{other.object_}, ctr_{other.ctr_} {
		if(ctr_)
			ctr_->increment();
	}

	AnyDescriptor(AnyDescriptor &&other)
	: AnyDescriptor{} {
		swap(*this, other);
	}

	~AnyDescriptor() {
		if(ctr_ && ctr_->decrement_and_check_if_zero())
			releaseOnZero_();
	}

	AnyDescriptor &operator= (AnyDescriptor other) {
		swap(*this, other);
		return *this;
	}

	DescriptorType type() const {
		return type_;
	}

	uint32_t rights() const {
		return rights_;
	}

	uint8_t raw_extra() const { return extra_; }
	void *raw_object() const { return object_; }
	smarter::counter *raw_ctr() const { return ctr_; }

	void release() {
		type_ = DescriptorType::none;
		extra_ = 0;
		rights_ = 0;
		object_ = nullptr;
		ctr_ = nullptr;
	}

	template<DescriptorType K>
	bool is() const {
		return type_ == K;
	}

	// Resolves the descriptor to the object it holds (takes a new reference).
	// Fails with badDescriptor unless the descriptor is of type K.
	// Fails with badRights until the descriptor has all required rights.
	template<DescriptorType K>
	std::expected<DescriptorPointer<K>, Error> resolveObject(uint32_t requiredRights) const {
		if (!is<K>())
			return std::unexpected{Error::badDescriptor};
		if (!checkRights(rights_, requiredRights))
			return std::unexpected{Error::badRights};
		ctr_->increment();
		return adopt_<K>(object_, extra_, ctr_);
	}

	// Keep only the rights in the exposedRights mask.
	void exposeRights(uint32_t exposedRights) {
		rights_ &= exposedRights;
	}

private:
	template<DescriptorType K>
	static DescriptorPointer<K> adopt_(void *object, uint8_t extra, smarter::counter *ctr);

	void releaseOnZero_();

	DescriptorType type_ = DescriptorType::none;
	// Extra per-descriptor data for some descriptor types.
	// - For lane descriptors: the lane index.
	// - For sequenced event descriptors: waitEnd or raiseEnd.
	uint8_t extra_ = 0;
	// Rights associated with the descriptor.
	uint32_t rights_ = 0;
	// Invariant: object_ is non-null if type_ != DescriptorType::none.
	// For descriptor types that use TwoPeerPolicy, this is a TwoPeerObject pointer.
	void *object_ = nullptr;
	// Invariant: ctr_ is non-null if type_ != DescriptorType::none.
	smarter::counter *ctr_ = nullptr;
};

template<DescriptorType K>
AnyDescriptor AnyDescriptor::make(DescriptorPointer<K> ptr, uint32_t rights) {
	using Policy = typename DescriptorTraits<K>::Policy;
	static_assert(std::same_as<Policy, smarter::default_rc_policy>
			|| std::same_as<Policy, TwoPeerPolicy>);
	// AnyDescriptor may be stored in RCU protected data structures (e.g., Universe).
	// Hence, the objects that we store (and their refcount control blocks) must also be RCU protected.
	static_assert(IsRcuProtected<typename DescriptorTraits<K>::Object>);
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = K;
	descriptor.rights_ = rights;
	if constexpr (std::same_as<Policy, TwoPeerPolicy>) {
		auto object = ptr.policy().object();
		auto peer = ptr.policy().peer();
		descriptor.extra_ = static_cast<uint8_t>(peer);
		descriptor.object_ = object;
		descriptor.ctr_ = &object->peerCounter(peer);
	} else {
		descriptor.object_ = ptr.get();
		descriptor.ctr_ = &ptr.policy().base()->ctr();
	}
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::thread>(
		smarter::shared_ptr<Thread, ActiveHandle> ptr, uint32_t rights);

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::addressSpace>(
		smarter::shared_ptr<AddressSpace, BindableHandle> ptr, uint32_t rights);

template<DescriptorType K>
DescriptorPointer<K> AnyDescriptor::adopt_(void *object, uint8_t extra, smarter::counter *ctr) {
	using Policy = typename DescriptorTraits<K>::Policy;
	using ObjectType = typename DescriptorTraits<K>::Object;

	if constexpr (std::same_as<Policy, TwoPeerPolicy>) {
		auto base = static_cast<TwoPeerObject *>(object);
		return DescriptorPointer<K>{
			smarter::adopt_rc,
			static_cast<ObjectType *>(base),
			TwoPeerPolicy{base, static_cast<int>(extra)}
		};
	} else {
		static_assert(std::same_as<Policy, smarter::default_rc_policy>);
		return smarter::shared_ptr<ObjectType>{
			smarter::adopt_rc,
			static_cast<ObjectType *>(object),
			smarter::default_rc_policy{smarter::meta_object_base::from_ctr(ctr)}
		};
	}
}

template<>
inline smarter::shared_ptr<Thread, ActiveHandle>
AnyDescriptor::adopt_<DescriptorType::thread>(void *object, uint8_t, smarter::counter *) {
	auto thread = static_cast<Thread *>(object);
	return smarter::shared_ptr<Thread, ActiveHandle>{
		smarter::adopt_rc, thread, ActiveHandle{thread}
	};
}

template<>
inline smarter::shared_ptr<AddressSpace, BindableHandle>
AnyDescriptor::adopt_<DescriptorType::addressSpace>(void *object, uint8_t, smarter::counter *) {
	auto space = static_cast<AddressSpace *>(object);
	return smarter::shared_ptr<AddressSpace, BindableHandle>{
		smarter::adopt_rc, space, BindableHandle{space}
	};
}

// --------------------------------------------------------
// DescriptorView
// --------------------------------------------------------

// Non-owning view of a descriptor that is attached to a Universe.
// Only valid within the RCU critical section of Universe::inspectDescriptor().
struct DescriptorView {
	friend struct Universe;

	DescriptorView(const DescriptorView &) = delete;

	DescriptorView &operator= (const DescriptorView &) = delete;

	DescriptorType type() const {
		return type_;
	}

	uint32_t rights() const {
		return rights_;
	}

	template<DescriptorType K>
	bool is() const {
		return type_ == K;
	}

	// Takes a new reference that remains valid outside of the RCU critical section.
	// Fails with noDescriptor if the handle was detached concurrently and the object died.
	std::expected<AnyDescriptor, Error> pin() const {
		if(!ctr_->increment_if_nonzero())
			return std::unexpected{Error::noDescriptor};
		return AnyDescriptor{smarter::adopt_rc, type_, extra_, rights_, object_, ctr_};
	}

	// Like AnyDescriptor::resolveObject(). In addition, fails with noDescriptor like pin().
	template<DescriptorType K>
	std::expected<DescriptorPointer<K>, Error> resolveObject(uint32_t requiredRights) const {
		if (!is<K>())
			return std::unexpected{Error::badDescriptor};
		if (!checkRights(rights_, requiredRights))
			return std::unexpected{Error::badRights};
		if (!ctr_->increment_if_nonzero())
			return std::unexpected{Error::noDescriptor};
		return AnyDescriptor::adopt_<K>(object_, extra_, ctr_);
	}

	// Returns both the object (see resolveObject()) and rights.
	template<DescriptorType K>
	std::expected<std::tuple<DescriptorPointer<K>, uint32_t>, Error>
	resolveCapability(uint32_t requiredRights) const {
		auto object = FRG_TRY(resolveObject<K>(requiredRights));
		return std::tuple{std::move(object), rights_};
	}

private:
	DescriptorView(DescriptorType type, uint8_t extra, uint32_t rights,
			void *object, smarter::counter *ctr)
	: type_{type}, extra_{extra}, rights_{rights}, object_{object}, ctr_{ctr} { }

	DescriptorType type_;
	uint8_t extra_;
	uint32_t rights_;
	void *object_;
	smarter::counter *ctr_;
};

// --------------------------------------------------------
// Universe.
// --------------------------------------------------------

// Maps handles to descriptors.
// Lookups are lock-free via RCU, modifications take a lock.
// Handles encode a slot index in their low bits and a per-slot generation in their high bits.
// Slot reuse bumps the generation. Slots are only reused after an RCU grace period has passed since the previous detach.
struct Universe : RcuProtected, private RcuCallable {
private:
	struct CtorToken {};

	static constexpr unsigned int slotIndexBits = 20;
	static constexpr unsigned int generationBits = 63 - slotIndexBits;
	static constexpr uint64_t slotIndexMask = (uint64_t{1} << slotIndexBits) - 1;
	// Set in a slot's handle field iff no descriptor is attached to the slot.
	static constexpr uint64_t invalidMarker = uint64_t{1} << 63;
	// Denotes the empty list in the free/pending/retiring head and tail members.
	static constexpr uint32_t nilIndex = ~uint32_t{0};

	static constexpr unsigned int chunkShift = 6;
	static constexpr size_t chunkSize = size_t{1} << chunkShift;

	struct Slot {
		// Encodes the state of this slot.
		// - Live slots: the invalidMarker bit is clear.
		//   The value matches the handle of the slot.
		// - Detached slots: the invalidMarker bit is set.
		//   Generation bits store the next generation to use.
		//   Index bits store the next slot in the free-list that the slot is part of.
		std::atomic<uint64_t> state{invalidMarker | (uint64_t{1} << slotIndexBits)};
		// The remainder of the fields are constant after attachDescriptor().
		// They remain valid until reuse (i.e., until a RCU grace period has passed after detachDescriptor()).
		void *object = nullptr;
		smarter::counter *ctr = nullptr;
		DescriptorType type = DescriptorType::none;
		uint8_t extra = 0;
		uint32_t rights = 0;
	};
	static_assert(sizeof(Slot) == 32);

	struct Root : RcuCallable {
		size_t numChunks;

		std::atomic<Slot *> *chunks() {
			return reinterpret_cast<std::atomic<Slot *> *>(this + 1);
		}
	};

public:
	static std::expected<smarter::shared_ptr<Universe>, Error> create();

	Universe(CtorToken);

	Universe(const Universe &) = delete;

	Universe &operator= (const Universe &) = delete;

	~Universe();

	Handle attachDescriptor(AnyDescriptor descriptor);

	std::optional<AnyDescriptor> getDescriptor(Handle handle);

	// fn runs within an RCU read-side section and must not block.
	// fn should also avoid dropping references (move them out instead) such that object teardown does not run within the RCU section.
	template<typename Fn>
	requires requires(Fn fn, const DescriptorView &desc) {
		{ fn(desc) };
	}
	auto inspectDescriptor(Handle handle, Fn &&fn)
			-> std::invoke_result_t<Fn, const DescriptorView &> {
		using ResultType = std::invoke_result_t<Fn, const DescriptorView &>;

		if(handle <= 0)
			return ResultType{std::unexpect, Error::noDescriptor};
		auto h = static_cast<uint64_t>(handle);

		IplGuard<ipl::noSchedule> rcuGuard;

		auto slot = slotFor_(h);
		if(!slot || slot->state.load(std::memory_order_acquire) != h)
			return ResultType{std::unexpect, Error::noDescriptor};
		DescriptorView desc{slot->type, slot->extra, slot->rights, slot->object, slot->ctr};
		return std::forward<Fn>(fn)(desc);
	}

	// Convenience wrapper for inspectDescriptor() -> resolveObject().
	template<DescriptorType K>
	std::expected<DescriptorPointer<K>, Error> resolveObject(Handle handle, uint32_t rights) {
		return inspectDescriptor(handle, [&](const DescriptorView &desc) {
			return desc.resolveObject<K>(rights);
		});
	}

	// Convenience wrapper for inspectDescriptor() -> resolveCapability().
	template<DescriptorType K>
	std::expected<std::tuple<DescriptorPointer<K>, uint32_t>, Error>
	resolveCapability(Handle handle, uint32_t rights) {
		return inspectDescriptor(handle, [&](const DescriptorView &desc) {
			return desc.resolveCapability<K>(rights);
		});
	}

	frg::optional<AnyDescriptor> detachDescriptor(Handle handle);

private:
	Slot *slotFor_(uint64_t handle) {
		auto index = handle & slotIndexMask;
		auto root = root_.load(std::memory_order_acquire);
		auto chunkIndex = index >> chunkShift;
		if(chunkIndex >= root->numChunks)
			return nullptr;
		auto chunk = root->chunks()[chunkIndex].load(std::memory_order_acquire);
		if(!chunk)
			return nullptr;
		return &chunk[index & (chunkSize - 1)];
	}

	Slot *slotAt_(uint32_t index);
	void setLink_(Slot *slot, uint32_t next);
	Slot *allocateSlot_();
	void growRoot_(size_t numChunks);
	static void recycleRcu_(RcuCallable *base);

	frg::ticket_spinlock lock_;
	std::atomic<Root *> root_{nullptr};
	// Number of slots that have been opened so far (never decreases; slots are recycled).
	size_t numSlots_ = 0;
	// FIFO of slots that are ready for reuse.
	uint32_t freeHead_ = nilIndex;
	uint32_t freeTail_ = nilIndex;
	// Open batch of slots that await recycling; closed once the retiring batch drains.
	uint32_t pendingHead_ = nilIndex;
	uint32_t pendingTail_ = nilIndex;
	// Batch of slots covered by the in-flight RCU callback.
	uint32_t retiringHead_ = nilIndex;
	uint32_t retiringTail_ = nilIndex;
	bool rcuInFlight_ = false;
	// Used to keep the Universe alive while the RCU callback is in flight.
	smarter::borrowed_ptr<Universe> selfPtr_;
};

inline std::optional<AnyDescriptor> Universe::getDescriptor(Handle handle) {
	auto pinned = inspectDescriptor(handle, [](const DescriptorView &desc) {
		return desc.pin();
	});
	if(!pinned)
		return std::nullopt;
	return std::move(*pinned);
}

} // namespace thor
