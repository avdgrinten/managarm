#pragma once

#include <atomic>
#include <optional>
#include <frg/list.hpp>
#include <frg/span.hpp>
#include <frg/vector.hpp>
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

// A device that the kernel drives itself, e.g., the boot framebuffer.
//
// Userspace does not know which hardware the kernel touches, and the kernel does not know
// how that hardware is attached (e.g., whether a framebuffer sits behind a PCI BAR).
// The registry closes this gap: the kernel publishes the resources it accesses and
// userspace claims the device once a driver takes it over.
struct KernelDevice {
	KernelDevice(const char *name)
	: name_{name} { }

	KernelDevice(const KernelDevice &) = delete;

	KernelDevice &operator= (const KernelDevice &) = delete;

	// Resources and the framebuffer must be set before the device is registered.
	void addResource(KernelDeviceResource resource);
	void setFramebuffer(KernelFramebufferInfo framebuffer);

	const char *name() const { return name_; }
	uint64_t id() const { return id_; }
	frg::span<const KernelDeviceResource> resources() const {
		return {resources_.data(), resources_.size()};
	}
	const std::optional<KernelFramebufferInfo> &framebuffer() const { return framebuffer_; }

	frg::default_list_hook<KernelDevice> hook;

protected:
	~KernelDevice() = default;

	// Called once when the kernel gives up the device for good.
	virtual void claim() { }

private:
	friend void registerKernelDevice(KernelDevice *device);
	friend void claimKernelDevice(KernelDevice *device);

	const char *name_;
	uint64_t id_ = 0;
	frg::vector<KernelDeviceResource, KernelAlloc> resources_{*kernelAlloc};
	std::optional<KernelFramebufferInfo> framebuffer_;
	std::atomic<bool> claimed_{false};
};

void registerKernelDevice(KernelDevice *device);

// Devices are never unregistered, hence the returned pointers stay valid.
frg::vector<KernelDevice *, KernelAlloc> allKernelDevices();
KernelDevice *findKernelDevice(uint64_t id);

void claimKernelDevice(KernelDevice *device);

} // namespace thor
