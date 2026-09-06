#include <string.h>
#include <iostream>

#include <core/id-allocator.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../vfs.hpp"

namespace generic_subsystem {

namespace {

id_allocator<uint32_t> minorAllocator{0};

struct Device final : UnixDevice, std::enable_shared_from_this<Device> {
	Device(VfsType type, std::string name, helix::UniqueLane lane)
	: UnixDevice{type},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return _name;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	FutureMaybe<smarter::shared_ptr<FsLink, LinkRc>> mount(std::string fs_type) override {
		return mountExternalDevice(_lane, shared_from_this(), fs_type);
	}

private:
	std::string _name;
	helix::UniqueLane _lane;
};

std::unordered_map<std::string, id_allocator<uint64_t>> deviceIdAllocators;
std::unordered_map<mbus_ng::EntityId, std::pair<std::string, uint64_t>> deviceIdMap;

uint64_t allocateDeviceIds(mbus_ng::EntityId entity_id, std::string prefix) {
	if(!deviceIdAllocators.contains(prefix)) {
		id_allocator<uint64_t> new_alloc{0};
		deviceIdAllocators.insert({prefix, new_alloc});
	}

	auto id = deviceIdAllocators.at(prefix).allocate();
	deviceIdMap.insert({entity_id, {prefix, id}});

	return id;
}

async::detached observeDevices(VfsType devType, auto &registry, int major) {
	const char *typeStr = devType == VfsType::blockDevice ? "block" : "char";

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter {"generic.devtype", typeStr}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			auto name = std::get<mbus_ng::StringItem>(event.properties.at("generic.devname"));
			auto id = allocateDeviceIds(entity.id(), name.value);

			auto sysfs_name = name.value + std::to_string(id);

			std::cout << "POSIX: Installing " << typeStr << " device "
					<< sysfs_name << std::endl;

			auto lane = (co_await entity.getRemoteLane()).unwrap();
			auto device = std::make_shared<Device>(devType,
					sysfs_name, std::move(lane));

			// We use a fixed major here, and allocate minors sequentially.
			device->assignId({major, minorAllocator.allocate()});
			registry.install(device);
		}
	}
}

} // anonymous namepsace

std::optional<std::pair<std::string, uint64_t>> getDeviceName(mbus_ng::EntityId id) {
	if(deviceIdMap.contains(id)) {
		return deviceIdMap.at(id);
	}

	return std::nullopt;
}

void run() {
	observeDevices(VfsType::blockDevice, blockRegistry, 240);
	observeDevices(VfsType::charDevice, charRegistry, 234);
}

} // namespace generic_subsystem
