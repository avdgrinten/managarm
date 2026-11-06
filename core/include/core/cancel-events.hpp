#pragma once

#include <async/cancellation.hpp>
#include <helix/ipc.hpp>
#include <map>
#include <mutex>
#include <optional>

// Like thor's cancellation tags, IDs need not be unique: cancel() cancels all events with the ID.
struct CancelEventRegistry {
	// TODO: unordered_multimap would be better, but std::pair is not hashable ...
	using EventMap = std::multimap<std::pair<helix_ng::Credentials, uint64_t>, async::cancellation_event>;

	struct CancelEventGuard {
		friend CancelEventRegistry;

		CancelEventGuard(CancelEventRegistry *r, helix_ng::Credentials creds, uint64_t id)
		: r_{r}, entry_{r->registerEvent(creds, id)},
			token_{entry_ ? async::cancellation_token{(*entry_)->second} : r->closedToken()} { }

		CancelEventGuard() = default;

		CancelEventGuard(const CancelEventGuard &) = delete;

		CancelEventGuard(CancelEventGuard &&other) {
			swap(*this, other);
		}

		CancelEventGuard &operator=(CancelEventGuard other) {
			swap(*this, other);
			return *this;
		}

		~CancelEventGuard() {
			if (r_ && entry_)
				r_->removeEvent(*entry_);
		}

		friend void swap(CancelEventGuard &a, CancelEventGuard &b) {
			using std::swap;
			swap(a.r_, b.r_);
			swap(a.entry_, b.entry_);
			swap(a.token_, b.token_);
		}

		operator async::cancellation_token() const {
			return token_;
		}

	private:
		CancelEventRegistry *r_ = nullptr;
		std::optional<EventMap::iterator> entry_;
		async::cancellation_token token_;
	};

	CancelEventGuard event(helix_ng::Credentials creds, uint64_t id) {
		return CancelEventGuard{this, creds, id};
	}

	bool cancel(helix_ng::Credentials creds, uint64_t id) {
		async::deferred_cancellation deferred;
		bool found = false;
		{
			std::lock_guard lock{mutex_};
			auto [begin, end] = list_.equal_range({creds, id});
			for (auto it = begin; it != end; ++it) {
				it->second.cancel_deferred(deferred);
				found = true;
			}
		}
		deferred.invoke();
		return found;
	}

	// Cancels all events, including the ones that are registered afterwards.
	void close() {
		async::deferred_cancellation deferred;
		{
			std::lock_guard lock{mutex_};
			closed_ = true;
			closedEvent_.cancel_deferred(deferred);
			for (auto &[key, event] : list_)
				event.cancel_deferred(deferred);
		}
		deferred.invoke();
	}

	// Cancelled by close(); for waits that do not have a cancellation ID, like event(creds, 0).
	async::cancellation_token closedToken() {
		return closedEvent_;
	}

protected:
	std::optional<EventMap::iterator> registerEvent(helix_ng::Credentials creds, uint64_t id) {
		if (id == 0)
			return std::nullopt;

		std::lock_guard lock{mutex_};
		auto it = list_.emplace(std::piecewise_construct, std::tuple{creds, id}, std::tuple{});

		// The new event has no callbacks yet, hence cancelling it under the lock is fine.
		if (closed_)
			it->second.cancel();

		return it;
	}

	void removeEvent(EventMap::iterator it) {
		std::lock_guard lock{mutex_};
		list_.erase(it);
	}

private:
	std::mutex mutex_;
	bool closed_ = false;
	async::cancellation_event closedEvent_;

	EventMap list_;
};
