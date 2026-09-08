#include <format>
#include <print>
#include <iostream>

#include <core/id-allocator.hpp>
#include <core/logging.hpp>
#include <protocols/mbus/client.hpp>

#include "sound.hpp"
#include "../device.hpp"
#include "../drvcore.hpp"

namespace sound_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> cardAllocator{0};
id_allocator<uint32_t> minorAllocator{0};

}

struct CardDevice final : drvcore::ClassDevice {
	CardDevice(int index)
	: drvcore::ClassDevice{sysfsSubsystem, nullptr, std::format("card{}", index), nullptr} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "sound");
	}

	std::optional<std::string> getClassPath() override {
		return "sound";
	}
};

struct ControlDevice final : UnixDevice, drvcore::ClassDevice {
	ControlDevice(int index, helix::UniqueLane lane, std::shared_ptr<CardDevice> parent)
	: UnixDevice{VfsType::charDevice},
			drvcore::ClassDevice{sysfsSubsystem, std::move(parent), std::format("controlC{}", index), this},
			index_{index}, lane_{std::move(lane)} { }

	std::string nodePath() override {
		return std::format("snd/controlC{}", index_);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(lane_, std::move(mount), std::move(link), semantic_flags);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "sound");
	}

	std::optional<std::string> getClassPath() override {
		return "sound";
	}

	int index() const {
		return index_;
	}

	id_allocator<uint32_t> deviceIdAllocator{0};

private:
	int index_;
	helix::UniqueLane lane_;
};

struct PcmDevice final : UnixDevice, drvcore::ClassDevice {
	PcmDevice(int cardIndex, int index, bool playback, helix::UniqueLane lane, std::shared_ptr<CardDevice> parent)
	: UnixDevice{VfsType::charDevice},
			drvcore::ClassDevice{sysfsSubsystem, std::move(parent),
						std::format("pcmC{}D{}{}", cardIndex, index, playback ? 'p' : 'c'), this},
			cardIndex_{cardIndex}, index_{index}, playback_{playback}, lane_{std::move(lane)} { }

	std::string nodePath() override {
		return std::format("snd/pcmC{}D{}{}", cardIndex_, index_, playback_ ? 'p' : 'c');
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(lane_, std::move(mount), std::move(link), semantic_flags);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "sound");
	}

	std::optional<std::string> getClassPath() override {
		return "sound";
	};

private:
	int cardIndex_;
	int index_;
	bool playback_;
	helix::UniqueLane lane_;
};

namespace {

struct CardDevices {
	std::shared_ptr<CardDevice> card;
	std::shared_ptr<ControlDevice> control;
	uint64_t remainingChildDevices;
};

std::unordered_map<mbus_ng::EntityId, CardDevices> cardMbusMap;

}

async::result<void> installPcmDevice(mbus_ng::Entity entity, const std::string &soundDeviceType,
		ControlDevice *controlDevice, std::shared_ptr<CardDevice> cardDevice) {
	auto lane = (co_await entity.getRemoteLane()).unwrap();

	int cardIndex = controlDevice->index();
	auto deviceIndex = controlDevice->deviceIdAllocator.allocate();

	std::shared_ptr<PcmDevice> pcmDevice;
	if (soundDeviceType == "playback") {
		pcmDevice = std::make_shared<PcmDevice>(cardIndex, deviceIndex, true, std::move(lane), cardDevice);
	} else if (soundDeviceType == "capture") {
		pcmDevice = std::make_shared<PcmDevice>(cardIndex, deviceIndex, false, std::move(lane), cardDevice);
	} else {
		logPanic("posix: unsupported sound device type '{}' (mbus ID {})", soundDeviceType, entity.id());
	}

	pcmDevice->assignId({116, minorAllocator.allocate()});

	charRegistry.install(pcmDevice);
	drvcore::registerMbusDevice(entity.id(), pcmDevice);
	drvcore::installDevice(pcmDevice);
}

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"sound"};

	auto filter = mbus_ng::Disjunction{{
		mbus_ng::EqualsFilter{"class", "sound-card"},
		mbus_ng::EqualsFilter{"class", "sound-device"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto type = std::get<mbus_ng::StringItem>(event.properties.at("class")).value;

			if (type == "sound-card") {
				int cardIndex = cardAllocator.allocate();
				std::println(std::cout, "posix: Installing sound card snd/card{}", cardIndex);

				auto cardDevice = std::make_shared<CardDevice>(cardIndex);
				drvcore::installDevice(cardDevice);

				auto lane = (co_await entity.getRemoteLane()).unwrap();
				auto controlDevice = std::make_shared<ControlDevice>(cardIndex, std::move(lane), cardDevice);

				controlDevice->assignId({116, minorAllocator.allocate()});

				auto &remainingChildDevicesStr =
						std::get<mbus_ng::StringItem>(event.properties.at("sound.num-devices")).value;

				CardDevices cardDevices{
					.card{cardDevice},
					.control{controlDevice},
					.remainingChildDevices = std::stoull(remainingChildDevicesStr),
				};
				cardMbusMap.insert({entity.id(), std::move(cardDevices)});
			} else if (type == "sound-device") {
				auto &parentIdStr = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
				mbus_ng::EntityId parentId = std::stoi(parentIdStr);

				auto &cardDevice = cardMbusMap.at(parentId);

				auto &soundDeviceType = std::get<mbus_ng::StringItem>(event.properties.at("sound.type")).value;
				co_await installPcmDevice(std::move(entity), soundDeviceType, cardDevice.control.get(), cardDevice.card);
				cardDevice.remainingChildDevices--;

				if (cardDevice.remainingChildDevices == 0) {
					charRegistry.install(cardDevice.control);
					drvcore::registerMbusDevice(parentId, cardDevice.control);
					drvcore::installDevice(cardDevice.control);
				}
			} else {
				std::println(std::cout, "posix: unsupported sound device class '{}' (mbus ID {})", type, entity.id());
			}
		}
	}
}

} // namespace sound_subsystem
