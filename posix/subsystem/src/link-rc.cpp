#include "link-rc.hpp"

void LinkReclaimer::enlist(LinkMetaObjectBase *meta) {
	std::unique_lock lock{_mutex};
	// The link was reclaimed while we were waiting for the mutex, or it is enlisted already
	// because it was referenced and released again after enlisting.
	if(meta->reclaimState != LinkMetaObjectBase::ReclaimState::owned)
		return;
	meta->reclaimState = LinkMetaObjectBase::ReclaimState::enlisted;
	_lru.push_front(meta);
	++_size;

	if(_size > _capacity)
		trim();
	runDestructors(lock);
}

void LinkReclaimer::touch(LinkMetaObjectBase *meta) {
	std::lock_guard lock{_mutex};
	if(meta->reclaimState != LinkMetaObjectBase::ReclaimState::enlisted)
		return;
	_lru.erase(_lru.iterator_to(meta));
	_lru.push_front(meta);
}

void LinkReclaimer::reclaim(LinkMetaObjectBase *meta) {
	std::unique_lock lock{_mutex};
	if(meta->reclaimState == LinkMetaObjectBase::ReclaimState::destructing)
		return;
	if(meta->reclaimState == LinkMetaObjectBase::ReclaimState::enlisted) {
		_lru.erase(_lru.iterator_to(meta));
		--_size;
		meta->reclaimState = LinkMetaObjectBase::ReclaimState::owned;
	}
	tryReclaim(meta);
	runDestructors(lock);
}

// Releases our reference, but only if it is the last one. On failure the link is referenced
// elsewhere and enlist() picks it up again once that reference goes away.
bool LinkReclaimer::tryReclaim(LinkMetaObjectBase *meta) {
	unsigned int expected = 1;
	if(!meta->ctr().raw().compare_exchange_strong(expected, 0,
			std::memory_order_acq_rel, std::memory_order_relaxed))
		return false;
	meta->reclaimState = LinkMetaObjectBase::ReclaimState::destructing;
	onReclaim(meta);
	_destructing.push_back(meta);
	return true;
}

void LinkReclaimer::trim() {
	while(_size > _capacity && !_lru.empty()) {
		auto meta = _lru.pop_back();
		--_size;
		meta->reclaimState = LinkMetaObjectBase::ReclaimState::owned;
		tryReclaim(meta);
	}
}

void LinkReclaimer::runDestructors(std::unique_lock<std::mutex> &lock) {
	// Destructing a link drops its references to other links, which re-enters the reclaimer.
	if(_runningDestructors)
		return;
	_runningDestructors = true;
	while(!_destructing.empty()) {
		auto meta = _destructing.pop_front();
		lock.unlock();
		meta->finalize();
		lock.lock();
	}
	_runningDestructors = false;
}
