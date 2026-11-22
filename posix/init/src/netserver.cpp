#include <core/device-path.hpp>
#include <filesystem>
#include <format>
#include <helix/ipc.hpp>
#include <print>
#include <string.h>

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

async::result<void> bindNetserver(Reactor &reactor, std::string devicePath) {
	UeventStream uevents{reactor};

	auto parser = DevicePathParser::fromString(devicePath);
	if(!parser) {
		std::println("init: failed to parse device path '{}'", devicePath);
		co_return;
	}

	// Empty until all devices along the device path are published.
	std::filesystem::path sysfsPath;
	while(true) {
		auto uevent = co_await uevents.next();

		if(sysfsPath.empty()) {
			auto sysfs = parser->sysfs();
			if(sysfs) {
				sysfsPath = sysfs.value();
			} else if(sysfs.error() == DevicePathParser::Error::broken) {
				std::println("init: device path '{}' cannot be resolved, not starting netserver",
						devicePath);
				co_return;
			}
		}

		if(!sysfsPath.empty()
				&& std::format("/sys{}", lookup(uevent, "DEVPATH")) == sysfsPath.string()) {
			auto mbusId = lookup(uevent, "MBUS_ID");
			if(mbusId.empty())
				std::println("init: Device {} has no MBUS_ID, not starting netserver", devicePath);
			else if(auto e = bindServer("netserver", mbusId); !e)
				std::println("init: Failed to bind netserver: {}", strerror(e.error()));
			co_return;
		}
	}
}
