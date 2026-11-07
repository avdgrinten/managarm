#pragma once

#include <array>
#include <async/cancellation.hpp>
#include <compare>
#include <cstring>
#include <frg/hash.hpp>
#include <helix/ipc.hpp>
#include <mutex>
#include <unordered_map>

// Like in thor, cancellation tags need not be unique: cancel() cancels all events with the tag.
struct CancelEventRegistry {
	struct Key {
		helix_ng::Credentials creds;
		uint64_t tag;

		bool operator==(const Key &other) const {
			return std::is_eq(creds <=> other.creds) && tag == other.tag;
		}
	};

	struct KeyHash {
		size_t operator()(const Key &key) const {
			auto creds = key.creds;
			std::array<char, 16> bytes = creds;
			uint64_t words[2];
			memcpy(words, bytes.data(), sizeof(words));
			return frg::mix_bits(words[0] ^ frg::mix_bits(words[1] ^ frg::mix_bits(key.tag)));
		}
	};

	using EventMap = std::unordered_multimap<Key, async::cancellation_event, KeyHash>;

	struct CancelEventGuard {
		friend CancelEventRegistry;

		CancelEventGuard(CancelEventRegistry *r, helix_ng::Credentials creds, uint64_t tag)
		: r_{r}, key_{creds, tag}, event_{r->registerEvent(key_)},
			token_{event_ ? async::cancellation_token{*event_} : r->closedToken()} { }

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
			if (r_ && event_)
				r_->removeEvent(key_, event_);
		}

		friend void swap(CancelEventGuard &a, CancelEventGuard &b) {
			using std::swap;
			swap(a.r_, b.r_);
			swap(a.key_, b.key_);
			swap(a.event_, b.event_);
			swap(a.token_, b.token_);
		}

		operator async::cancellation_token() const {
			return token_;
		}

	private:
		CancelEventRegistry *r_ = nullptr;
		Key key_{};
		async::cancellation_event *event_ = nullptr;
		async::cancellation_token token_;
	};

	CancelEventGuard event(helix_ng::Credentials creds, uint64_t tag) {
		return CancelEventGuard{this, creds, tag};
	}

	bool cancel(helix_ng::Credentials creds, uint64_t tag) {
		async::deferred_cancellation deferred;
		bool found = false;
		{
			std::lock_guard lock{mutex_};
			auto [begin, end] = list_.equal_range(Key{creds, tag});
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

	// Cancelled by close(); for waits that do not have a cancellation tag, like event(creds, 0).
	async::cancellation_token closedToken() {
		return closedEvent_;
	}

protected:
	async::cancellation_event *registerEvent(const Key &key) {
		if (key.tag == 0)
			return nullptr;

		std::lock_guard lock{mutex_};
		auto it = list_.emplace(std::piecewise_construct, std::tuple{key}, std::tuple{});

		// The new event has no callbacks yet, hence cancelling it under the lock is fine.
		if (closed_)
			it->second.cancel();

		return &it->second;
	}

	// Iterators do not survive rehashing, hence we identify the entry by the address of its event.
	void removeEvent(const Key &key, async::cancellation_event *event) {
		std::lock_guard lock{mutex_};
		auto [begin, end] = list_.equal_range(key);
		for (auto it = begin; it != end; ++it) {
			if (&it->second == event) {
				list_.erase(it);
				return;
			}
		}
	}

private:
	std::mutex mutex_;
	bool closed_ = false;
	async::cancellation_event closedEvent_;

	EventMap list_;
};
