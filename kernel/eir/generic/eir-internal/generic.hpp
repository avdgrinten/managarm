#pragma once

#include <cstddef>
#include <eir-internal/arch/types.hpp>
#include <eir/interface.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace eir {

extern frg::span<uint8_t> kernel_image;
// Physical base address of the kernel image.
extern address_t kernel_physical;
extern frg::span<uint8_t> initrd_image;

extern CpuConfig cpuConfig;
extern AcpiData acpiDataNote;
extern DtData dtDataNote;
extern EirFramebuffer framebufferNote;
extern Initrd initrdNote;
extern PhysicalMemory physicalMemoryNote;
extern CommandLine commandLineNote;

enum class RegionType { null, unconstructed, allocatable };

struct Region {
	RegionType regionType;
	address_t address;
	address_t size;

	int order;
	uint64_t numRoots;
	address_t buddyTree;
	address_t buddyOverhead;
	address_t buddyMap;
};

extern Region regions[eirMaxMemoryRegions];
extern address_t allocatedMemory;
extern address_t physOffset;

// Capabilites of a boot protocol.
struct BootCaps {
	uintptr_t imageStart{0};
	uintptr_t imageEnd{0};
	bool hasMemoryMap{false};

	static const BootCaps &get();
};

void eirRelocate();

physaddr_t bootReserve(size_t length, size_t alignment);
physaddr_t allocPage();
void allocLogRingBuffer();

void setupRegionStructs();
void createInitialRegion(address_t base, address_t size);
// Removes memory from the allocatable regions. Must be called before setupRegionStructs().
address_t cutFromRegion(size_t size);

void
reportFirmwareMemory(address_t address, address_t size, EirMemoryType type, uint32_t attributes);
void serializeFirmwareMemoryMap();

struct InitialRegion {
	address_t base;
	address_t size;
};

void createInitialRegions(InitialRegion region, frg::span<InitialRegion> reserved);

template <typename T>
T *physToVirt(physaddr_t physical) {
	return reinterpret_cast<T *>(physOffset + physical);
}

template <typename T>
physaddr_t virtToPhys(T *virt) {
	return reinterpret_cast<physaddr_t>(virt) - physOffset;
}

// Data that eir passes to thor. A placement is virtually contiguous in thor's address space
// but not in eir's; the write functions take care of crossing page boundaries.
struct BootstrapData {
	// Reserves size bytes of bootstrap data. The data is filled in by the write functions.
	static BootstrapData place(size_t size, size_t alignment);

	void writeBytes(std::span<const std::byte> bytes);

	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	void write(const T &object) {
		writeBytes(std::as_bytes(std::span{&object, 1}));
	}

	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	void writeArray(std::span<T> array) {
		writeBytes(std::as_bytes(array));
	}

	// Address of the placement in thor's address space.
	address_t kernelAddress() { return address_; }

private:
	BootstrapData(address_t address, size_t size) : address_{address}, size_{size} {}

	address_t address_;
	size_t size_;
	size_t offset_{0};
	// eir's view of the page that offset_ points into.
	std::byte *window_{nullptr};
};

void mapKasanShadow(uint64_t address, size_t size);
void unpoisonKasanShadow(uint64_t address, size_t size);
void mapRegionsAndStructs();

// Kernel entrypoint.
extern uint64_t kernelEntry;

// Must be called after memory regions are known but before setupRegionStructs().
void decompressInitrd();
void parseInitrd(void *initrd);
void loadKernelImage(void *image);

template <typename T>
T *bootAlloc(size_t n = 1) {
	auto pointer = physToVirt<T>(bootReserve(sizeof(T) * n, alignof(T)));
	for (size_t i = 0; i < n; i++)
		new (&pointer[i]) T();
	return pointer;
}

} // namespace eir
