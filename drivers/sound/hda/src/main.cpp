#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <uhda/uhda.h>

#include "controller.hpp"

namespace {

struct PciDeviceId {
	uint16_t vendor;
	uint16_t device;
};

constexpr auto uhdaDevices = std::to_array<PciDeviceId>({UHDA_MATCHING_DEVICES});

std::vector<std::unique_ptr<Controller>> globalControllers;

} // namespace

async::detached handleIrqs(helix::BorrowedDescriptor irq, UhdaIrqHandlerFn fn, void *arg) {
	uint64_t irqSequence = 0;

	while (true) {
		auto awaitResult = co_await helix_ng::awaitEvent(irq, irqSequence);

		HEL_CHECK(awaitResult.error());
		irqSequence = awaitResult.sequence();

		bool handled = fn(arg);

		if (handled) {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, irqSequence));
		} else {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, irqSequence));
		}
	}
}

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device dev{(co_await entity.getRemoteLane()).unwrap()};

	auto info = co_await dev.getPciInfo();
	co_await dev.enableBusmaster();
	co_await dev.enableDma(false);
	auto [iommuActive, dmaSpace, _] = co_await dev.getDmaSpace();

	auto controller = std::make_unique<Controller>(std::move(dev), info.numMsis != 0, std::move(dmaSpace), iommuActive);

	UhdaController *uhdaCtrl;
	auto status = uhda_init(reinterpret_cast<uintptr_t>(controller.get()), &uhdaCtrl);
	assert(status == UHDA_STATUS_SUCCESS);

	const UhdaCodec *const *codecs;
	size_t codecCount;
	uhda_get_codecs(uhdaCtrl, &codecs, &codecCount);

	const UhdaOutput *chosenOutput;

	for (size_t i = 0; i < codecCount; ++i) {
		auto codec = codecs[i];

		const UhdaOutputGroup *const *outputGroups;
		size_t outputGroupCount;
		uhda_codec_get_output_groups(codec, &outputGroups, &outputGroupCount);

		for(size_t j = 0; j < outputGroupCount; ++j) {
			auto group = outputGroups[j];

			const UhdaOutput *const *outputs;
			size_t outputCount;
			uhda_output_group_get_outputs(group, &outputs, &outputCount);

			for(size_t k = 0; k < outputCount; ++k) {
				auto output = outputs[k];

				bool presence;
				status = uhda_output_get_presence(output, &presence);
				if(status == UHDA_STATUS_UNSUPPORTED) {
					presence = true;
				} else {
					assert(status == UHDA_STATUS_SUCCESS);
				}

				if(!presence) {
					continue;
				}

				auto info = uhda_output_get_info(output);
				if(info.type == UHDA_OUTPUT_TYPE_LINE_OUT ||
					info.type == UHDA_OUTPUT_TYPE_HEADPHONE) {
					chosenOutput = output;
					break;
				}
			}

			if(chosenOutput) {
				break;
			}
		}

		if (chosenOutput) {
			break;
		}
	}

	if (!chosenOutput) {
		uhda_destroy(uhdaCtrl);
		co_return;
	}

	UhdaPath *path;
	status = uhda_find_path(chosenOutput, nullptr, 0, false, &path);
	assert(status == UHDA_STATUS_SUCCESS);

	controller->uhda = uhdaCtrl;
	controller->codecs = codecs;
	controller->codecCount = codecCount;

	UhdaStream** outputStreams;
	size_t outputStreamCount;
	uhda_get_output_streams(uhdaCtrl, &outputStreams, &outputStreamCount);

	for (size_t i = 0; i < outputStreamCount; ++i) {
		auto hdaStream = std::make_unique<Stream>(outputStreams[i], false);
		controller->playbackStreams.push_back(std::move(hdaStream));
	}

	co_await controller->run(1);

	co_await controller->mbusPublishedEvent.wait();
	auto device = std::make_unique<Device>(controller.get(), path, sound::DeviceType::playback, controller->mbusId);
	sound::runDevice(device.get());
	controller->devices.push_back(std::move(device));

	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto classFilter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", std::format("{:02x}", UHDA_MATCHING_CLASS)},
		mbus_ng::EqualsFilter{"pci-subclass", std::format("{:02x}", UHDA_MATCHING_SUBCLASS)}
	}};

	std::vector<mbus_ng::AnyFilter> filters;
	filters.push_back(std::move(classFilter));

	for (auto [vendor, device] : uhdaDevices) {
		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"pci-vendor", std::format("{:04x}", vendor)},
			mbus_ng::EqualsFilter{"pci-device", std::format("{:04x}", device)}
		}};

		filters.push_back(std::move(filter));
	}

	auto filter = mbus_ng::Disjunction{std::move(filters)};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "sound/hda: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

int main() {
	std::cout << "sound/hda: Starting driver" << std::endl;

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}
