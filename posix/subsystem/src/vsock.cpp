#include "vsock.hpp"

#include <atomic>
#include <print>

#include <protocols/mbus/client.hpp>

namespace vsock {
namespace {
helix::UniqueLane serverLane;
// serverLane is written once before this is set and never changes afterwards.
std::atomic<bool> foundServer{false};
} // namespace anonymous

async::result<void> enumerateServer() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "vsock"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while(true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for(auto &event : events) {
			if(event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			std::println("posix: Found vsock server");
			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			serverLane = (co_await entity.getRemoteLane()).unwrap();
			foundServer.store(true, std::memory_order_release);
			co_return;
		}
	}
}

std::optional<helix::BorrowedLane> getLane() {
	if(!foundServer.load(std::memory_order_acquire))
		return std::nullopt;
	return serverLane;
}

} // namespace vsock
