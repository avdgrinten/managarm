#pragma once

#include <async/basic.hpp>
#include <async/cancellation.hpp>
#include <async/oneshot-event.hpp>
#include <async/queue.hpp>
#include <async/result.hpp>
#include <chrono>
#include <frg/std_compat.hpp>
#include <list>
#include <optional>

#include "uevent.hpp"

using Clock = std::chrono::steady_clock;

class Reactor;

// Yields the add uevents that the Reactor receives after the stream is constructed.
// All of them carry DEVPATH.
// Coroutines need to construct their streams before they suspend for the first time;
// otherwise, they miss the uevents that triggerColdplug() caused before Reactor::run().
class UeventStream {
	friend class Reactor;

public:
	explicit UeventStream(Reactor &reactor);

	UeventStream(const UeventStream &) = delete;
	UeventStream &operator=(const UeventStream &) = delete;

	~UeventStream();

	// Devices can be reported more than once (e.g., a device that is announced while
	// triggerColdplug() runs gets both its real and a synthetic add uevent).
	// Hence, consumers of the uevents need to be idempotent.
	async::result<Uevent> next();

	// Returns std::nullopt on cancellation.
	async::result<std::optional<Uevent>> next(async::cancellation_token ct);

private:
	Reactor &reactor_;
	async::queue<Uevent, frg::stl_allocator> queue_;
};

// Handles the add uevents that the Reactor receives. All of them carry DEVPATH.
// Rules need to be constructed before triggerColdplug() to handle the uevents of all devices.
class UeventRule {
public:
	explicit UeventRule(Reactor &reactor);

	UeventRule(const UeventRule &) = delete;
	UeventRule &operator=(const UeventRule &) = delete;

	// Returns false once the rule is done; the Reactor does not call it again afterwards.
	// Devices can be reported more than once (e.g., a device that is announced while
	// triggerColdplug() runs gets both its real and a synthetic add uevent).
	// Hence, rules need to be idempotent.
	virtual bool handle(const Uevent &uevent) = 0;

protected:
	~UeventRule();

	Reactor &reactor_;
};

// Runs coroutines on top of poll(): lets them sleep and hands uevents to their UeventStreams
// and to the UeventRules.
class Reactor {
	friend class UeventStream;
	friend class UeventRule;

public:
	explicit Reactor(UeventSocket &socket) : socket_{socket} {}

	Reactor(const Reactor &) = delete;
	Reactor &operator=(const Reactor &) = delete;

	// Runs a coroutine until it completes. All coroutines that use the Reactor need to be
	// awaited (directly or indirectly) by this coroutine such that they run on our run queue.
	template<typename T>
	T run(async::result<T> coroutine) {
		return async::run(std::move(coroutine), IoService{this});
	}

	// Returns false on cancellation.
	async::result<bool> sleepFor(Clock::duration duration, async::cancellation_token ct = {});

private:
	struct RunQueue final : async::run_queue {
		// Work is only posted from the thread that runs the Reactor.
		void wakeup() override {}
	};

	struct IoService {
		async::run_queue *get_run_queue() { return &self->rq_; }
		void wait() { self->wait(); }

		Reactor *self;
	};

	struct Timer {
		Clock::time_point deadline;
		async::oneshot_event expired;
		bool raised = false;
	};

	// Runs the coroutines that are ready; if there are none, blocks until a timer expires
	// or a uevent arrives.
	void wait();

	UeventSocket &socket_;
	RunQueue rq_;
	std::list<Timer *> timers_;
	std::list<UeventStream *> streams_;
	std::list<UeventRule *> rules_;
};
