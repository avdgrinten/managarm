
#include <linux/input.h>
#include <string.h>
#include <iostream>
#include <sstream>

#include <core/id-allocator.hpp>
#include <protocols/mbus/client.hpp>

#include "../device.hpp"
#include "../drvcore.hpp"
#include "../vfs.hpp"
#include "usb/usb.hpp"
#include "fs.bragi.hpp"

namespace input_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> evdevAllocator{0};

struct CapabilityAttribute : sysfs::Attribute {
	CapabilityAttribute(std::string name, int index, size_t bits)
	: sysfs::Attribute{std::move(name), false}, _index{index}, _bits{bits} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;

private:
	int _index;
	size_t _bits;
};

CapabilityAttribute evCapability{"ev", 0, EV_MAX};
CapabilityAttribute keyCapability{"key", EV_KEY, KEY_MAX};
CapabilityAttribute relCapability{"rel", EV_REL, REL_MAX};
CapabilityAttribute absCapability{"abs", EV_ABS, ABS_MAX};

struct EventDevice final : UnixDevice, drvcore::ClassDevice {
	EventDevice(VfsType type, int index, std::shared_ptr<Device> parent, helix::UniqueLane lane)
	: UnixDevice{type},
			drvcore::ClassDevice{sysfsSubsystem, parent, "event" + std::to_string(index), this},
			_index{index}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return "input/event" + std::to_string(_index);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "input");
	}

	std::optional<std::string> getClassPath() override {
		return "input";
	};

private:
	int _index;
	helix::UniqueLane _lane;
};

struct InputDevice final : drvcore::ClassDevice {
	InputDevice(int index, std::shared_ptr<Device> parent)
	: drvcore::ClassDevice{sysfsSubsystem, std::move(parent), "input" + std::to_string(index), nullptr} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "input");
	}

	std::optional<std::string> getClassPath() override {
		return "input";
	};
};

async::result<frg::expected<Error, std::string>> CapabilityAttribute::show(sysfs::Object *object) {
	auto device = static_cast<EventDevice *>(object);
	auto fileMaybe = co_await device->open(nullptr, nullptr, nullptr, 0);
	if(!fileMaybe){
		std::cout << "\e[31mposix: show(): open() error\e[39m" << std::endl;
		while(true);
	}
	auto file = fileMaybe.value(); //TODO: properly handle error returns here
	auto lane = file->getPassthroughLane();

	std::vector<uint64_t> buffer;
	buffer.resize((_bits + 63) & ~size_t(63));

	managarm::fs::IoctlRequest ioctl_req;
	managarm::fs::GenericIoctlRequest req;
	if(_index) {
		req.set_command(EVIOCGBIT(1, 0));
		req.set_input_type(_index);
	}else{
		req.set_command(EVIOCGBIT(0, 0));
	}
	req.set_size(buffer.size() * sizeof(uint64_t));

	auto ser = req.SerializeAsString();
	auto [offer, send_ioctl_req, send_req, recv_resp, recv_data]
			= co_await helix_ng::exchangeMsgs(lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(ioctl_req, frg::stl_allocator{}),
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::recvBuffer(buffer.data(), buffer.size() * sizeof(uint64_t))
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_ioctl_req.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_data.error());

	managarm::fs::GenericIoctlReply resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	std::stringstream ss;
	ss << std::hex;
	bool suffix = false;
	for(auto it = buffer.rbegin(); it != buffer.rend(); it++) {
		if(!(*it) && !suffix)
			continue;
		if(it != buffer.rbegin())
			ss << ' ';
		ss << *it;
		suffix = true;
	}

	co_return ss.str();
}

} // anonymous namepsace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"input"};

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"unix.subsystem", "input"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
			auto parent_dev = drvcore::getMbusDevice(std::stoi(parent_id));

			if(event.properties.contains("usb.parent-interface"))
				parent_dev = co_await usb_subsystem::getInterfaceDevice(parent_dev, event.properties);

			int index = evdevAllocator.allocate();
			std::cout << "POSIX: Installing input device input/event" << index << std::endl;

			auto inputDevice = std::make_shared<InputDevice>(index, parent_dev);
			drvcore::installDevice(inputDevice);

			auto lane = (co_await entity.getRemoteLane()).unwrap();
			auto device = std::make_shared<EventDevice>(VfsType::charDevice,
					index, inputDevice, std::move(lane));
			device->assignId({13, 64 + index}); // evdev devices start at minor 64.

			charRegistry.install(device);
			drvcore::installDevice(device);

			// TODO: Do this before the device becomes visible in sysfs!
			auto link = device->directoryNode()->directMkdir(device->dirLink().get(), "capabilities");
			auto caps = static_cast<sysfs::DirectoryNode *>(link->getTarget().get());
			caps->directMkattr(link.get(), device.get(), &evCapability);
			caps->directMkattr(link.get(), device.get(), &keyCapability);
			caps->directMkattr(link.get(), device.get(), &relCapability);
			caps->directMkattr(link.get(), device.get(), &absCapability);
		}
	}
}

} // namespace input_subsystem
