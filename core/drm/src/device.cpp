#include "core/drm/core.hpp"
#include "core/drm/device.hpp"
#include "core/drm/debug.hpp"
#include "core/drm/property.hpp"

// ----------------------------------------------------------------
// Device
// ----------------------------------------------------------------

void drm_core::Device::setupCrtc(drm_core::Crtc *crtc) {
	crtc->index = _crtcs.size();
	_crtcs.push_back(crtc);
}

void drm_core::Device::setupEncoder(drm_core::Encoder *encoder) {
	encoder->index = _encoders.size();
	_encoders.push_back(encoder);
}

void drm_core::Device::attachConnector(drm_core::Connector *connector) {
	_connectors.push_back(connector);
}

const std::vector<drm_core::Crtc *> &drm_core::Device::getCrtcs() {
	return _crtcs;
}

const std::vector<drm_core::Encoder *> &drm_core::Device::getEncoders() {
	return _encoders;
}

const std::vector<drm_core::Connector *> &drm_core::Device::getConnectors() {
	return _connectors;
}

void drm_core::Device::registerObject(drm_core::ModeObject *object) {
	_objects.insert({object->id(), object});
}

std::shared_ptr<drm_core::ModeObject> drm_core::Device::findObject(uint32_t id) {
	auto it = _objects.find(id);
	if(it == _objects.end())
		return nullptr;
	return it->second->sharedModeObject();
}

std::shared_ptr<drm_core::Blob> drm_core::Device::registerBlob(std::vector<char> data) {
	uint32_t id = _blobIdAllocator.allocate();
	auto blob = std::make_shared<drm_core::Blob>(data, id);
	_blobs.insert({id, blob});

	return blob;
}

bool drm_core::Device::deleteBlob(uint32_t id) {
	if(_blobs.contains(id)) {
		_blobs.erase(id);
		return true;
	}

	return false;
}

std::shared_ptr<drm_core::Blob> drm_core::Device::findBlob(uint32_t id) {
	auto it = _blobs.find(id);
	if(it == _blobs.end())
		return nullptr;
	return it->second;
}

std::unique_ptr<drm_core::AtomicState> drm_core::Device::atomicState() {
	auto state = AtomicState(this);
	return std::make_unique<drm_core::AtomicState>(state);
}

/**
 * Adds a (credentials, BufferObject) pair to the list of exported BOs for this device
 */
void drm_core::Device::registerBufferObject(std::shared_ptr<drm_core::BufferObject> obj, helix_ng::Credentials creds) {
	_exportedBufferObjects.insert({creds, obj});
}

/**
 * Retrieves a BufferObject from the list of exported BOs for this device, given the credentials for it
 */
std::shared_ptr<drm_core::BufferObject> drm_core::Device::findBufferObject(helix_ng::Credentials creds) {
	auto it = _exportedBufferObjects.find(creds);
	if(it == _exportedBufferObjects.end())
		return nullptr;
	return it->second;
}

uint64_t drm_core::Device::installMapping(drm_core::BufferObject *bo) {
	assert(bo->getSize() < (UINT64_C(1) << 32));
	return static_cast<uint64_t>(_memorySlotAllocator.allocate()) << 32;
}

void drm_core::Device::uninstallMapping(drm_core::BufferObject *bo) {
	_memorySlotAllocator.free(bo->getMapping() >> 32);
}

void drm_core::Device::setupMinDimensions(uint32_t width, uint32_t height) {
	_minWidth = width;
	_minHeight = height;
}

void drm_core::Device::setupMaxDimensions(uint32_t width, uint32_t height) {
	_maxWidth = width;
	_maxHeight = height;
}

void drm_core::Device::setupCursorDimensions(uint32_t width, uint32_t height) {
	_cursorWidth = width;
	_cursorHeight = height;
}

uint32_t drm_core::Device::getMinWidth() {
	return _minWidth;
}

uint32_t drm_core::Device::getMaxWidth() {
	return _maxWidth;
}

uint32_t drm_core::Device::getMinHeight() {
	return _minHeight;
}

uint32_t drm_core::Device::getMaxHeight() {
	return _maxHeight;
}

uint32_t drm_core::Device::getCursorWidth() {
	return _cursorWidth;
}

uint32_t drm_core::Device::getCursorHeight() {
	return _cursorHeight;
}

bool drm_core::Device::getAddFb2ModifiersSupport() {
	return fb2ModifiersSupport;
}

drm_core::Property *drm_core::Device::srcWProperty() {
	return _srcWProperty.get();
}

drm_core::Property *drm_core::Device::srcHProperty() {
	return _srcHProperty.get();
}

drm_core::Property *drm_core::Device::fbIdProperty() {
	return _fbIdProperty.get();
}

drm_core::Property *drm_core::Device::modeIdProperty() {
	return _modeIdProperty.get();
}

drm_core::Property *drm_core::Device::crtcXProperty() {
	return _crtcXProperty.get();
}

drm_core::Property *drm_core::Device::crtcYProperty() {
	return _crtcYProperty.get();
}

drm_core::Property *drm_core::Device::planeTypeProperty() {
	return _planeTypeProperty.get();
}

drm_core::Property *drm_core::Device::dpmsProperty() {
	return _dpmsProperty.get();
}

drm_core::Property *drm_core::Device::crtcIdProperty() {
	return _crtcIdProperty.get();
}

drm_core::Property *drm_core::Device::activeProperty() {
	return _activeProperty.get();
}

drm_core::Property *drm_core::Device::srcXProperty() {
	return _srcXProperty.get();
}

drm_core::Property *drm_core::Device::srcYProperty() {
	return _srcYProperty.get();
}

drm_core::Property *drm_core::Device::crtcWProperty() {
	return _crtcWProperty.get();
}

drm_core::Property *drm_core::Device::crtcHProperty() {
	return _crtcHProperty.get();
}

drm_core::Property *drm_core::Device::inFormatsProperty() {
	return _inFormatsProperty.get();
}

void drm_core::Device::registerProperty(std::shared_ptr<drm_core::Property> p) {
	_properties.insert({p->id(), p});
}

std::shared_ptr<drm_core::Property> drm_core::Device::getProperty(uint32_t id) {
	auto it = _properties.find(id);

	if(it == _properties.end()) {
		return nullptr;
	}

	return it->second;
}
