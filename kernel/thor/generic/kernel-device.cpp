#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-device.hpp>

namespace thor {

namespace {

constinit IrqSpinlock registryMutex;

constinit frg::intrusive_list<
	KernelDevice,
	frg::locate_member<
		KernelDevice,
		frg::default_list_hook<KernelDevice>,
		&KernelDevice::hook
	>
> registry;

constinit uint64_t nextId = 1;

} // anonymous namespace

void KernelDevice::addResource(KernelDeviceResource resource) {
	assert(!id_);
	resources_.push_back(resource);
}

void KernelDevice::setFramebuffer(KernelFramebufferInfo framebuffer) {
	assert(!id_);
	framebuffer_ = framebuffer;
}

void registerKernelDevice(KernelDevice *device) {
	auto lock = frg::guard(&registryMutex);
	assert(!device->id_);
	device->id_ = nextId++;
	registry.push_back(device);
}

frg::vector<KernelDevice *, KernelAlloc> allKernelDevices() {
	frg::vector<KernelDevice *, KernelAlloc> devices{*kernelAlloc};
	auto lock = frg::guard(&registryMutex);
	for (auto device : registry)
		devices.push_back(device);
	return devices;
}

KernelDevice *findKernelDevice(uint64_t id) {
	auto lock = frg::guard(&registryMutex);
	for (auto device : registry) {
		if (device->id() == id)
			return device;
	}
	return nullptr;
}

void claimKernelDevice(KernelDevice *device) {
	if (device->claimed_.exchange(true, std::memory_order_relaxed))
		return;
	infoLogger() << "thor: Userspace claimed kernel device " << device->name() << frg::endlog;
	device->claim();
}

} // namespace thor
