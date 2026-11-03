#pragma once

#include <arch/dma_pool.hpp>
#include <arch/mem_space.hpp>
#include <core/drm/device.hpp>
#include <protocols/hw/client.hpp>
#include <semaphore.h>

extern "C" {
#include <nv.h>
#include <nvkms-api.h>
#include <nvkms-kapi.h>
#include <nvtypes.h>
}

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
#pragma mark Configuration
	struct Configuration final : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device) { };

		bool capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> & state) override;
		void dispose() override;
		void commit(std::unique_ptr<drm_core::AtomicState> state) override;
		async::detached dispatch(std::unique_ptr<drm_core::AtomicState> state);

	private:
		GfxDevice *_device;
	};

#pragma mark Support Objects
	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, size_t size, helix::UniqueDescriptor mem, NvKmsKapiMemory *memHandle,
			uint32_t width, uint32_t height);

		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;

		NvKmsKapiMemory *memHandle() const { return _memHandle; }

	private:
		size_t _size;
		helix::UniqueDescriptor _mem;

		NvKmsKapiMemory *_memHandle;
	};

#pragma mark Mode Objects
	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device, PlaneType type, size_t layerIndex);

		size_t layerIndex() const { return layerIndex_; }

		static std::vector<uint32_t> getDrmFormats(uint64_t mask);
		static uint32_t supportedRotations(NvKmsKapiDeviceResourcesInfo &info, size_t layer);

	private:
		size_t layerIndex_;
	};

	struct FrameBuffer final : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch, NvKmsKapiSurface *surface, uint32_t mod);

		GfxDevice::BufferObject *getBufferObject();
		uint32_t getPixelPitch();
		void notifyDirty() override;
		uint32_t getWidth() override;
		uint32_t getHeight() override;
		uint32_t getModifier() override;

		NvKmsKapiSurface *surface() const {
			return _surface;
		}

	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
		NvKmsKapiSurface *_surface;
		uint32_t _modifier;
	};

	struct Encoder;

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device, NvKmsConnectorType type, bool internal,
			size_t physicalIndex, char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH]);

		// static utilities
		static int getConnectorType(NvKmsConnectorType type, NvBool internal) {
			switch (type) {
				default:
				case NVKMS_CONNECTOR_TYPE_UNKNOWN:
					return DRM_MODE_CONNECTOR_Unknown;
				case NVKMS_CONNECTOR_TYPE_DP:
					return internal ? DRM_MODE_CONNECTOR_eDP : DRM_MODE_CONNECTOR_DisplayPort;
				case NVKMS_CONNECTOR_TYPE_HDMI:
					return DRM_MODE_CONNECTOR_HDMIA;
				case NVKMS_CONNECTOR_TYPE_DVI_D:
					return DRM_MODE_CONNECTOR_DVID;
				case NVKMS_CONNECTOR_TYPE_DVI_I:
					return DRM_MODE_CONNECTOR_DVII;
				case NVKMS_CONNECTOR_TYPE_LVDS:
					return DRM_MODE_CONNECTOR_LVDS;
				case NVKMS_CONNECTOR_TYPE_VGA:
					return DRM_MODE_CONNECTOR_VGA;
				case NVKMS_CONNECTOR_TYPE_DSI:
					return DRM_MODE_CONNECTOR_DSI;
				case NVKMS_CONNECTOR_TYPE_DP_SERIALIZER:
					return DRM_MODE_CONNECTOR_DisplayPort;
			}
		}

		static std::shared_ptr<Connector> find(GfxDevice *dev, size_t physicalIndex, NvKmsConnectorType type,
			bool internal, char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH]);

		// accessors
		NvKmsConnectorType type() const { return type_; }

		bool internal() const { return internal_; }

		size_t physicalIndex() const { return physicalIndex_; }

		std::span<const char> dpAddress() const {
			return std::span<const char>{dpAddress_, NVKMS_DP_ADDRESS_STRING_LENGTH};
		}

		// utilities
		void updateModeList();
		NvKmsKapiDynamicDisplayParams *updateDisplayInfo(NvKmsKapiDisplay handle);

		// base class overrides
		async::result<void> probe() override;

		Encoder *detectedEncoder = nullptr;

	private:
		GfxDevice *_device;
		NvKmsConnectorType type_;
		bool internal_;
		size_t physicalIndex_;
		char dpAddress_[NVKMS_DP_ADDRESS_STRING_LENGTH];
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device, NvKmsKapiDisplay handle);

		static int getSignalFormat(NvKmsConnectorSignalFormat format) {
			switch (format) {
				default:
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_UNKNOWN:
					return DRM_MODE_ENCODER_NONE;
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_TMDS:
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_DP:
					return DRM_MODE_ENCODER_TMDS;
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_LVDS:
					return DRM_MODE_ENCODER_LVDS;
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_VGA:
					return DRM_MODE_ENCODER_DAC;
				case NVKMS_CONNECTOR_SIGNAL_FORMAT_DSI:
					return DRM_MODE_ENCODER_DSI;
			}
		}

		NvKmsKapiDisplay handle() const { return handle_; }

	private:
		NvKmsKapiDisplay handle_;
	};

	struct Crtc final : drm_core::Crtc {
		Crtc(GfxDevice *device, size_t headId, std::shared_ptr<GfxDevice::Plane> primary);

		drm_core::Plane *primaryPlane() override;
		drm_core::Plane *cursorPlane() override;

		size_t headId() const { return headId_; }

		struct CrtcState final : drm_core::CrtcState {
			CrtcState(std::weak_ptr<Crtc> crtc) : drm_core::CrtcState(crtc) {}

			std::shared_ptr<drm_core::CrtcState> clone() const override {
				auto cs = std::make_shared<CrtcState>(*this);
				cs->activeChanged = false;
				cs->modeChanged = false;
				return cs;
			}

			NvKmsKapiHeadRequestedConfig params;
		};

	private:
		GfxDevice *_device;

		size_t headId_;
		std::shared_ptr<GfxDevice::Plane> primaryPlane_;
	};

	GfxDevice(protocols::hw::Device device, uint32_t seg, uint32_t bus, uint32_t dev, uint32_t func, bool iommuActive, helix::UniqueDescriptor dmaSpaceHandle);

	std::unique_ptr<drm_core::Configuration> createConfiguration() override;

	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
	createDumb(uint32_t width, uint32_t height, uint32_t bpp) override;

	std::shared_ptr<drm_core::FrameBuffer>
	createFrameBuffer(std::shared_ptr<drm_core::BufferObject> buff,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch, uint32_t mod) override;

	std::tuple<int, int, int> driverVersion() override;
	std::tuple<std::string, std::string, std::string> driverInfo() override;

	async::result<void> initialize();
	async::result<void> open();

	async::result<void> handleIrqs();
	async::result<void> rcTimer();

	static std::shared_ptr<GfxDevice> getGpu(size_t gpuId);
	static std::pair<size_t, helix::BorrowedDescriptor> accessMmio(uintptr_t address, size_t len);

	void pciRead(uint32_t reg, uint32_t *value);
	void pciRead(uint32_t reg, uint16_t *value);
	void pciRead(uint32_t reg, uint8_t *value);

	void pciWrite(uint32_t reg, uint32_t value);
	void pciWrite(uint32_t reg, uint16_t value);
	void pciWrite(uint32_t reg, uint8_t value);

private:
	uint8_t getNvidiaBarIndex(uint8_t nv_bar_index);

	void setupCrtcAndPlanes(NvKmsKapiDeviceResourcesInfo &resInfo);
	void setupConnectorsAndEncoders();

	static void eventCallback(const struct NvKmsKapiEvent *event);

	protocols::hw::Device hwDevice_;
	arch::mem_space regs_;
	protocols::hw::PciInfo info_;
	helix::UniqueDescriptor msi_;
	nv_state_t nv_;

	std::vector<std::shared_ptr<Crtc>> crtcs_;
	std::vector<std::shared_ptr<Plane>> planes_;
	std::vector<std::shared_ptr<Encoder>> encoders_;
	std::vector<std::shared_ptr<Connector>> connectors_;
	std::vector<std::shared_ptr<BufferObject>> bos_;

	helix::UniqueDescriptor apertureHandle_;

	async::recurring_event flipEvent;

	sem_t irqInitSem_;

public:
	pthread_mutex_t timerLock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t timerCond = PTHREAD_COND_INITIALIZER;

	bool iommuActive_ = false;

	arch::contiguous_pool *pool() {
		return &pool_;
	}

	arch::dma_space &space() {
		return dmaSpace_;
	}

	arch::dma_realm &dmaRealm() {
		return dmaRealm_;
	}

private:
	NvKmsKapiDevice *kmsdev = nullptr;

	bool adapterInitialized_ = false;
	bool hasVideoMemory_;
	uint32_t pitchAlignment_;

	uint32_t segment_;
	uint32_t bus_;
	uint32_t slot_;
	uint32_t function_;

	uint16_t vendor_;
	uint16_t device_;
	uint8_t classCode_;
	uint8_t subclassCode_;
	uint8_t progIf_;
	uint16_t subsystemVendor_;
	uint16_t subsystemDevice_;

	arch::dma_realm dmaRealm_;
	arch::contiguous_pool pool_{&dmaRealm_, {.addressBits = 32, .allocateContigous = false}};
	helix::UniqueDescriptor dmaSpaceHandle_;
	arch::dma_space dmaSpace_;
};

typedef void (*workqueue_func_t)(void *);

void workqueueAdd(workqueue_func_t, void *arg);

struct WorkqueueItem {
	workqueue_func_t func;
	void *arg;
};

struct AllocInfo {
	HelHandle handle;
	arch::imported_dma_buffer buf;
	size_t page_count;
	uintptr_t base;
};

uint8_t getSmbiosChassis();
