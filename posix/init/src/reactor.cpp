#include <algorithm>
#include <errno.h>
#include <poll.h>
#include <print>
#include <stdlib.h>
#include <string.h>

#include "log.hpp"
#include "reactor.hpp"

UeventStream::UeventStream(Reactor &reactor) : reactor_{reactor} {
	reactor_.streams_.push_back(this);
}

UeventStream::~UeventStream() {
	reactor_.streams_.remove(this);
}

async::result<Uevent> UeventStream::next() {
	// Without cancellation, the queue always yields an item.
	auto uevent = co_await queue_.async_get();
	co_return std::move(*uevent);
}

async::result<std::optional<Uevent>> UeventStream::next(async::cancellation_token ct) {
	auto uevent = co_await queue_.async_get(ct);
	if(!uevent)
		co_return std::nullopt;
	co_return std::move(*uevent);
}

async::result<bool> Reactor::sleepFor(Clock::duration duration, async::cancellation_token ct) {
	Timer timer{.deadline = Clock::now() + duration};
	timers_.push_back(&timer);
	bool expired = co_await timer.expired.wait(ct);
	timers_.remove(&timer);
	co_return expired;
}

void Reactor::wait() {
	// Coroutines are resumed from the run queue; they need to run before we block.
	if(rq_.check()) {
		rq_.run_iteration();
		return;
	}

	// Timers are absolute, so uevents that no coroutine waits for do not delay them.
	auto now = Clock::now();
	bool expired = false;
	std::optional<Clock::time_point> wakeup;
	for(auto timer : timers_) {
		if(timer->raised)
			continue;
		if(timer->deadline <= now) {
			timer->raised = true;
			timer->expired.raise();
			expired = true;
		} else if(!wakeup || timer->deadline < *wakeup) {
			wakeup = timer->deadline;
		}
	}
	if(expired)
		return;

	int timeoutMs = -1;
	if(wakeup) {
		auto left = std::chrono::ceil<std::chrono::milliseconds>(*wakeup - now);
		timeoutMs = std::max<int>(left.count(), 0);
	}

	pollfd pfd{.fd = socket_.fd(), .events = POLLIN};
	int ret = poll(&pfd, 1, timeoutMs);
	if(ret < 0) {
		std::println("init: poll() on the uevent socket failed: {}", strerror(errno));
		abort();
	}
	if(!ret)
		return;

	auto uevent = socket_.receive();
	if(!uevent) {
		std::println("init: Failed to receive uevent: {}", strerror(uevent.error()));
		abort();
	}

	auto action = lookup(*uevent, "ACTION");
	if(action.empty() || lookup(*uevent, "DEVPATH").empty()) {
		std::println("init: Ignoring uevent without ACTION or DEVPATH");
		return;
	}
	if(action != "add")
		return;

	if(logDiscovery) {
		std::println("init: Received uevent");
		for (const auto &kv : *uevent)
			std::println("    {}={}", kv.first, kv.second);
	}

	for(auto stream : streams_)
		stream->queue_.put(*uevent);
}
