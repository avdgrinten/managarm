
#include <string.h>
#include <iostream>
#include <string_view>
#include <linux/fs.h>

#include <core/id-allocator.hpp>
#include <protocols/mbus/client.hpp>

#include "../device.hpp"
#include "../vfs.hpp"
#include "../drvcore.hpp"
#include "helpers.hpp"

namespace block_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> minorAllocator{0};
std::unordered_map<std::string, id_allocator<uint32_t>> idAllocators;
std::unordered_map<int64_t, std::string> diskNames;

std::unordered_set<std::string_view> alphabetizedIds = {
	"sd",
};

struct Device final : UnixDevice, drvcore::BlockDevice, std::enable_shared_from_this<Device> {
	Device(VfsType type, std::string name, helix::UniqueLane lane,
			std::shared_ptr<drvcore::Device> parent, size_t size)
	: UnixDevice{type},
			drvcore::BlockDevice{sysfsSubsystem, std::move(parent), name,
					this},
			_name{std::move(name)}, _lane{std::move(lane)}, _size{size} { }

	std::string nodePath() override {
		return _name;
	}

	size_t size() {
		return _size;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	FutureMaybe<smarter::shared_ptr<FsLink, LinkRc>> mount(std::string fs_type) override {
		return mountExternalDevice(_lane, shared_from_this(), fs_type);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		std::pair<int, int> dev = getId();
		ue.set("SUBSYSTEM", "block");
		ue.set("MAJOR", std::to_string(dev.first));
		ue.set("MINOR", std::to_string(dev.second));
	}

private:
	std::string _name;
	helix::UniqueLane _lane;

	size_t _size;
};

} // anonymous namepsace

struct ReadOnlyAttribute : sysfs::Attribute {
	ReadOnlyAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SizeAttribute : sysfs::Attribute {
	SizeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct ManagarmRootAttribute : sysfs::Attribute {
	ManagarmRootAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

ReadOnlyAttribute roAttr{"ro"};
DevAttribute<Device> devAttr{"dev"};
SizeAttribute sizeAttr{"size"};
ManagarmRootAttribute managarmRootAttr{"managarm-root"};

async::result<frg::expected<Error, std::string>> ReadOnlyAttribute::show(sysfs::Object *object) {
	(void) object;
	// The format is 0\n\0.
	// Hardcode to zero as we don't support ro mounts yet.
	co_return "0\n";
}

async::result<frg::expected<Error, std::string>> SizeAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::to_string(device->size() / 512) + "\n";
}

async::result<frg::expected<Error, std::string>> ManagarmRootAttribute::show(sysfs::Object *) {
	co_return "1\n";
}

async::detached observePartitions() {
	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.devtype", "block"},
		mbus_ng::EqualsFilter{"unix.blocktype", "partition"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto &properties = event.properties;

			auto diskEntityId = std::stoll(std::get<mbus_ng::StringItem>(properties.at("unix.diskid")).value);
			auto diskSuffix = std::get<mbus_ng::StringItem>(properties.at("unix.partname-suffix")).value;
			auto diskId = diskNames.at(diskEntityId);
			auto partId = std::get<mbus_ng::StringItem>(properties.at("unix.partid")).value;

			auto name = diskId + diskSuffix + partId;
			std::cout << "POSIX: Installing block device " << name << std::endl;

			auto parent_property = std::get<mbus_ng::StringItem>(properties.at("drvcore.mbus-parent"));
			auto mbus_parent = std::stoi(parent_property.value);
			std::shared_ptr<drvcore::Device> parent_device;
			if (mbus_parent != -1) {
				parent_device = drvcore::getMbusDevice(mbus_parent);
				assert(parent_device);
			}

			auto lane = (co_await entity.getRemoteLane()).unwrap();

			managarm::fs::GenericIoctlRequest req;
			req.set_command(BLKGETSIZE64);

			auto ser = req.SerializeAsString();
			auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline()
				)
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());

			managarm::fs::GenericIoctlReply resp;
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			recv_resp.reset();
			assert(resp.error() == managarm::fs::Errors::SUCCESS);

			size_t size = resp.size();

			auto device = std::make_shared<Device>(VfsType::blockDevice,
					name,
					std::move(lane),
					parent_device,
					size);
			// We use 8 here, the major for SCSI devices and allocate minors sequentially.
			// Note that this is not really correct as the minor of a partition
			// depends on the minor of the whole device (see Linux devices.txt documentation).
			device->assignId({8, minorAllocator.allocate()});
			blockRegistry.install(device);
			drvcore::installDevice(device);

			// TODO: Call realizeAttribute *before* installing the device.
			device->realizeAttribute(&roAttr);
			device->realizeAttribute(&devAttr);
			device->realizeAttribute(&sizeAttr);
			if (std::get<mbus_ng::StringItem>(properties.at("unix.is-managarm-root")).value == "1")
				device->realizeAttribute(&managarmRootAttr);
		}
	}
}

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"block"};

	observePartitions();

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.devtype", "block"},
		mbus_ng::EqualsFilter{"unix.blocktype", "disk"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto diskPrefix = std::get<mbus_ng::StringItem>(event.properties.at("unix.diskname-prefix")).value;
			auto diskSuffix = std::get<mbus_ng::StringItem>(event.properties.at("unix.diskname-suffix")).value;

			if(!idAllocators.contains(diskPrefix))
				idAllocators.insert({diskPrefix, {0}});

			int diskId = idAllocators.at(diskPrefix).allocate();

			if(alphabetizedIds.contains(diskPrefix)) {
				constexpr std::string_view alphabet{"abcdefghijklmnopqrstuvwxyz"};
				assert(static_cast<size_t>(diskId) < alphabet.size());
				diskNames.emplace(event.id, diskPrefix + alphabet[diskId]);
			} else {
				diskNames.emplace(event.id, diskPrefix + std::to_string(diskId));
			}

			auto parent_property = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent"));
			auto mbus_parent = std::stoi(parent_property.value);
			std::shared_ptr<drvcore::Device> parent_device;
			if (mbus_parent != -1) {
				parent_device = drvcore::getMbusDevice(mbus_parent);
				assert(parent_device);
			}

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto lane = (co_await entity.getRemoteLane()).unwrap();

			managarm::fs::GenericIoctlRequest req;
			req.set_command(BLKGETSIZE64);

			auto ser = req.SerializeAsString();
			auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline()
				)
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());

			managarm::fs::GenericIoctlReply resp;
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			recv_resp.reset();
			assert(resp.error() == managarm::fs::Errors::SUCCESS);

			size_t size = resp.size();

			auto device = std::make_shared<Device>(VfsType::blockDevice,
				diskNames.at(event.id) + diskSuffix,
				std::move(lane),
				parent_device,
				size);

			device->assignId({8, minorAllocator.allocate()});
			blockRegistry.install(device);
			drvcore::installDevice(device);
		}
	}
}

} // namespace block_subsystem
