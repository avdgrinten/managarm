#pragma once

#include <mutex>

#include <frg/list.hpp>
#include <frg/manual_box.hpp>
#include <smarter.hpp>

struct LinkReclaimer;

struct LinkMetaObjectBase : smarter::meta_object_base {
	LinkMetaObjectBase(void (*finalize)(smarter::meta_object_base *),
			void (*finalizeWeak)(smarter::meta_object_base *),
			LinkReclaimer *reclaimer_)
	: smarter::meta_object_base{finalize, finalizeWeak}, reclaimer{reclaimer_} { }

	// Owner that holds a reference of its own, or null if the link is not reclaimable.
	// Fixed at construction so that decrement() can read it without synchronization.
	LinkReclaimer *const reclaimer;

	enum class ReclaimState {
		// The link is not part of the reclaimer's LRU list.
		owned,
		// The link waits in the reclaimer's LRU list.
		enlisted,
		// The link was reclaimed and waits to be destructed.
		destructing
	};

	// Protected by LinkReclaimer::_mutex.
	ReclaimState reclaimState = ReclaimState::owned;
	// Protected by LinkReclaimer::_mutex.
	frg::default_list_hook<LinkMetaObjectBase> reclaimHook;
};

// Refcount policy of all FsLinks.
// Intercepts refcount drops to one such that owners can put cached links into a reclaim list.
struct LinkRc {
	LinkRc() = default;

	explicit LinkRc(LinkMetaObjectBase *meta)
	: _meta{meta} { }

	explicit operator bool() const {
		return _meta != nullptr;
	}

	void increment() const {
		_meta->ctr().increment();
	}

	void decrement() const;

	bool try_increment() const {
		return _meta->ctr().increment_if_nonzero();
	}

	void increment_weak() const {
		_meta->weak_ctr().increment();
	}

	void decrement_weak() const {
		if(_meta->weak_ctr().decrement_and_check_if_zero())
			_meta->finalize_weak();
	}

	LinkMetaObjectBase *meta() const {
		return _meta;
	}

private:
	LinkMetaObjectBase *_meta{nullptr};
};

// Keeps links alive until they are collected from an LRU list.
// The reclaimer owns one reference per link.
// That reference is released by a CAS in trim(), it is always that last reference that is released.
struct LinkReclaimer {
	LinkReclaimer(size_t capacity)
	: _capacity{capacity} { }

	// Called by LinkRc once the reclaimer's reference is the only one left.
	void enlist(LinkMetaObjectBase *meta);

	// Marks a link as recently used.
	void touch(LinkMetaObjectBase *meta);

	// Reclaims a link right away, e.g. because its owner knows that it is useless.
	// Links that are still referenced elsewhere are reclaimed once these external references go away.
	void reclaim(LinkMetaObjectBase *meta);

protected:
	~LinkReclaimer() = default;

	// Called before a reclaimed link is destructed, i.e. while it is still constructed but
	// unreferenced. Runs under the reclaimer's mutex.
	virtual void onReclaim(LinkMetaObjectBase *meta) = 0;

private:
	using ReclaimList = frg::intrusive_list<LinkMetaObjectBase,
			frg::locate_member<LinkMetaObjectBase,
					frg::default_list_hook<LinkMetaObjectBase>,
					&LinkMetaObjectBase::reclaimHook>>;

	bool tryReclaim(LinkMetaObjectBase *meta);
	void trim();
	void runDestructors(std::unique_lock<std::mutex> &lock);

	std::mutex _mutex;
	ReclaimList _lru;
	ReclaimList _destructing;
	// Number of enlisted links, i.e. of the links that the reclaimer is able to reclaim.
	size_t _size = 0;
	size_t _capacity;
	// Whether a thread is already running the deferred destructors.
	bool _runningDestructors = false;
};

inline void LinkRc::decrement() const {
	// Keep the meta object alive across enlist(): the link can be reclaimed concurrently
	// once we give up our reference below.
	auto reclaimer = _meta->reclaimer;
	if(reclaimer)
		_meta->weak_ctr().increment();

	auto count = _meta->ctr().raw().fetch_sub(1, std::memory_order_acq_rel);
	assert(count >= 1);
	if(count == 1) {
		_meta->finalize();
	}else if(count == 2 && reclaimer) {
		reclaimer->enlist(_meta);
	}

	if(reclaimer && _meta->weak_ctr().decrement_and_check_if_zero())
		_meta->finalize_weak();
}

static_assert(smarter::weak_rc_policy<LinkRc>);

template<typename T>
struct LinkMetaObject : LinkMetaObjectBase {
	template<typename... Args>
	LinkMetaObject(LinkReclaimer *reclaimer, Args &&...args)
	: LinkMetaObjectBase{&finalize_, &finalizeWeak_, reclaimer} {
		// Reclaimable links are constructed with the reclaimer's reference in place.
		ctr().setup(smarter::adopt_rc, reclaimer ? 2 : 1);
		weak_ctr().setup(smarter::adopt_rc, 1);
		_box.initialize(std::forward<Args>(args)...);
	}

	T *get() {
		return _box.get();
	}

private:
	static void finalize_(smarter::meta_object_base *base) {
		auto self = static_cast<LinkMetaObject *>(base);
		self->_box.destruct();
		if(base->weak_ctr().decrement_and_check_if_zero())
			base->finalize_weak();
	}

	static void finalizeWeak_(smarter::meta_object_base *base) {
		delete static_cast<LinkMetaObject *>(base);
	}

	frg::manual_box<T> _box;
};
