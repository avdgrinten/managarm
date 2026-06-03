
#include <assert.h>
#include <protocols/posix/procdata.hpp>
#include <stdio.h>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "vmware.hpp"

#include <fs.bragi.hpp>

std::unordered_map<int64_t, std::shared_ptr<GfxDevice>> baseDeviceMap;

// ----------------------------------------------------------------
// GfxDevice
// ----------------------------------------------------------------

uint32_t GfxDevice::readRegister(register_index reg) {
	_operational.store(ports::register_port, (uint32_t)reg);
	return _operational.load(ports::value_port);
}

void GfxDevice::writeRegister(register_index reg, uint32_t val) {
	_operational.store(ports::register_port, (uint32_t)reg);
	_operational.store(ports::value_port, val);
}

GfxDevice::GfxDevice(protocols::hw::Device hw_dev,
			helix::Mapping fb_mapping,
			helix::Mapping fifo_mapping,
			helix::UniqueDescriptor io_bar, uint16_t io_base)
		: _hwDev(std::move(hw_dev)), _fifo{this, std::move(fifo_mapping)},_fbMapping{std::move(fb_mapping)}, _isClaimed{false}, _deviceVersion{0} {
	HEL_CHECK(helEnableIo(io_bar.getHandle()));
	_operational = arch::io_space{io_base};
}

async::result<std::unique_ptr<drm_core::Configuration>> GfxDevice::initialize() {
	auto pci_info = co_await _hwDev.getPciInfo();

	// negotiate version
	_deviceVersion = versions::id_2;

	do {
		writeRegister(register_index::id, _deviceVersion);
		if (readRegister(register_index::id) == _deviceVersion) {
			break;
		}

		_deviceVersion--;
	} while (_deviceVersion >= versions::id_0);

	assert(_deviceVersion >= versions::id_0 && "failed to negotiate version with device");

	_deviceCaps = _deviceVersion >= versions::id_1 ? readRegister(register_index::capabilities) : 0;

	// configure fifo
	_fifo.initialize();

	if (_deviceCaps & (uint32_t)caps::irqmask) {
		writeRegister(register_index::irqmask, 0);
		_operational.store(ports::irq_status_port, 0xFF);
	} else {
		printf("\e[35mgfx/vmware: device doesn't support interrupts\e[39m\n");
	}

	auto current_w = readRegister(register_index::width);
	auto current_h = readRegister(register_index::height);

	// device should be working from this point forward

	std::vector<drm_core::Assignment> assignments;

	_crtc = std::make_shared<Crtc>(this);
	_crtc->setupWeakPtr(_crtc);
	_crtc->setupState(_crtc);
	_encoder = std::make_shared<Encoder>(this);
	_encoder->setupWeakPtr(_encoder);
	_connector = std::make_shared<Connector>(this);
	_connector->setupWeakPtr(_connector);
	_connector->setupState(_connector);
	_primaryPlane = std::make_shared<Plane>(this, Plane::PlaneType::PRIMARY);
	_primaryPlane->setupWeakPtr(_primaryPlane);
	_primaryPlane->setupState(_primaryPlane);

	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, planeTypeProperty(), 1));

	if (hasCapability(caps::cursor)) {
		_cursorPlane = std::make_shared<Plane>(this, Plane::PlaneType::CURSOR);
		_cursorPlane->setupWeakPtr(_cursorPlane);
		_cursorPlane->setupState(_cursorPlane);

		assignments.push_back(drm_core::Assignment::withInt(_cursorPlane, planeTypeProperty(), 2));
	}

	assignments.push_back(drm_core::Assignment::withInt(_connector, dpmsProperty(), 3));
	assignments.push_back(drm_core::Assignment::withBlob(_crtc, modeIdProperty(), nullptr));
	assignments.push_back(drm_core::Assignment::withInt(_crtc, activeProperty(), 0));

	registerObject(_crtc.get());
	registerObject(_encoder.get());
	registerObject(_connector.get());
	registerObject(_primaryPlane.get());

	if (hasCapability(caps::cursor)) {
		registerObject(_cursorPlane.get());
	}

	assignments.push_back(drm_core::Assignment::withModeObj(_connector, crtcIdProperty(), nullptr));
	assignments.push_back(drm_core::Assignment::withModeObj(_primaryPlane, crtcIdProperty(), _crtc));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withModeObj(_primaryPlane, fbIdProperty(), nullptr));

	_encoder->setCurrentCrtc(_crtc.get());
	_connector->setupPossibleEncoders({_encoder.get()});
	_connector->setCurrentEncoder(_encoder.get());
	_connector->setCurrentStatus(1);
	_encoder->setupPossibleCrtcs({_crtc.get()});
	_encoder->setupPossibleClones({_encoder.get()});
	_primaryPlane->setupPossibleCrtcs({_crtc.get()});

	if (hasCapability(caps::cursor)) {
		_cursorPlane->setupPossibleCrtcs({_crtc.get()});

		assignments.push_back(drm_core::Assignment::withModeObj(_cursorPlane, crtcIdProperty(), _crtc));
		assignments.push_back(drm_core::Assignment::withModeObj(_cursorPlane, fbIdProperty(), nullptr));
	}

	setupCrtc(_crtc.get());
	setupEncoder(_encoder.get());
	attachConnector(_connector.get());

	std::vector<drm_mode_modeinfo> supported_modes;
	drm_core::addDmtModes(supported_modes, current_w, current_h);

	_connector->setModeList(supported_modes);

	setupMinDimensions(32, 32);
	setupMaxDimensions(current_w, current_h);

	_connector->setupPhysicalDimensions(306, 230);
	_connector->setupSubpixel(0);
	_connector->setConnectorType(DRM_MODE_CONNECTOR_VIRTUAL);

	auto config = createConfiguration();
	auto state = atomicState();
	auto valid = config->capture(assignments, state);
	assert(valid);
	config->commit(std::move(state));

	co_return std::move(config);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t w, uint32_t, uint32_t, uint32_t, uint32_t) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);

	auto fb = std::make_shared<FrameBuffer>(this, bo, w * 4);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {1, 0, 0};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"vmware-drm", "vmware svga interface", "20190505"};
}

bool GfxDevice::hasCapability(caps capability) {
	return (_deviceCaps & (uint32_t)capability) != 0;
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t w, uint32_t h, uint32_t bpp) {
	auto size = ((w * h * bpp / 8) + 4095) & ~4095;

	HelHandle handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), size, 0, nullptr, &handle));

	auto bo = std::make_shared<GfxDevice::BufferObject>(this, size, helix::UniqueDescriptor(handle), w, h);

	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);

	return std::make_pair(bo, w * bpp / 8);
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

async::result<void> GfxDevice::waitIrq(uint32_t irq_mask) {

	static uint64_t irq_sequence;

	if (_deviceCaps & (uint32_t)caps::irqmask) {
		writeRegister(register_index::irqmask, irq_mask);
		writeRegister(register_index::sync, 1);

		auto irq = co_await _hwDev.accessIrq();

		while (true) {
			auto await = co_await helix_ng::awaitEvent(irq, irq_sequence);
			HEL_CHECK(await.error());
			irq_sequence = await.sequence();

			uint32_t irq_flags = _operational.load(ports::irq_status_port);
			if (!(irq_flags & irq_mask)) {
				HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, irq_sequence));
				continue;
			}

			_operational.store(ports::irq_status_port, irq_flags);

			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, irq_sequence));
			break;
		}
	} else {
		writeRegister(register_index::sync, 1);
		readRegister(register_index::busy);
	}

}

// ----------------------------------------------------------------
// GfxDevice::DeviceFifo
// ----------------------------------------------------------------

GfxDevice::DeviceFifo::DeviceFifo(GfxDevice *device, helix::Mapping fifoMapping)
	:_device{device}, _fifoMapping{std::move(fifoMapping)}, _fifoSize{0} {
}

void inline GfxDevice::DeviceFifo::writeRegister(fifo_index idx, uint32_t value) {
	auto mem = static_cast<volatile uint32_t *>(_fifoMapping.get());
	mem[(uint32_t)idx] = value;
}

uint32_t inline GfxDevice::DeviceFifo::readRegister(fifo_index idx) {
	auto mem = static_cast<volatile uint32_t *>(_fifoMapping.get());
	return mem[(uint32_t)idx];
}

void GfxDevice::DeviceFifo::initialize() {
	_fifoSize = _device->readRegister(register_index::mem_size);

	uint32_t min = (uint32_t)fifo_index::num_regs * 4;
	writeRegister(fifo_index::min, min);
	writeRegister(fifo_index::max, _fifoSize);
	writeRegister(fifo_index::next_cmd, min);
	writeRegister(fifo_index::stop, min);

	_device->writeRegister(register_index::config_done, 1);
}

bool GfxDevice::DeviceFifo::hasCapability(caps capability) {
	if (!_device->hasCapability(caps::fifo_extended))
		return false;
	return (readRegister(fifo_index::capabilities) & (uint32_t)capability) != 0;
}

async::result<void *> GfxDevice::DeviceFifo::reserve(size_t size) {
	size_t bytes = size * 4;
	uint32_t min = readRegister(fifo_index::min);
	uint32_t max = readRegister(fifo_index::max);
	uint32_t next_cmd = readRegister(fifo_index::next_cmd);

	bool reserveable = hasCapability(caps::fifo_reserve);

	assert(bytes < (max - min));
	assert(!(bytes % 4));
	assert(_reservedSize == 0);

	_reservedSize = bytes;

	_usingBounceBuf = false;

	while(1) {
		uint32_t stop = readRegister(fifo_index::stop);
		bool in_place = false;

		if (next_cmd >= stop) {
			if (next_cmd + bytes < max ||
				(next_cmd + bytes == max && stop > min)) in_place = true;

			else if ((max - next_cmd) + (stop - min) <= bytes) {
				co_await _device->waitIrq(2); // TODO: add a definiton for the mask
			} else {
				_usingBounceBuf = true;
			}
		} else {
			if (next_cmd + bytes < stop) in_place = true;
			else co_await _device->waitIrq(2); // TODO: add a definiton for the mask
		}

		if (in_place) {
			if (reserveable) {
				writeRegister(fifo_index::reserved, bytes);
				auto mem = static_cast<uint8_t *>(_fifoMapping.get());
				auto ptr = mem + next_cmd;

				co_return ptr;
			}
		}

		_usingBounceBuf = true;
		co_return _bounceBuf;
	}

	co_return nullptr;
}

void GfxDevice::DeviceFifo::commit(size_t bytes) {
	uint32_t min = readRegister(fifo_index::min);
	uint32_t max = readRegister(fifo_index::max);
	uint32_t next_cmd = readRegister(fifo_index::next_cmd);

	bool reserveable = hasCapability(caps::fifo_reserve);

	assert(_reservedSize > 0);

	_reservedSize = 0;

	if (_usingBounceBuf) {
		if (reserveable) {
			auto fifo = static_cast<uint8_t *>(_fifoMapping.get());

			auto chunk_size = std::min(bytes, (size_t)(max - next_cmd));
			writeRegister(fifo_index::reserved, bytes);
			memcpy(fifo + next_cmd, _bounceBuf, chunk_size);
			memcpy(fifo + min, &_bounceBuf[chunk_size], bytes - chunk_size);
		} else {
			auto buf = reinterpret_cast<uint32_t *>(_bounceBuf);
			auto fifo = reinterpret_cast<uint32_t *>(_fifoMapping.get());
			while (bytes) {
				fifo[next_cmd / 4] = *buf++;
				next_cmd += 4;
				if (next_cmd >= max) {
					next_cmd -= max - min;
				}
				writeRegister(fifo_index::next_cmd, next_cmd);
				bytes -= 4;
			}
		}

	} else {
		next_cmd += bytes;
		if (next_cmd >= max)
			next_cmd -= max - min;

		writeRegister(fifo_index::next_cmd, next_cmd);
	}

	if (reserveable)
		writeRegister(fifo_index::reserved, 0);
}

void GfxDevice::DeviceFifo::commitAll() {
	commit(_reservedSize);
}

#define SVGA_BITMAP_SIZE(w, h)      ((((w) + 31) >> 5) * (h))
#define SVGA_PIXMAP_SIZE(w, h, bpp) (((((w) * (bpp)) + 31) >> 5) * (h))

async::result<void> GfxDevice::DeviceFifo::defineCursor(int width, int height, GfxDevice::BufferObject *bo) {

	if (!_device->hasCapability(caps::cursor))
		co_return;

	// size in dwords
	size_t size = sizeof(commands::define_cursor) / 4 + 1 + SVGA_BITMAP_SIZE(width, height) + SVGA_PIXMAP_SIZE(width, height, 32);

	auto ptr = static_cast<uint32_t *>(co_await reserve(size));

	ptr[0] = (uint32_t)command_index::define_cursor;
	auto cmd = reinterpret_cast<commands::define_cursor *>(ptr + 1);

	cmd->width = width;
	cmd->height = height;
	cmd->id = 0;
	cmd->hotspot_x = 1;
	cmd->hotspot_y = 1;
	cmd->and_mask_depth = 1;
	cmd->xor_mask_depth = 32;

	if (bo) {
		helix::Mapping bitmap{bo->getMemory().first, 0, (size_t)(width * height * 4)};

		auto pixels = static_cast<uint32_t *>(bitmap.get());
		auto mask = reinterpret_cast<uint8_t *>(cmd->pixel_data);

		memset(cmd->pixel_data, 0x00, (SVGA_BITMAP_SIZE(width, height) + SVGA_PIXMAP_SIZE(width, height, 32)) * 4);

		for (int i = 0; i < height; i++) {
			for (int j = 0; j < width; j++) {
				int idx = width * i + j;
				int bitmask = 1 << (7 - j % 8);
				int off = idx / 8;
				mask[off] |= !(pixels[idx] & 0xFF000000) ? bitmask : 0;
			}
		}

		memcpy(cmd->pixel_data + SVGA_BITMAP_SIZE(width, height) * 4, pixels, width * height * 4);
	}
	commitAll();
}

void GfxDevice::DeviceFifo::moveCursor(int x, int y) {
	if (!_device->hasCapability(caps::cursor))
		return;

	if (hasCapability(caps::fifo_cursor_bypass_3)) {
		writeRegister(fifo_index::cursor_x, x);
		writeRegister(fifo_index::cursor_y, y);
		writeRegister(fifo_index::cursor_count, readRegister(fifo_index::cursor_count) + 1);
		writeRegister(fifo_index::cursor_screen_id, 0xFFFFFFFF);
	} else {
		_device->writeRegister(register_index::cursor_x, x);
		_device->writeRegister(register_index::cursor_y, y);
	}
}

void GfxDevice::DeviceFifo::setCursorState(bool enabled) {
	if (!_device->hasCapability(caps::cursor))
		return;

	if (hasCapability(caps::fifo_cursor_bypass_3)) {
		writeRegister(fifo_index::cursor_on, enabled ? 1 : 0);
	} else {
		_device->writeRegister(register_index::cursor_on, enabled ? 1 : 0);
	}
}

async::result<void> GfxDevice::DeviceFifo::updateRectangle(int x, int y, int w, int h) {
	// size in dwords
	size_t size = sizeof(commands::update_rectangle) / 4 + 1;

	auto ptr = static_cast<uint32_t *>(co_await reserve(size));

	ptr[0] = (uint32_t)command_index::update;
	auto cmd = reinterpret_cast<commands::update_rectangle *>(ptr + 1);

	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;

	commitAll();
}

// ----------------------------------------------------------------
// GfxDevice::Configuration
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> & state) {
	for (auto &assign : assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);
	}

	auto primary_plane_state = state->plane(_device->_primaryPlane->id());
	auto crtc_state = state->crtc(_device->_crtc->id());

	if(crtc_state->mode != nullptr) {
		if (primary_plane_state->src_w <= 0 || primary_plane_state->src_h <= 0 ||
			primary_plane_state->src_w > 1024 || primary_plane_state->src_h > 768) {
			return false;
		}
	}

	for (auto &assign : assignment) {
		using namespace drm_core;

		switch(assign.property->id()) {
			case srcW: {
				if (assign.object == _device->_cursorPlane && _device->hasCapability(caps::cursor)) {
					_cursorUpdate = true;
				}
				break;
			}
			case srcH: {
				if (assign.object == _device->_cursorPlane && _device->hasCapability(caps::cursor)) {
					_cursorUpdate = true;
				}
				break;
			}
			case crtcX: {
				if (assign.object == _device->_cursorPlane && _device->hasCapability(caps::cursor)) {
					_cursorMove = true;
				}
				break;
			}
			case crtcY: {
				if (assign.object == _device->_cursorPlane && _device->hasCapability(caps::cursor)) {
					_cursorMove = true;
				}
				break;
			}
			case fbId: {
				if (assign.objectValue && assign.object == _device->_cursorPlane && _device->hasCapability(caps::cursor)) {
					_cursorUpdate = true;
				}
				break;
			}
			// Ignore any property that is unsupported by this driver
			default: {
				break;
			}
		}
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> state) {
	_device->_crtc->setDrmState(state->crtc(_device->_crtc->id()));
	_device->_primaryPlane->setDrmState(state->plane(_device->_primaryPlane->id()));
	_device->_cursorPlane->setDrmState(state->plane(_device->_cursorPlane->id()));

	commitConfiguration(std::move(state));
}

async::detached GfxDevice::Configuration::commitConfiguration(std::unique_ptr<drm_core::AtomicState> state) {
	auto primary_plane_state = state->plane(_device->_primaryPlane->id());
	auto cursor_plane_state = state->plane(_device->_cursorPlane->id());
	auto crtc_state = state->crtc(_device->_crtc->id());

	if(crtc_state->mode != nullptr) {
		if (!_device->_isClaimed) {
			co_await _device->_hwDev.claimDevice();
			_device->_isClaimed = true;
			_device->writeRegister(register_index::enable, 1); // lazy init
		}

		if (crtc_state->modeChanged) {
			_device->writeRegister(register_index::enable, 0); // prevent weird inbetween modes
			_device->writeRegister(register_index::width, primary_plane_state->src_w);
			_device->writeRegister(register_index::height, primary_plane_state->src_h);
			_device->writeRegister(register_index::bits_per_pixel, 32);
			_device->writeRegister(register_index::enable, 1);
		}
	}

	if (_cursorUpdate) {
		if (cursor_plane_state->src_w != 0 && cursor_plane_state->src_h != 0) {
			_device->_fifo.setCursorState(true);
			auto cursor_fb = static_pointer_cast<GfxDevice::FrameBuffer>(cursor_plane_state->fb);
			co_await _device->_fifo.defineCursor(cursor_plane_state->src_w, cursor_plane_state->src_h, cursor_fb->getBufferObject());
			_device->_fifo.setCursorState(true);
		} else {
			_device->_fifo.setCursorState(false);
		}
	}

	if (_cursorMove) {
		_device->_fifo.moveCursor(cursor_plane_state->src_x, cursor_plane_state->src_y);
	}

	if (primary_plane_state->fb != nullptr) {
		auto fb = static_pointer_cast<GfxDevice::FrameBuffer>(primary_plane_state->fb);
		auto [fbMemory, fbMemoryOffset] = fb->getBufferObject()->getMemory();
		helix::Mapping user_fb{fbMemory, static_cast<ptrdiff_t>(fbMemoryOffset), fb->getBufferObject()->getSize()};
		drm_core::fastCopy16(_device->_fbMapping.get(), user_fb.get(), fb->getBufferObject()->getSize());
		int w = _device->readRegister(register_index::width),
			h = _device->readRegister(register_index::height);

		co_await _device->_fifo.updateRectangle(0, 0, w, h);
	}

	complete();
}

// ----------------------------------------------------------------
// GfxDevice::Connector
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *dev)
	: drm_core::Connector { dev, dev->allocator.allocate() } {
	_encoders.push_back(dev->_encoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *dev)
	: drm_core::Encoder { dev, dev->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *dev)
	: drm_core::Crtc { dev, dev->allocator.allocate() } {
	_device = dev;
}

drm_core::Plane *GfxDevice::Crtc::Crtc::primaryPlane() {
	return _device->_primaryPlane.get();
}

drm_core::Plane *GfxDevice::Crtc::Crtc::cursorPlane() {
	return _device->_cursorPlane.get();
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *dev,
		std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
	: drm_core::FrameBuffer { dev, dev->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

void GfxDevice::FrameBuffer::notifyDirty() {

}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

// ----------------------------------------------------------------
// GfxDevice::Plane
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *dev, PlaneType type)
	:drm_core::Plane { dev, dev->allocator.allocate(), type } {
}

// ----------------------------------------------------------------
// GfxDevice::BufferObject
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *dev, size_t size, helix::UniqueDescriptor mem, uint32_t width, uint32_t height)
: drm_core::BufferObject{width, height}, _size{size}, _mem{std::move(mem)} {
	(void)dev;
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_mem}, 0);
}

// ----------------------------------------------------------------

async::result<void> setupDevice(mbus_ng::Entity hwEntity) {
	std::cout << "gfx/vmware: setting up the device" << std::endl;

	protocols::hw::Device pci_device((co_await hwEntity.getRemoteLane()).unwrap());
	auto info = co_await pci_device.getPciInfo();

	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	assert(info.barInfo[1].ioType == protocols::hw::IoType::kIoTypeMemory);
	assert(info.barInfo[2].ioType == protocols::hw::IoType::kIoTypeMemory);

	auto io_bar = co_await pci_device.accessBar(0);
	auto fb_bar = co_await pci_device.accessBar(1);
	auto fifo_bar = co_await pci_device.accessBar(2);

	auto io_bar_info = info.barInfo[0];
	auto fb_bar_info = info.barInfo[1];
	auto fifo_bar_info = info.barInfo[2];

	auto gfxDevice = std::make_shared<GfxDevice>(std::move(pci_device),
			helix::Mapping{fb_bar, 0, fb_bar_info.length},
			helix::Mapping{fifo_bar, 0, fifo_bar_info.length},
			std::move(io_bar), io_bar_info.address);

	auto config = co_await gfxDevice->initialize();

	// Create an mbus object for the device.
	mbus_ng::Properties descriptor{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(hwEntity.id())}},
		{"unix.subsystem", mbus_ng::StringItem{"drm"}},
		{"unix.devname", mbus_ng::StringItem{"dri/card"}}
	};

	co_await config->waitForCompletion();

	auto gfxEntity = (co_await mbus_ng::Instance::global().createEntity(
		"gfx_vmware", descriptor)).unwrap();

	[] (auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			drm_core::serveDrmDevice(device, std::move(localLane));
		}
	}(gfxDevice, std::move(gfxEntity));

	baseDeviceMap.insert({hwEntity.id(), gfxDevice});
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "gfx/vmware: Binding to device " << base_id << std::endl;
	auto baseEntity = co_await mbus_ng::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(baseDeviceMap.find(baseEntity.id()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = (co_await baseEntity.getProperties()).unwrap();
	if(auto vendor_str = std::get_if<mbus_ng::StringItem>(&properties["pci-vendor"]);
			!vendor_str || vendor_str->value != "15ad")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if(auto device_str = std::get_if<mbus_ng::StringItem>(&properties["pci-device"]);
			!device_str || device_str->value != "0405")
		co_return protocols::svrctl::Error::deviceNotSupported;

	co_await setupDevice(std::move(baseEntity));
	co_return protocols::svrctl::Error::success;
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

int main() {
	printf("gfx/vmware: starting driver\n");

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}

