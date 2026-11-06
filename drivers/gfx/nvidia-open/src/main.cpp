#include <core/clock.hpp>
#include <core/logging.hpp>
#include <core/smbios.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <print>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <pthread.h>
#include <unistd.h>

extern "C" {

#include <nvtypes.h>
#include <nv.h>
#include <nv-reg.h>
#include <nvlink_export.h>
#include <nvkms.h>
#include <nvkms-kapi.h>
#include <nvkms-rmapi.h>
#include <nvkms-utils.h>
#include <os-interface.h>

extern const char *const pNV_KMS_ID;

}

#include "gfx.hpp"

extern "C" sem_t nvKmsLock;

namespace {

nvidia_stack_t *sp[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
std::unordered_map<int64_t, std::shared_ptr<GfxDevice>> baseDeviceMap;
std::map<std::pair<uintptr_t, size_t>, helix::UniqueDescriptor> mmioRanges;
std::deque<WorkqueueItem> workqueue_ = {};
std::vector<uint8_t> smbiosTable;

} // namespace

const struct NvKmsKapiFunctionsTable *nvKms;

pthread_mutex_t workqueueLock_ = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t workqueueCond_ = PTHREAD_COND_INITIALIZER;

async::result<void> workqueueThread() {
	while (true) {
		pthread_mutex_lock(&workqueueLock_);
		while (workqueue_.empty()) {
			pthread_cond_wait(&workqueueCond_, &workqueueLock_);
		}

		auto item = workqueue_.front();
		workqueue_.pop_front();
		pthread_mutex_unlock(&workqueueLock_);

		item.func(item.arg);
	}
}

void workqueueAdd(workqueue_func_t func, void *arg) {
	auto item = WorkqueueItem{func, arg};

	pthread_mutex_lock(&workqueueLock_);
	workqueue_.push_back(item);
	pthread_cond_signal(&workqueueCond_);
	pthread_mutex_unlock(&workqueueLock_);
}

uint8_t GfxDevice::getNvidiaBarIndex(uint8_t nv_bar_index) {
	assert(nv_bar_index < NV_GPU_NUM_BARS);

	uint8_t bar_index = 0;

	for (size_t i = 0; i < 6; i++) {
		if (bar_index == nv_bar_index)
			return i;

		if (info_.barInfo[i].hostType != protocols::hw::kIoTypeNone)
			bar_index++;
	}

	logPanic("gfx/nvidia-open: attempted to resolve invalid nv_bar_index {}", nv_bar_index);
}

std::pair<size_t, helix::BorrowedDescriptor> GfxDevice::accessMmio(uintptr_t address, size_t len) {
	std::optional<std::pair<std::weak_ptr<GfxDevice>, uint8_t>> target = std::nullopt;

	for (auto &[region, dev] : mmioRanges) {
		if (address >= region.first && (address + len) <= (region.first + region.second))
			return {address - region.first, dev};
	}

	logPanic("bar not found");
}

void GfxDevice::pciRead(uint32_t reg, uint32_t *value) {
	auto run = [this, reg]() -> async::result<uint32_t> {
		uint32_t ret = co_await hwDevice_.loadPciSpace(reg, 4);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

void GfxDevice::pciRead(uint32_t reg, uint16_t *value) {
	auto run = [this, reg]() -> async::result<uint16_t> {
		uint16_t ret = co_await hwDevice_.loadPciSpace(reg, 2);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

void GfxDevice::pciRead(uint32_t reg, uint8_t *value) {
	auto run = [this, reg]() -> async::result<uint8_t> {
		uint8_t ret = co_await hwDevice_.loadPciSpace(reg, 1);
		co_return ret;
	};

	*value = async::run(run(), helix::currentDispatcher);
}

void GfxDevice::pciWrite(uint32_t reg, uint32_t value) {
	auto run = [this, reg, value]() -> async::result<void> {
		co_await hwDevice_.storePciSpace(reg, 4, value);
	};

	async::run(run(), helix::currentDispatcher);
}

void GfxDevice::pciWrite(uint32_t reg, uint16_t value) {
	auto run = [this, reg, value]() -> async::result<void> {
		co_await hwDevice_.storePciSpace(reg, 2, value);
	};

	async::run(run(), helix::currentDispatcher);
}

void GfxDevice::pciWrite(uint32_t reg, uint8_t value) {
	auto run = [this, reg, value]() -> async::result<void> {
		co_await hwDevice_.storePciSpace(reg, 1, value);
	};

	async::run(run(), helix::currentDispatcher);
}

GfxDevice::GfxDevice(
    protocols::hw::Device hw_device,
    uint32_t seg,
    uint32_t bus,
    uint32_t dev,
    uint32_t func,
    bool iommuActive,
    helix::UniqueDescriptor dmaSpaceHandle
)
: hwDevice_{std::move(hw_device)},
  iommuActive_{iommuActive},
  segment_{seg},
  bus_{bus},
  slot_{dev},
  function_{func},
  dmaSpaceHandle_{std::move(dmaSpaceHandle)},
  dmaSpace_{dmaRealm_.attachDmaSpace(dmaSpaceHandle_, iommuActive)} {
	fb2ModifiersSupport = true;
}

std::shared_ptr<GfxDevice> GfxDevice::getGpu(size_t gpuId) {
	for (auto [mbusId, g] : baseDeviceMap) {
		if (g->nv_.gpu_id == gpuId)
			return g;
	}

	return {};
}

std::atomic<pid_t> irqThreadTid = 0;

namespace {

void *irqHandler(void *arg) {
	async::run(reinterpret_cast<GfxDevice *>(arg)->handleIrqs(), helix::currentDispatcher);

	return nullptr;
}

void *rcTimerHandler(void *arg) {
	async::run(reinterpret_cast<GfxDevice *>(arg)->rcTimer(), helix::currentDispatcher);

	return nullptr;
}

void *workqueueHandler(void *) {
	async::run(workqueueThread(), helix::currentDispatcher);

	return nullptr;
}

} // namespace

std::atomic<bool> irqHigherHalf = false;

async::result<void> GfxDevice::handleIrqs() {
	msi_ = co_await hwDevice_.installMsi(0);
	nv_.flags |= NV_FLAG_USES_MSI;

	irqThreadTid = gettid();

	sem_post(&irqInitSem_);

	uint64_t seq = 0;

	while (true) {
		auto awaitResult = co_await helix_ng::awaitEvent(msi_, seq);
		HEL_CHECK(awaitResult.error());

		// call ISR
		irqHigherHalf = true;

		uint32_t rm_serviceable_fault_cnt = 0;
		rm_gpu_handle_mmu_faults(nullptr, &nv_, &rm_serviceable_fault_cnt);
		bool rm_fault_handling_needed = (rm_serviceable_fault_cnt != 0);

		uint32_t need_to_run_bottom_half_gpu_lock_held = 0;
		[[maybe_unused]] bool rm_handled =
		    rm_isr(nullptr, &nv_, &need_to_run_bottom_half_gpu_lock_held);

		assert(!rm_fault_handling_needed);

		irqHigherHalf = false;

		seq = awaitResult.sequence();

		HEL_CHECK(helAcknowledgeIrq(msi_.getHandle(), kHelAckAcknowledge, seq));

		if (need_to_run_bottom_half_gpu_lock_held)
			rm_isr_bh(nullptr, &nv_);
	}
}

async::result<void> GfxDevice::rcTimer() {
	bool continueWaiting = true;

	while (true) {
		pthread_mutex_lock(&timerLock);
		while (!nv_.rc_timer_enabled || !continueWaiting) {
			pthread_cond_wait(&timerCond, &timerLock);

			if (nv_.rc_timer_enabled)
				continueWaiting = true;
		}
		pthread_mutex_unlock(&timerLock);

		co_await helix::sleepFor(1'000'000'000);

		pthread_mutex_lock(&timerLock);
		bool stillEnabled = nv_.rc_timer_enabled;
		pthread_mutex_unlock(&timerLock);

		if (!stillEnabled)
			continue;

		if (auto status = rm_run_rc_callback(nullptr, &nv_); status != NV_OK) {
			continueWaiting = false;
		}
	}
}

void GfxDevice::setupCrtcAndPlanes(NvKmsKapiDeviceResourcesInfo &resInfo) {
	std::vector<uint64_t> modifiers;
	auto gen = (resInfo.caps.genericPageKind == 0x06) ? 2 : 0;

	for (size_t i = 0; i <= 5; i++)
		modifiers.push_back(
		    DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, gen, resInfo.caps.genericPageKind, 5 - i)
		);

	modifiers.push_back(DRM_FORMAT_MOD_LINEAR);

	for (size_t i = 0; i < resInfo.numHeads; i++) {
		if (resInfo.numLayers[i] <= NVKMS_KAPI_LAYER_PRIMARY_IDX)
			continue;

		auto primaryPlane = std::make_shared<Plane>(
		    this, drm_core::Plane::PlaneType::PRIMARY, NVKMS_KAPI_LAYER_PRIMARY_IDX
		);
		primaryPlane->setupWeakPtr(primaryPlane);

		auto ppState = std::make_shared<drm_core::PlaneState>(drm_core::PlaneState(primaryPlane));

		uint32_t supported_rotations = primaryPlane->supportedRotations(resInfo, i);
		std::println(
		    "gfx/nvidia-open: layer {} supported rotations bitmap 0x{:x}", i, supported_rotations
		);

		auto crtc = std::make_shared<Crtc>(this, i, primaryPlane);
		crtc->setupWeakPtr(crtc);
		auto crtcState = std::make_shared<Crtc::CrtcState>(crtc);
		crtc->setDrmState(crtcState);

		ppState->crtc = crtc;

		primaryPlane->clearFormats();
		auto formats = Plane::getDrmFormats(resInfo.supportedSurfaceMemoryFormats[0]);
		for (auto format : formats)
			primaryPlane->addFormat(format);

		if (formats.size() && modifiers.size()) {
			size_t formatsSize = sizeof(uint32_t) * formats.size();
			size_t modifiersSize = sizeof(drm_format_modifier) * modifiers.size();
			size_t inFormatBlobSize =
			    sizeof(drm_format_modifier_blob) + formatsSize + modifiersSize;

			std::vector<char> inFormat(inFormatBlobSize);
			drm_format_modifier_blob blobHead{
			    .version = FORMAT_BLOB_CURRENT,
			    .flags = 0,
			    .count_formats = static_cast<uint32_t>(formats.size()),
			    .formats_offset = sizeof(drm_format_modifier_blob),
			    .count_modifiers = static_cast<uint32_t>(modifiers.size()),
			    .modifiers_offset =
			        static_cast<uint32_t>(sizeof(drm_format_modifier_blob) + formatsSize),
			};
			memcpy(inFormat.data(), &blobHead, sizeof(blobHead));
			memcpy(inFormat.data() + sizeof(blobHead), formats.data(), formatsSize);
			for (size_t i = 0; i < modifiers.size(); i++) {
				drm_format_modifier mod{
				    .formats = ((1ULL << formats.size()) - 1),
				    .offset = 0,
				    .modifier = modifiers.at(i),
				};
				memcpy(
				    inFormat.data() + sizeof(blobHead) + formatsSize
				        + (sizeof(drm_format_modifier) * i),
				    &mod,
				    sizeof(mod)
				);
			}
			ppState->in_formats = registerBlob(inFormat);
		}

		primaryPlane->setDrmState(std::move(ppState));
		primaryPlane->setupPossibleCrtcs({crtc.get()});

		registerObject(primaryPlane.get());
		registerObject(crtc.get());

		for (size_t layer = 0; layer < resInfo.numLayers[i]; layer++) {
			if (layer == NVKMS_KAPI_LAYER_PRIMARY_IDX)
				continue;

			std::println("gfx/nvidia-open: skipping creation of overlay plane");
		}

		setupCrtc(crtc.get());

		planes_.push_back(primaryPlane);
		crtcs_.push_back(crtc);
	}
}

void GfxDevice::setupConnectorsAndEncoders() {
	NvU32 nDisplays = 0;
	auto success = nvKms->getDisplays(kmsdev, &nDisplays, nullptr);
	assert(success);

	std::vector<NvKmsKapiDisplay> hDisplays(nDisplays);

	success = nvKms->getDisplays(kmsdev, &nDisplays, hDisplays.data());
	if (!success) {
		std::println("gfx/nvidia-open: failed to get displays");
		return;
	}

	for (size_t i = 0; i < nDisplays; i++) {
		NvKmsKapiStaticDisplayInfo displayInfo = {};

		success = nvKms->getStaticDisplayInfo(kmsdev, hDisplays[i], &displayInfo);
		if(!success)
			continue;

		NvKmsKapiConnectorInfo connectorInfo = {};

		success = nvKms->getConnectorInfo(kmsdev, displayInfo.connectorHandle, &connectorInfo);
		if (!success)
			continue;

		auto encoder = std::make_shared<Encoder>(this, displayInfo.handle);
		encoder->setupWeakPtr(encoder);
		encoder->setupEncoderType(Encoder::getSignalFormat(connectorInfo.signalFormat));

		std::vector<drm_core::Crtc *> possibleCrtcs;

		for (auto crtc : crtcs_) {
			if (displayInfo.headMask & (1 << crtc->headId())) {
				possibleCrtcs.push_back(crtc.get());
			}
		}

		auto connector = Connector::find(
		    this,
		    connectorInfo.physicalIndex,
		    connectorInfo.type,
		    displayInfo.internal,
		    displayInfo.dpAddress
		);
		connector->addPossibleEncoder(encoder.get());

		encoder->setCurrentConnector(connector.get());
		encoder->setupPossibleCrtcs(possibleCrtcs);

		registerObject(encoder.get());
		setupEncoder(encoder.get());

		encoders_.push_back(encoder);
	}
}

async::result<void> GfxDevice::initialize() {
	info_ = co_await hwDevice_.getPciInfo();
	auto bar0 = co_await hwDevice_.accessBar(0);

	helix::Mapping mapping{bar0, info_.barInfo[0].offset, info_.barInfo[0].length};
	regs_ = {mapping.get()};

	apertureHandle_ = co_await hwDevice_.accessBar(1);

	auto vendor_dev = co_await hwDevice_.loadPciSpace(pci::Vendor, 4);
	auto class_code = co_await hwDevice_.loadPciSpace(pci::Revision, 4);
	auto subsystem = co_await hwDevice_.loadPciSpace(pci::SubsystemVendor, 4);

	vendor_ = vendor_dev & 0xFFFF;
	device_ = vendor_dev >> 16;
	subsystemVendor_ = subsystem & 0xFFFF;
	subsystemDevice_ = subsystem >> 16;
	classCode_ = (class_code >> 24) & 0xFF;
	subclassCode_ = (class_code >> 16) & 0xFF;
	progIf_ = (class_code >> 8) & 0xFF;

	// Claim the device before we start reprogramming it
	// (otherwise thor will continue writing to the boot framebuffer).
	co_await hwDevice_.claimDevice();

	if (!rm_wait_for_bar_firewall(nullptr, segment_, bus_, slot_, function_, device_)) {
		fprintf(stderr, "NVRM: failed to wait for bar firewall to lower\n");
		co_return;
	}

	auto supported = rm_is_supported_pci_device(
	    (class_code >> 24) & 0xFF,
	    (class_code >> 16) & 0xFF,
	    vendor_dev & 0xFFFF,
	    vendor_dev >> 16,
	    subsystem & 0xFFFF,
	    subsystem >> 16,
	    NV_TRUE /* print_legacy_warning */
	);
	std::println("gfx/nvidia-open: device supported={}", supported);
	assert(supported);

	nv_.pci_info.domain = segment_;
	nv_.pci_info.bus = bus_;
	nv_.pci_info.slot = slot_;
	nv_.pci_info.function = function_;
	nv_.pci_info.vendor_id = vendor_;
	nv_.pci_info.device_id = device_;
	nv_.subsystem_vendor = subsystemVendor_;
	nv_.subsystem_id = subsystemDevice_;
	nv_.os_state = this;
	nv_.handle = this;
	nv_.dma_dev = reinterpret_cast<nv_dma_device_t *>(this);
	nv_.cpu_numa_node_id = -1;
	nv_.interrupt_line = 0;

	size_t nvBarIndex = 0;

	for (size_t i = 0; i < 6; i++) {
		if (info_.barInfo[i].hostType == protocols::hw::kIoTypeNone)
			continue;

		if (info_.barInfo[i].hostType == protocols::hw::kIoTypeMemory) {
			nv_.bars[nvBarIndex].offset = info_.barInfo[i].offset;
			nv_.bars[nvBarIndex].cpu_address = info_.barInfo[i].address;
			nv_.bars[nvBarIndex].size = info_.barInfo[i].length;
			nv_.bars[nvBarIndex].map = nullptr;
			nv_.bars[nvBarIndex].map_u = nullptr;
			nvBarIndex++;

			mmioRanges.insert(
			    {{info_.barInfo[i].address, info_.barInfo[i].length},
			     co_await hwDevice_.accessBar(i)}
			);
		}
	}

	nv_.regs = &nv_.bars[NV_GPU_BAR_INDEX_REGS];
	nv_.fb = &nv_.bars[NV_GPU_BAR_INDEX_FB];

	NV_STATUS status = rm_is_supported_device(nullptr, &nv_);
	assert(status == NV_OK);

	auto success = rm_init_private_state(nullptr, &nv_);
	assert(success);

	rm_set_rm_firmware_requested(nullptr, &nv_);
	rm_enable_dynamic_power_management(nullptr, &nv_);

	/*
	 * This must be the last action in probe(). Do not add code after this line.
	 */
	rm_notify_gpu_addition(nullptr, &nv_);

	rm_unref_dynamic_power(nullptr, &nv_, NV_DYNAMIC_PM_FINE);

	sem_init(&nvKmsLock, 0, 1);

	static struct NvKmsKapiFunctionsTable nvKmsFuncsTable = {
	    .versionString = NV_VERSION_STRING,
	};

	nvKms = &nvKmsFuncsTable;

	assert(nvKmsKapiGetFunctionsTableInternal(&nvKmsFuncsTable));

	nvKmsModuleLoad();

	struct NvKmsKapiAllocateDeviceParams params{
	    .gpuId = nv_.gpu_id,
	    .privateData = this,
	    .eventCallback = &GfxDevice::eventCallback,
	};
	kmsdev = nvKms->allocateDevice(&params);

	success = nvKms->grabOwnership(kmsdev);
	assert(success);

	nvKms->framebufferConsoleDisabled(kmsdev);

	NvKmsKapiDeviceResourcesInfo resInfo;
	success = nvKms->getDeviceResourcesInfo(kmsdev, &resInfo);
	assert(success);

	setupMinDimensions(resInfo.caps.minWidthInPixels, resInfo.caps.minHeightInPixels);
	setupMaxDimensions(resInfo.caps.maxWidthInPixels, resInfo.caps.maxHeightInPixels);
	setupCursorDimensions(resInfo.caps.maxCursorSizeInPixels, resInfo.caps.maxCursorSizeInPixels);

	pitchAlignment_ = resInfo.caps.pitchAlignment;
	hasVideoMemory_ = resInfo.caps.hasVideoMemory;

	// setup CRTCs and planes
	setupCrtcAndPlanes(resInfo);

	// setup Connectors and Encoders
	setupConnectorsAndEncoders();

	success = nvKms->declareEventInterest(kmsdev,
		((1 << NVKMS_EVENT_TYPE_DPY_CHANGED) |
		 (1 << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) |
		 (1 << NVKMS_EVENT_TYPE_FLIP_OCCURRED)));
	assert(success);

	co_return;
}

async::result<void> GfxDevice::open() {
	if (nv_.flags & NV_FLAG_OPEN)
		co_return;

	if (!adapterInitialized_) {
		rm_ref_dynamic_power(nullptr, &nv_, NV_DYNAMIC_PM_COARSE);

		co_await hwDevice_.enableMsi();

		sem_init(&irqInitSem_, 0, 0);

		pthread_t irq;
		int ret = pthread_create(&irq, nullptr, irqHandler, this);
		assert(ret == 0);

		sem_wait(&irqInitSem_);

		pthread_t rcTimer;
		ret = pthread_create(&rcTimer, nullptr, rcTimerHandler, this);
		assert(ret == 0);

		auto success = rm_init_adapter(nullptr, &nv_);
		assert(success);

		adapterInitialized_ = true;
	}

	nv_.flags |= NV_FLAG_OPEN;

	rm_request_dnotifier_state(nullptr, &nv_);
}

void GfxDevice::eventCallback(const struct NvKmsKapiEvent *event) {
	auto nv_dev = reinterpret_cast<GfxDevice *>(event->privateData);

	switch (event->type) {
		case NVKMS_EVENT_TYPE_DPY_CHANGED: {
			auto res = std::ranges::find_if(nv_dev->encoders_, [&event](auto e) {
				return e->handle() == event->u.displayChanged.display;
			});
			assert(res != nv_dev->encoders_.end());
			std::println("gfx/nvidia-open: hotplug of display {}", event->u.displayChanged.display);

			auto enc = *(res);
			auto con [[maybe_unused]] = reinterpret_cast<Connector *>(enc->currentConnector());

			// TODO: update encoder states, get updated mode list, report hotplug event

			break;
		}
		case NVKMS_EVENT_TYPE_FLIP_OCCURRED: {
			auto gfx = reinterpret_cast<GfxDevice *>(event->privateData);
			gfx->flipEvent.raise();
			break;
		}
		default:
			std::println("gfx/nvidia-open: unhandled event {}", uint32_t(event->type));
	}
}

// ----------------------------------------------------------------
//
// ----------------------------------------------------------------

async::result<void>
bindController(mbus_ng::Entity hwEntity, uint32_t seg, uint32_t bus, uint32_t dev, uint32_t func) {
	std::println("gfx/nvidia-open: Setting up NVRM");

	auto id = hwEntity.id();
	protocols::hw::Device hwDevice((co_await hwEntity.getRemoteLane()).unwrap());

	co_await hwDevice.enableBusmaster();
	co_await hwDevice.enableDma(false);
	auto dmaSpaceInfo = co_await hwDevice.getDmaSpace();
	if(!dmaSpaceInfo) {
		std::println("gfx/nvidia-open: Failed to get DMA space");
		co_return;
	}
	auto [iommuActive, dmaSpaceHandle, _] = std::move(*dmaSpaceInfo);

	auto gfxDevice = std::make_shared<GfxDevice>(std::move(hwDevice), seg, bus, dev, func, iommuActive, std::move(dmaSpaceHandle));
	baseDeviceMap.insert({id, gfxDevice});
	co_await gfxDevice->initialize();

	// Create an mbus object for the device.
	mbus_ng::Properties descriptor{
	    {"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(hwEntity.id())}},
	    {"unix.subsystem", mbus_ng::StringItem{"drm"}},
	    {"unix.devname", mbus_ng::StringItem{"dri/card"}}
	};

	auto gfxEntity =
	    (co_await mbus_ng::Instance::global().createEntity("gfx_nvidia_open", descriptor)).unwrap();

	[](auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			drm_core::serveDrmDevice(device, std::move(localLane));
		}
	}(gfxDevice, std::move(gfxEntity));

	std::cout << "gfx/nvidia-open: setup complete!" << std::endl;
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if (baseDeviceMap.find(baseEntity.id()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	std::cout << "gfx/nvidia-open: Binding to device " << base_id << std::endl;
	// Make sure that we only bind to supported devices.
	auto properties = (co_await baseEntity.getProperties()).unwrap();
	if (auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
	    !vendor_str || vendor_str->value != "10de")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if (auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-class"]);
	    !device_str || device_str->value != "03")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if (auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-subclass"]);
	    !device_str || device_str->value != "00")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if (auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-interface"]);
	    !device_str || device_str->value != "00")
		co_return protocols::svrctl::Error::deviceNotSupported;

	auto pciSegment =
	    std::stoul(std::get<mbus_ng::StringItem>(properties["pci-segment"]).value, nullptr, 16);
	auto pciBus =
	    std::stoul(std::get<mbus_ng::StringItem>(properties["pci-bus"]).value, nullptr, 16);
	auto pciSlot =
	    std::stoul(std::get<mbus_ng::StringItem>(properties["pci-slot"]).value, nullptr, 16);
	auto pciFunction =
	    std::stoul(std::get<mbus_ng::StringItem>(properties["pci-function"]).value, nullptr, 16);

	co_await bindController(std::move(baseEntity), pciSegment, pciBus, pciSlot, pciFunction);
	co_return protocols::svrctl::Error::success;
}

async::result<void> enumerateSmbios() {
	auto filter = mbus_ng::Conjunction({
	    mbus_ng::EqualsFilter{"unix.subsystem", "firmware"},
	    mbus_ng::EqualsFilter{"firmware.type", "smbios"},
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			if (!event.properties.contains("version")
			    || std::get_if<mbus_ng::StringItem>(&event.properties.at("version"))->value
			           != "3") {
				co_return;
			}

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
			smbiosTable = co_await device.getSmbiosTable();

			co_return;
		}
	}
}

uint8_t getSmbiosChassis() {
	if(smbiosTable.empty())
		return 0x02; // Unknown

	auto entry = getSmbiosEntry({smbiosTable.data(), smbiosTable.size()}, 3);
	if(entry.size() >= 0x0D) // 0x0D is the minimum size for version >= 2.1
		return entry[5] & 0x7F; // bit 7 is set for chassis lock

	return 0x02; // Unknown
}

static constexpr protocols::svrctl::ControlOperations controlOps = {.bind = bindDevice};

int main() {
	printf("gfx/nvidia-open: Starting driver\n");

	// set up clock access
	async::run(clk::enumerateTracker(), helix::currentDispatcher);
	async::run(enumerateSmbios(), helix::currentDispatcher);

	pthread_t workqueue;
	auto ret = pthread_create(&workqueue, nullptr, workqueueHandler, nullptr);
	assert(ret == 0);

	nvlink_lib_initialize();

	if (!rm_init_rm(*sp)) {
		std::println("gfx/nvidia-open: rm_init_rm() failed!");
		return 1;
	}

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}
