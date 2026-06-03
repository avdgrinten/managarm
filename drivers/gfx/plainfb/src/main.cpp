
#include <assert.h>
#include <protocols/posix/procdata.hpp>
#include <stdio.h>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "plainfb.hpp"
#include <fs.bragi.hpp>

// ----------------------------------------------------------------
// GfxDevice.
// ----------------------------------------------------------------

GfxDevice::GfxDevice(protocols::hw::Device hw_device,
		unsigned int screen_width, unsigned int screen_height,
		size_t screen_pitch, helix::Mapping fb_mapping)
: _hwDevice{std::move(hw_device)},
		_screenWidth{screen_width}, _screenHeight{screen_height},
		_screenPitch{screen_pitch},_fbMapping{std::move(fb_mapping)} {
	if((reinterpret_cast<uintptr_t>(_fbMapping.get()) & 15)) {
		std::cout << "\e[31m" "gfx/plainfb: Hardware framebuffer is not aligned;"
				" expect perfomance degradation!" "\e[39m" << std::endl;
		_hardwareFbIsAligned = false;
	}
}

async::result<std::unique_ptr<drm_core::Configuration>> GfxDevice::initialize() {
	// Setup planes, encoders and CRTCs (i.e. the static entities).
	_plane = std::make_shared<Plane>(this, Plane::PlaneType::PRIMARY);
	_theCrtc = std::make_shared<Crtc>(this);
	_theEncoder = std::make_shared<Encoder>(this);

	_plane->setupWeakPtr(_plane);
	_theCrtc->setupWeakPtr(_theCrtc);
	_theEncoder->setupWeakPtr(_theEncoder);

	_theEncoder->setupPossibleCrtcs({_theCrtc.get()});
	_theEncoder->setupPossibleClones({_theEncoder.get()});
	_theEncoder->setCurrentCrtc(_theCrtc.get());

	_theCrtc->setupState(_theCrtc);
	_plane->setupState(_plane);
	_plane->setupPossibleCrtcs({_theCrtc.get()});

	std::vector<drm_core::Assignment> assignments;

	assignments.push_back(drm_core::Assignment::withInt(_theCrtc, activeProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, planeTypeProperty(), 1));

	registerObject(_plane.get());
	registerObject(_theCrtc.get());
	registerObject(_theEncoder.get());

	auto [dumb_fb, dumb_pitch] = createDumb(_screenWidth, _screenHeight, 32);
	auto fb = createFrameBuffer(dumb_fb, _screenWidth, _screenHeight, 0, dumb_pitch, DRM_FORMAT_MOD_LINEAR);

	assignments.push_back(drm_core::Assignment::withModeObj(_plane, crtcIdProperty(), _theCrtc));
	assignments.push_back(drm_core::Assignment::withInt(_plane, srcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, srcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, crtcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, crtcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, srcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, srcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, crtcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_plane, crtcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withModeObj(_plane, fbIdProperty(), fb));

	setupCrtc(_theCrtc.get());
	setupEncoder(_theEncoder.get());

	// Setup the connector.
	_theConnector = std::make_shared<Connector>(this);
	_theConnector->setupWeakPtr(_theConnector);
	_theConnector->setupState(_theConnector);

	_theConnector->setupPossibleEncoders({_theEncoder.get()});
	_theConnector->setCurrentEncoder(_theEncoder.get());
	_theConnector->setCurrentStatus(1);

	registerObject(_theConnector.get());
	attachConnector(_theConnector.get());

	setupMinDimensions(_screenWidth, _screenHeight);
	setupMaxDimensions(_screenWidth, _screenHeight);

	assignments.push_back(drm_core::Assignment::withInt(_theConnector, dpmsProperty(), 3));
	assignments.push_back(drm_core::Assignment::withModeObj(_theConnector, crtcIdProperty(), _theCrtc));

	std::vector<drm_mode_modeinfo> supported_modes;
	drm_core::addDmtModes(supported_modes, _screenWidth, _screenHeight);
	_theConnector->setModeList(supported_modes);

	// Connector::setModeList() sorts the modes in descending order of (width * height)
	auto info_ptr = reinterpret_cast<const char *>(&_theConnector->modeList().front());
	std::vector<char> modeData(info_ptr, info_ptr + sizeof(drm_mode_modeinfo));
	auto modeBlob = registerBlob(std::move(modeData));

	assignments.push_back(drm_core::Assignment::withBlob(_theCrtc, modeIdProperty(), modeBlob));

	auto config = createConfiguration();
	auto state = atomicState();
	assert(config->capture(assignments, state));
	config->commit(std::move(state));

	co_return config;
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t, uint32_t pitch, uint32_t mod [[maybe_unused]]) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);

	assert(pitch % 4 == 0);

	assert(pitch / 4 >= width);
	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pitch);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {0, 0, 1};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"plainfb_gpu", "plainfb gpu", "0"};
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
GfxDevice::createDumb(uint32_t width, uint32_t height, uint32_t bpp) {
	HelHandle handle;
	auto size = ((width * height * bpp / 8) + (4096 - 1)) & ~(4096 - 1);
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), size, 0, nullptr, &handle));

	auto bo = std::make_shared<BufferObject>(this, size,
			helix::UniqueDescriptor(handle), width, height);
	uint32_t pitch = width * bpp / 8;

	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);

	return std::make_pair(bo, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignments, std::unique_ptr<drm_core::AtomicState> &state) {
	for(auto &assign : assignments) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);
	}

	auto plane_state = state->plane(_device->_plane->id());
	auto crtc_state = state->crtc(_device->_theCrtc->id());

	if(crtc_state->mode != nullptr) {
		// TODO: Consider current width/height if FB did not change.
		drm_mode_modeinfo mode_info;
		memcpy(&mode_info, crtc_state->mode->data(), sizeof(drm_mode_modeinfo));
		plane_state->src_h = mode_info.vdisplay;
		plane_state->src_w = mode_info.hdisplay;

		// TODO: Check max dimensions: plane_state->width > 1024 || plane_state->height > 768
		if(plane_state->src_w <= 0 || plane_state->src_h <= 0) {
			std::cout << "\e[31m" "gfx/plainfb: invalid state width of height" << "\e[39m" << std::endl;
			return false;
		}
	}
	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> state) {
	_device->_theCrtc->setDrmState(state->crtc(_device->_theCrtc->id()));
	_device->_theConnector->setDrmState(state->connector(_device->_theConnector->id()));
	_device->_plane->setDrmState(state->plane(_device->_plane->id()));

	_dispatch(std::move(state));
}

async::detached GfxDevice::Configuration::_dispatch(std::unique_ptr<drm_core::AtomicState> state) {
	auto crtc_state = state->crtc(_device->_theCrtc->id());

	if(crtc_state->mode != nullptr) {
		if(!_device->_claimedDevice) {
			co_await _device->_hwDevice.claimDevice();
			_device->_claimedDevice = true;
		}

		auto plane_state = state->plane(_device->_plane->id());

		if(plane_state->fb != nullptr) {
			auto fb = static_pointer_cast<GfxDevice::FrameBuffer>(plane_state->fb);

			auto bo = fb->getBufferObject();
			auto dest = reinterpret_cast<char *>(_device->_fbMapping.get());
			auto src = reinterpret_cast<char *>(bo->accessMapping());

			auto minWidth = std::min(bo->getWidth(), _device->_screenWidth);
			auto minHeight = std::min(bo->getHeight(), _device->_screenHeight);

			if(fb->fastScanout()) {
				for(unsigned int k = 0; k < minHeight; k++) {
					drm_core::fastCopy16(dest, src, minWidth * 4);
					dest += _device->_screenPitch;
					src += fb->getPitch();
				}
			}else{
				for(unsigned int k = 0; k < minHeight; k++) {
					memcpy(dest, src, minWidth * 4);
					dest += _device->_screenPitch;
					src += fb->getPitch();
				}
			}
		}
	} else {
		std::cout << "gfx/plainfb: Disable scanout" << std::endl;
	}

	complete();
}

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_core::Connector { device, device->allocator.allocate() } {
//	_encoders.push_back(device->_theEncoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder.
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *device)
	:drm_core::Encoder { device, device->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc.
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *device)
: drm_core::Crtc{device, device->allocator.allocate()} {
	_device = device;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _device->_plane.get();
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device,
		std::shared_ptr<GfxDevice::BufferObject> bo, size_t pitch)
: drm_core::FrameBuffer{device, device->allocator.allocate()},
		_device{device}, _bo{std::move(bo)}, _pitch{pitch} {
	if(!_device->_hardwareFbIsAligned) {
		_fastScanout = false;
	}else if((reinterpret_cast<uintptr_t>(_bo->accessMapping()) & 15)
			|| ((_bo->getWidth() * 4) & 15)) {
		std::cout << "\e[31m" "gfx/plainfb: Framebuffer is not aligned!" "\e[39m" << std::endl;
		_fastScanout = false;
	}
}

size_t GfxDevice::FrameBuffer::getPitch() {
	return _pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

void GfxDevice::FrameBuffer::notifyDirty() {
	// TODO: Re-blit the FrameBuffer if it is currently displayed.
	std::cout << "gfx/plainfb: notifyDirty() is not implemented correctly" << std::endl;
}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device, PlaneType type)
: drm_core::Plane{device, device->allocator.allocate(), type} { }

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *device,
		size_t size, helix::UniqueDescriptor memory,
		uint32_t width, uint32_t height)
: drm_core::BufferObject{width, height}, _size{size}, _memory{std::move(memory)} {
	(void)device;
	_bufferMapping = helix::Mapping{_memory, 0, getSize()};
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

void *GfxDevice::BufferObject::accessMapping() {
	return _bufferMapping.get();
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_memory), 0);
}

// ----------------------------------------------------------------
//
// ----------------------------------------------------------------

async::detached bindController(mbus_ng::Entity hwEntity) {
	protocols::hw::Device hwDevice((co_await hwEntity.getRemoteLane()).unwrap());

	auto info = co_await hwDevice.getFbInfo();
	auto fbMemory = co_await hwDevice.accessFbMemory();
	std::cout << "gfx/plainfb: Resolution " << info.width
			<< "x" << info.height << " (" << info.bpp
			<< " bpp, pitch: " << info.pitch << ")" << std::endl;
	assert(info.bpp == 32);

	auto gfxDevice = std::make_shared<GfxDevice>(std::move(hwDevice),
			info.width, info.height, info.pitch,
			helix::Mapping{fbMemory, 0, info.pitch * info.height});
	auto config = co_await gfxDevice->initialize();

	// Create an mbus object for the device.
	mbus_ng::Properties descriptor{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(hwEntity.id())}},
		{"unix.subsystem", mbus_ng::StringItem{"drm"}},
		{"unix.devname", mbus_ng::StringItem{"dri/card"}}
	};

	co_await config->waitForCompletion();

	auto gfxEntity = (co_await mbus_ng::Instance::global().createEntity(
		"gfx_plainfb", descriptor)).unwrap();

	[] (auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			drm_core::serveDrmDevice(device, std::move(localLane));
		}
	}(gfxDevice, std::move(gfxEntity));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "framebuffer"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "gfx/plainfb: Detected device" << std::endl;
			bindController(std::move(entity));
		}
	}
}

int main() {
	std::cout << "gfx/plainfb: Starting driver" << std::endl;

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

