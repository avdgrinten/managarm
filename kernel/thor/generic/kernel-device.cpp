#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-device.hpp>
#include <thor-internal/rcu.hpp>

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

coroutine<void> suspendKernelDevice(KernelDevice *device) {
	device->suspendCount_.fetch_add(1, std::memory_order_seq_cst);
	// Drivers check the gate with scheduling disabled, so one quiescent state per CPU
	// guarantees that no access that missed the closed gate is still in flight.
	co_await rcuBarrier();
}

bool resumeKernelDevice(KernelDevice *device) {
	auto count = device->suspendCount_.load(std::memory_order_relaxed);
	do {
		if (!count)
			return false;
	} while (!device->suspendCount_.compare_exchange_weak(
		count,
		count - 1,
		std::memory_order_release,
		std::memory_order_relaxed
	));

	if (count == 1 && !device->claimed_.load(std::memory_order_relaxed))
		device->resume();
	return true;
}

coroutine<void> claimKernelDevice(KernelDevice *device) {
	auto first = !device->claimed_.exchange(true, std::memory_order_seq_cst);
	// Same read side as in suspendKernelDevice(); this also covers a racing resume().
	// Repeated claims wait as well since they must not return while accesses are in flight.
	co_await rcuBarrier();
	if (!first)
		co_return;
	infoLogger() << "thor: Userspace claimed kernel device " << device->name() << frg::endlog;
	device->claim();
}

} // namespace thor
