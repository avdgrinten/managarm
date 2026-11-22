#include <assert.h>
#include <core/device-path.hpp>
#include <filesystem>
#include <helix/ipc.hpp>
#include <print>
#include <string.h>

#include "netserver.hpp"
#include "spawn.hpp"

mbus_ng::EntityId findNetserver() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "netserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = async::run(enumerator.nextEvents(), helix::currentDispatcher).unwrap();
	assert(events.size() == 1);

	return events[0].id;
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
		const auto &devpath = uevent.at("DEVPATH");

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

		if(!sysfsPath.empty() && "/sys" + devpath == sysfsPath.string()) {
			if(auto e = bindServer("netserver", uevent.at("MBUS_ID")); !e)
				std::println("init: Failed to bind netserver: {}", strerror(e.error()));
			co_return;
		}
	}
}
