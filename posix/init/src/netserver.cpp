#include <format>
#include <helix/ipc.hpp>
#include <print>
#include <string.h>
#include <utility>

#include "netserver.hpp"
#include "spawn.hpp"

std::expected<mbus_ng::EntityId, mbus_ng::Error> findNetserver() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "netserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while(true) {
		auto result = async::run(enumerator.nextEvents(), helix::currentDispatcher);
		if(!result)
			return std::unexpected{result.error()};

		for(auto &event : result.value().events) {
			if(event.type == mbus_ng::EnumerationEvent::Type::created)
				return event.id;
		}
	}
}

NetserverBinder::NetserverBinder(Reactor &reactor, std::string devicePath, DevicePathParser parser)
: UeventRule{reactor}, devicePath_{std::move(devicePath)}, parser_{std::move(parser)} {}

bool NetserverBinder::handle(const Uevent &uevent) {
	if(sysfsPath_.empty()) {
		auto sysfs = parser_.sysfs();
		if(sysfs) {
			sysfsPath_ = sysfs.value();
		} else if(sysfs.error() == DevicePathParser::Error::broken) {
			std::println("init: device path '{}' cannot be resolved, not starting netserver",
					devicePath_);
			done_.raise();
			return false;
		}
	}

	if(!sysfsPath_.empty()
			&& std::format("/sys{}", lookup(uevent, "DEVPATH")) == sysfsPath_.string()) {
		auto mbusId = lookup(uevent, "MBUS_ID");
		if(mbusId.empty())
			std::println("init: Device {} has no MBUS_ID, not starting netserver", devicePath_);
		else if(auto e = bindServer("netserver", mbusId); !e)
			std::println("init: Failed to bind netserver: {}", strerror(e.error()));
		done_.raise();
		return false;
	}
	return true;
}

async::result<void> NetserverBinder::wait() {
	co_await done_.wait();
}
