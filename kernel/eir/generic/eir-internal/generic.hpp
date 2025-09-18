#pragma once

#include <eir-internal/arch/types.hpp>
#include <eir/interface.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <stddef.h>
#include <stdint.h>

namespace eir {

extern frg::span<uint8_t> kernel_image;
// Physical base address of the kernel image.
extern address_t kernel_physical;
extern frg::span<uint8_t> initrd_image;

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

static constexpr size_t numRegions = 64;
extern Region regions[numRegions];
extern address_t allocatedMemory;
extern address_t physOffset;

struct GenericInfo {
	const char *cmdline;
	EirFramebuffer fb;
	uint32_t debugFlags;
	bool hasFb;
};

// Capabilites of a boot protocol.
struct BootCaps {
	uintptr_t imageStart{0};
	uintptr_t imageEnd{0};
	bool hasMemoryMap{false};

	static const BootCaps &get();
};

void eirRelocate();
[[noreturn]] void eirGenericMain(const GenericInfo &genericInfo);

physaddr_t bootReserve(size_t length, size_t alignment);
physaddr_t allocPage();
void allocLogRingBuffer();

void setupRegionStructs();
void createInitialRegion(address_t base, address_t size);

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

address_t mapBootstrapData(void *p);
void mapKasanShadow(uint64_t address, size_t size);
void unpoisonKasanShadow(uint64_t address, size_t size);
void mapRegionsAndStructs();

// Kernel entrypoint.
extern uint64_t kernelEntry;

void parseInitrd(void *initrd);
void loadKernelImage(void *image);

EirInfo *generateInfo(frg::string_view cmdline);

template <typename T>
T *bootAlloc(size_t n = 1) {
	auto pointer = physToVirt<T>(bootReserve(sizeof(T) * n, alignof(T)));
	for (size_t i = 0; i < n; i++)
		new (&pointer[i]) T();
	return pointer;
}

} // namespace eir
