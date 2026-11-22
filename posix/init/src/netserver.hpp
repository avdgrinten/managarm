#pragma once

#include <async/oneshot-event.hpp>
#include <async/result.hpp>
#include <core/device-path.hpp>
#include <expected>
#include <filesystem>
#include <protocols/mbus/client.hpp>
#include <string>

#include "reactor.hpp"

// Returns the mbus entity of netserver. Blocks until netserver shows up on mbus.
std::expected<mbus_ng::EntityId, mbus_ng::Error> findNetserver();

// Binds netserver to the NIC that is identified by a UEFI device path.
class NetserverBinder final : public UeventRule {
public:
	// parser is the parsed form of devicePath.
	NetserverBinder(Reactor &reactor, std::string devicePath, DevicePathParser parser);

	bool handle(const Uevent &uevent) override;

	// Completes once netserver is bound or it is clear that it cannot be bound.
	async::result<void> wait();

private:
	std::string devicePath_;
	DevicePathParser parser_;
	// Empty until all devices along the device path are published.
	std::filesystem::path sysfsPath_;
	async::oneshot_event done_;
};
