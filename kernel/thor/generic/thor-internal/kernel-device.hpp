#pragma once

#include <atomic>
#include <optional>
#include <frg/list.hpp>
#include <frg/span.hpp>
#include <frg/vector.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

enum class KernelDeviceResourceKind {
	mmio,
	io,
};

// A physical address range or I/O port range that a kernel driver accesses.
struct KernelDeviceResource {
	KernelDeviceResourceKind kind;
	uint64_t base;
	uint64_t length;
};

// Geometry of a framebuffer that a kernel device draws to.
struct KernelFramebufferInfo {
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
};

// A device that the kernel drives itself, e.g., the boot framebuffer or the boot UART.
//
// Userspace does not know which hardware the kernel touches, and the kernel does not know
// how that hardware is attached (e.g., whether a framebuffer sits behind a PCI BAR).
// The registry closes this gap: the kernel publishes the resources it accesses and
// userspace claims the device once a driver takes it over.
//
// Userspace also suspends devices while they cannot be accessed (e.g., during PCIe enumeration).
// Drivers drop hardware accesses while suspended rather than wait: they run with scheduling
// disabled, and userspace may keep the device suspended indefinitely. resume() lets a driver
// redo what it dropped where that is possible (e.g., the framebuffer re-renders the log ring).
struct KernelDevice {
	constexpr KernelDevice(const char *name)
	: name_{name} { }

	KernelDevice(const KernelDevice &) = delete;

	KernelDevice &operator= (const KernelDevice &) = delete;

	// Resources and the framebuffer must be set before the device is registered.
	void addResource(KernelDeviceResource resource);
	void setFramebuffer(KernelFramebufferInfo framebuffer);

	// Drivers check this with scheduling disabled (e.g., with IRQs disabled) right before
	// accessing the hardware; that is the read side that suspendKernelDevice() and
	// claimKernelDevice() wait for. A claimed device stays suspended for good.
	bool isSuspended() const {
		return suspendCount_.load(std::memory_order_acquire) != 0
				|| claimed_.load(std::memory_order_acquire);
	}

	const char *name() const { return name_; }
	uint64_t id() const { return id_; }
	frg::span<const KernelDeviceResource> resources() const {
		return {resources_.data(), resources_.size()};
	}
	const std::optional<KernelFramebufferInfo> &framebuffer() const { return framebuffer_; }

	frg::default_list_hook<KernelDevice> hook;

protected:
	~KernelDevice() = default;

	// Called when the last suspension is lifted.
	virtual void resume() { }
	// Called once when the kernel gives up the device for good.
	virtual void claim() { }

private:
	friend void registerKernelDevice(KernelDevice *device);
	friend coroutine<void> suspendKernelDevice(KernelDevice *device);
	friend bool resumeKernelDevice(KernelDevice *device);
	friend coroutine<void> claimKernelDevice(KernelDevice *device);

	const char *name_;
	uint64_t id_ = 0;
	frg::vector<KernelDeviceResource, KernelAlloc> resources_;
	std::optional<KernelFramebufferInfo> framebuffer_;
	std::atomic<unsigned int> suspendCount_{0};
	std::atomic<bool> claimed_{false};
};

void registerKernelDevice(KernelDevice *device);

// Devices are never unregistered, hence the returned pointers stay valid.
frg::vector<KernelDevice *, KernelAlloc> allKernelDevices();
KernelDevice *findKernelDevice(uint64_t id);

// Closes the gate and waits until every driver access that started before the call has
// completed. Suspensions nest.
coroutine<void> suspendKernelDevice(KernelDevice *device);
// Lifts one suspension. Returns false if the device was not suspended.
bool resumeKernelDevice(KernelDevice *device);
// Like suspendKernelDevice() but permanent; claim() runs once no driver access is in flight.
coroutine<void> claimKernelDevice(KernelDevice *device);

} // namespace thor
