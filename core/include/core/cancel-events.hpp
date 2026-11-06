#pragma once

#include <async/cancellation.hpp>
#include <expected>
#include <helix/ipc.hpp>
#include <map>
#include <mutex>

struct CancelEventRegistry {
	struct Error {};

	struct CancelEventGuard {
		friend CancelEventRegistry;

		CancelEventGuard(CancelEventRegistry *r, helix_ng::Credentials creds, uint64_t id)
		: r_{r}, creds_{creds}, id_{id} {
			token_ = r_->registerEvent(creds_, id_);
		}

		CancelEventGuard()
		: r_{nullptr}, creds_{}, id_{0}, token_{} { }

		CancelEventGuard(const CancelEventGuard &) = delete;

		CancelEventGuard(CancelEventGuard &&other) {
			swap(*this, other);
		}

		CancelEventGuard &operator=(CancelEventGuard other) {
			swap(*this, other);
			return *this;
		}

		~CancelEventGuard() {
			if (r_)
				r_->removeEvent(creds_, id_);
		}

		friend void swap(CancelEventGuard &a, CancelEventGuard &b) {
			using std::swap;
			swap(a.r_, b.r_);
			swap(a.creds_, b.creds_);
			swap(a.id_, b.id_);
			swap(a.token_, b.token_);
		}

		operator bool() const {
			return token_.has_value();
		}

		operator async::cancellation_token() {
			assert(token_.has_value());
			return token_.value();
		}

	private:
		CancelEventRegistry *r_;
		helix_ng::Credentials creds_;
		uint64_t id_;
		std::expected<async::cancellation_token, Error> token_;
	};

	CancelEventGuard event(helix_ng::Credentials creds, uint64_t id) {
		CancelEventGuard reg{this, creds, id};
		return reg;
	}

	bool cancel(helix_ng::Credentials creds, uint64_t id) {
		async::deferred_cancellation deferred;
		{
			std::lock_guard lock{mutex_};
			auto res = list_.find({creds, id});
			if (res == list_.end())
				return false;
			res->second.cancel_deferred(deferred);
		}
		deferred.invoke();
		return true;
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
	std::expected<async::cancellation_token, Error> registerEvent(helix_ng::Credentials creds, uint64_t id) {
		if (id == 0)
			return {closedEvent_};

		std::lock_guard lock{mutex_};
		auto emplaceResult = list_.emplace(std::piecewise_construct,
			std::tuple{creds, id}, std::tuple{});
		if (!emplaceResult.second)
			return std::unexpected{Error{}};

		// The new event has no callbacks yet, hence cancelling it under the lock is fine.
		if (closed_)
			emplaceResult.first->second.cancel();

		return {emplaceResult.first->second};
	}

	void removeEvent(helix_ng::Credentials creds, uint64_t id) {
		std::lock_guard lock{mutex_};
		list_.erase({creds, id});
	}

private:
	std::mutex mutex_;
	bool closed_ = false;
	async::cancellation_event closedEvent_;

	// TODO: unordered_map would be better, but std::pair is not hashable ...
	std::map<std::pair<helix_ng::Credentials, uint64_t>, async::cancellation_event> list_;
};
