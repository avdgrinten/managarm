#include <iostream>
#include <protocols/posix/procdata.hpp>
#include <unordered_map>
#include <pthread.h>

#include <uhda/kernel_api.h>

#include <protocols/hw/client.hpp>

#include <helix/timer.hpp>
#include <helix/memory.hpp>

#include "controller.hpp"

namespace {

template<typename T>
async::result<void> pciWrite(protocols::hw::Device &dev, uint8_t offset, T value) {
	co_await dev.storePciSpace(offset, sizeof(T), value);
}

template<typename T>
async::result<T> pciRead(protocols::hw::Device &dev, uint8_t offset) {
	co_return co_await dev.loadPciSpace(offset, sizeof(T));
}

async::result<void *> mapBar(protocols::hw::Device &dev, int bar) {
	auto handle = co_await dev.accessBar(bar);

	auto info = co_await dev.getPciInfo();

	void *barAddress;

	HEL_CHECK(helMapMemory(handle.getHandle(), kHelNullHandle, nullptr,
		0, info.barInfo[bar].length, kHelMapProtRead | kHelMapProtWrite, &barAddress));

	co_return barAddress;
}

async::result<void> unmapBar(protocols::hw::Device &dev, int bar, void *ptr) {
	auto info = co_await dev.getPciInfo();

	HEL_CHECK(helUnmapMemory(kHelNullHandle, ptr, info.barInfo[bar].length));
}

}

UhdaStatus uhda_kernel_pci_read(void *pci_device, uint8_t offset, uint8_t size, uint32_t *res) {
	auto ctrl = static_cast<Controller *>(pci_device);

	switch (size) {
	case 1:
		*res = async::run(pciRead<uint8_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	case 2:
		*res = async::run(pciRead<uint16_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	case 4:
		*res = async::run(pciRead<uint32_t>(ctrl->device, offset), helix::currentDispatcher);
		break;
	default:
		return UHDA_STATUS_UNSUPPORTED;
	}

	return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_kernel_pci_write(void *pci_device, uint8_t offset, uint8_t size, uint32_t value) {
	auto ctrl = static_cast<Controller *>(pci_device);

	switch (size) {
	case 1:
		async::run(pciWrite<uint8_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	case 2:
		async::run(pciWrite<uint16_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	case 4:
		async::run(pciWrite<uint32_t>(ctrl->device, offset, value), helix::currentDispatcher);
		break;
	default:
		return UHDA_STATUS_UNSUPPORTED;
	}

	return UHDA_STATUS_SUCCESS;
}

async::detached handleIrqs(helix::BorrowedDescriptor irq, UhdaIrqHandlerFn fn, void *arg);

UhdaStatus uhda_kernel_pci_allocate_irq(void *pci_device, UhdaIrqHint hint, UhdaIrqHandlerFn fn, void *arg, void **opaque_irq) {
	// TODO: Don't ignore hint
	(void)hint;

	auto ctrl = static_cast<Controller *>(pci_device);

	auto irq = async::run(ctrl->device.accessIrq(), helix::currentDispatcher);

	handleIrqs(irq, fn, arg);

	*opaque_irq = reinterpret_cast<void *>(irq.getHandle());
	irq.release();

	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_deallocate_irq(void *, void *opaque_irq) {
	helix::UniqueDescriptor irq{reinterpret_cast<HelHandle>(opaque_irq)};
}

void uhda_kernel_pci_enable_irq(void* pci_device, void *, bool enable) {
	auto ctrl = static_cast<Controller *>(pci_device);

	if (enable) {
		async::run(ctrl->device.enableBusIrq(), helix::currentDispatcher);
	}
}

UhdaStatus uhda_kernel_pci_map_bar(void *pci_device, uint32_t bar, void **virt) {
	auto ctrl = static_cast<Controller *>(pci_device);

	*virt = async::run(mapBar(ctrl->device, bar), helix::currentDispatcher);
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_unmap_bar(void* pci_device, uint32_t bar, void* virt) {
	auto ctrl = static_cast<Controller *>(pci_device);

	async::run(unmapBar(ctrl->device, bar, virt), helix::currentDispatcher);
}

void* uhda_kernel_malloc(size_t size) {
	return malloc(size);
}

void uhda_kernel_free(void *ptr, size_t) {
	free(ptr);
}

void uhda_kernel_delay(uint32_t microseconds) {
	async::run(helix::sleepFor(microseconds * 1000), helix::currentDispatcher);
}

void uhda_kernel_log(const char *str) {
	std::cout << "sound/hda: uHDA: " << str << std::endl;
}

namespace {

std::unordered_map<uintptr_t, void *> physicalMappings;

}

UhdaStatus uhda_kernel_allocate_physical(size_t size, uintptr_t *res) {
	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), size, kHelAllocContinuous, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
		0, size, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));

	*res = helix::ptrToPhysical(window);

	physicalMappings.insert({*res, window});

	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_deallocate_physical(uintptr_t phys, size_t size) {
	auto mapping = physicalMappings.find(phys);
	assert(mapping != physicalMappings.end());

	HEL_CHECK(helUnmapMemory(kHelNullHandle, mapping->second, size));
}

UhdaStatus uhda_kernel_map(uintptr_t phys, size_t, void **virt) {
	auto mapping = physicalMappings.find(phys);
	assert(mapping != physicalMappings.end());

	*virt = mapping->second;
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_unmap(void *, size_t) {}

UhdaStatus uhda_kernel_create_spinlock(void **spinlock) {
	auto lock = new pthread_spinlock_t{};
	pthread_spin_init(lock, PTHREAD_PROCESS_PRIVATE);
	*spinlock = lock;
	return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_free_spinlock(void *spinlock) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_destroy(lock);
	delete lock;
}

UhdaIrqState uhda_kernel_lock_spinlock(void *spinlock) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_lock(lock);
	return 0;
}

void uhda_kernel_unlock_spinlock(void* spinlock, UhdaIrqState) {
	auto lock = static_cast<pthread_spinlock_t *>(spinlock);
	pthread_spin_unlock(lock);
}
