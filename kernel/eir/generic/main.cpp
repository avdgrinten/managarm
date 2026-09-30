#include <eir-internal/arch.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/cpio.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir-internal/uart/uart.hpp>

#include <elf.h>
#include <frg/algorithm.hpp>
#include <frg/array.hpp>
#include <frg/manual_box.hpp>
#include <frg/utility.hpp>
#include <limits.h>
#include <physical-buddy.hpp>

namespace eir {

// Address of the DTB.
constinit physaddr_t eirDtbPtr{0};
constinit physaddr_t eirRsdpAddr{0};
constinit physaddr_t eirSmbios3Addr{0};

void *initrd = nullptr;

address_t allocatedMemory;
frg::span<uint8_t> kernel_image{nullptr, 0};
address_t kernel_physical = SIZE_MAX;
frg::span<uint8_t> initrd_image{nullptr, 0};
// Start address of a physical map provided by the bootloader. Defaults to 0.
address_t physOffset = 0;

PerCpuRegion perCpuRegion{0, 0};
DebugCapabilities eirDebugCapabilities{};

CpuConfig cpuConfig{0};
DebugOptions debugOptions{};
AcpiData acpiDataNote{};
constinit DtData dtDataNote{};
EirFramebuffer framebufferNote{};
Initrd initrdNote{};
PhysicalMemory physicalMemoryNote{};
CommandLine commandLineNote{};

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

Region regions[eirMaxMemoryRegions];

Region *obtainRegion() {
	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::null)
			continue;
		regions[i].regionType = RegionType::unconstructed;
		return &regions[i];
	}
	eir::panicLogger() << "Eir: Memory region limit exhausted" << frg::endlog;
	__builtin_unreachable();
}

void createInitialRegion(address_t base, address_t size) {
	auto limit = base + size;

	// Split regions into parts below 4 GiB and above 4 GiB.
	// This allows Thor to leave more memory available for devices that support only 32-bit DMA.
	constexpr address_t low4GiB = address_t{1} << 32;
	if (base < low4GiB && limit > low4GiB) {
		createInitialRegion(base, low4GiB - base);
		createInitialRegion(low4GiB, limit - low4GiB);
		return;
	}

	address_t address = base;

	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~address_t(0x1FFFFF);

	if (address >= limit) {
		eir::infoLogger() << "eir: Discarding memory region at 0x" << frg::hex_fmt{base}
		                  << " (smaller than alignment)" << frg::endlog;
		return;
	}

	// Trash the initial memory to find bugs in thor.
	// TODO: This code fails for size > 2^32.
	/*
	auto accessor = reinterpret_cast<uint8_t *>(address);
	uint64_t pattern = 0xB306'94E7'F8D2'78AB;
	for(ptrdiff_t i = 0; i < limit - address; ++i) {
	    accessor[i] = pattern;
	    pattern = (pattern << 8) | (pattern >> 56);
	}
	asm volatile ("" : : : "memory");
	*/

	// For now we ensure that the kernel has some memory to work with.
	// TODO: Handle small memory regions.
	if (limit - address < 32 * address_t(0x100000)) {
		eir::infoLogger() << "eir: Discarding memory region at 0x" << frg::hex_fmt{base}
		                  << " (smaller than minimum size)" << frg::endlog;
		return;
	}

	assert(!(address % pageSize));
	assert(!(limit % pageSize));

	auto region = obtainRegion();
	region->regionType = RegionType::allocatable;
	region->address = address;
	region->size = limit - address;
}

void createInitialRegions(InitialRegion region, frg::span<InitialRegion> reserved) {
	if (!reserved.size()) {
		createInitialRegion((region.base + 0xFFF) & ~0xFFF, region.size & ~0xFFF);
	} else {
		auto rsv = reserved.data()[0];

		if (rsv.base > (region.base + region.size) || (rsv.base + rsv.size) < region.base) {
			createInitialRegions(region, {reserved.data() + 1, reserved.size() - 1});
			return;
		}

		if (rsv.base > region.base) {
			createInitialRegions(
			    {region.base, rsv.base - region.base}, {reserved.data() + 1, reserved.size() - 1}
			);
		}

		if (rsv.base + rsv.size < region.base + region.size) {
			createInitialRegions(
			    {rsv.base + rsv.size, region.base + region.size - (rsv.base + rsv.size)},
			    {reserved.data() + 1, reserved.size() - 1}
			);
		}
	}
}

address_t cutFromRegion(size_t size) {
	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		if (regions[i].size < size)
			continue;

		regions[i].size -= size;

		// Discard this region if it's smaller than alignment
		if (regions[i].size < 0x200000)
			regions[i].regionType = RegionType::null;

		return regions[i].address + regions[i].size;
	}

	eir::panicLogger() << "Eir: Unable to cut memory from a region" << frg::endlog;
	__builtin_unreachable();
}

void setupRegionStructs() {
	for (size_t j = eirMaxMemoryRegions; j > 0; j--) {
		size_t i = j - 1;

		if (regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto preRoots = regions[i].size >> (pageShift + order);
		auto overhead = BuddyAccessor::determineSize(preRoots, order);
		overhead = (overhead + address_t(pageSize - 1)) & ~address_t(pageSize - 1);

		assert(overhead >= preRoots * (1 << (order + 1)));

		regions[i].buddyTree = cutFromRegion(overhead);
		regions[i].buddyOverhead = overhead;
	}

	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> pageShift);
		auto numRoots = regions[i].size >> (pageShift + order);
		assert(numRoots >= 32);

		assert(regions[i].size / 4096 >= numRoots * (1 << order));

		regions[i].order = order;
		regions[i].numRoots = numRoots;

		// Finally initialize the buddy tree.
		auto tablePtr = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor::initialize(tablePtr, numRoots, order);
	}
}

// ----------------------------------------------------------------------------
// Firmware memory map handling.
// ----------------------------------------------------------------------------

namespace {

constexpr size_t maxFirmwareMemoryEntries = 512;

EirFirmwareMemory firmwareMemoryEntries[maxFirmwareMemoryEntries];
size_t numFirmwareMemoryEntries = 0;

// Complains about type/attribute combinations that thor would map in a surprising way,
// either because the firmware lied to us or because we misclassified the entry.
void checkFirmwareMemoryAttrs(address_t address, EirMemoryType type, uint32_t attributes) {
	auto complain = [&](const char *what) {
		eir::infoLogger() << "eir: Firmware memory at 0x" << frg::hex_fmt{address} << " " << what
		                  << " (attributes: 0x" << frg::hex_fmt{attributes} << ")" << frg::endlog;
	};

	switch (type) {
		case EirMemoryType::usableRam:
		case EirMemoryType::acpiReclaimable:
		case EirMemoryType::acpiNvs:
			if (!(attributes & eir_memory_attrs::wb))
				complain("is RAM but does not support write-back caching");
			break;
		case EirMemoryType::mmio:
			if (attributes & eir_memory_attrs::wb)
				complain("is MMIO but advertises write-back caching");
			break;
		default:
			break;
	}
}

} // namespace

void
reportFirmwareMemory(address_t address, address_t size, EirMemoryType type, uint32_t attributes) {
	if (!size)
		return;

	checkFirmwareMemoryAttrs(address, type, attributes);

	// Merge with the previous entry if possible; firmware maps are usually sorted.
	if (numFirmwareMemoryEntries) {
		auto &last = firmwareMemoryEntries[numFirmwareMemoryEntries - 1];
		if (last.type == type && last.attributes == attributes
		    && last.address + last.size == address) {
			last.size += size;
			return;
		}
	}

	if (numFirmwareMemoryEntries >= maxFirmwareMemoryEntries)
		eir::panicLogger() << "Eir: Firmware memory map entry limit exhausted" << frg::endlog;
	firmwareMemoryEntries[numFirmwareMemoryEntries++] = {address, size, type, attributes};
}

void serializeFirmwareMemoryMap() {
	// Note that frg::insertion_sort() swaps whenever the comparator returns true,
	// i.e., this sorts by ascending address.
	frg::insertion_sort(
	    firmwareMemoryEntries,
	    firmwareMemoryEntries + numFirmwareMemoryEntries,
	    [](const EirFirmwareMemory &a, const EirFirmwareMemory &b) { return a.address > b.address; }
	);

	// Coalesce contiguous entries of the same type and attributes.
	size_t n = 0;
	for (size_t i = 0; i < numFirmwareMemoryEntries; ++i) {
		auto &entry = firmwareMemoryEntries[i];
		if (n && firmwareMemoryEntries[n - 1].type == entry.type
		    && firmwareMemoryEntries[n - 1].attributes == entry.attributes
		    && firmwareMemoryEntries[n - 1].address + firmwareMemoryEntries[n - 1].size
		           == entry.address) {
			firmwareMemoryEntries[n - 1].size += entry.size;
			continue;
		}
		firmwareMemoryEntries[n++] = entry;
	}
	numFirmwareMemoryEntries = n;

	auto bootstrapData =
	    BootstrapData::place(n * sizeof(EirFirmwareMemory), alignof(EirFirmwareMemory));
	bootstrapData.writeArray(std::span{firmwareMemoryEntries, n});

	physicalMemoryNote.numFirmwareEntries = n;
	physicalMemoryNote.firmwareEntriesPtr = bootstrapData.kernelAddress();
}

// ----------------------------------------------------------------------------

physaddr_t bootReserve(size_t length, size_t alignment) {
	assert(length <= pageSize);
	assert(alignment <= pageSize);

	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor accessor{
		    regions[i].address, pageShift, table, regions[i].numRoots, regions[i].order
		};
		auto physical = accessor.allocate(0, sizeof(uintptr_t) * CHAR_BIT);
		if (physical == BuddyAccessor::illegalAddress)
			continue;
		return physical;
	}

	eir::panicLogger() << "Eir: Out of memory" << frg::endlog;
	__builtin_unreachable();
}

physaddr_t allocPage() {
	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = physToVirt<int8_t>(regions[i].buddyTree);
		BuddyAccessor accessor{
		    regions[i].address, pageShift, table, regions[i].numRoots, regions[i].order
		};
		auto physical = accessor.allocate(0, sizeof(uintptr_t) * CHAR_BIT);
		if (physical == BuddyAccessor::illegalAddress)
			continue;
		allocatedMemory += pageSize;
		return physical;
	}

	eir::panicLogger() << "Eir: Out of memory" << frg::endlog;
	__builtin_unreachable();
}

// ----------------------------------------------------------------------------

namespace {
constexpr int kasanShift = 3;
constexpr address_t kasanShadowDelta = 0xdfffe00000000000;

constexpr size_t kasanScale = size_t{1} << kasanShift;

address_t kasanToShadow(address_t address) { return kasanShadowDelta + (address >> kasanShift); }

void setShadowRange(address_t base, size_t size, int8_t value) {
	assert(!(base & (kasanScale - 1)));
	assert(!(size & (kasanScale - 1)));

	size_t progress = 0;
	while (progress < size) {
		auto shadow = kasanToShadow(base + progress);
		auto page = shadow & ~address_t{pageSize - 1};
		auto physical = getSingle4kPage(page);
		assert(physical != static_cast<address_t>(-1));

		auto p = physToVirt<int8_t>(physical);
		auto n = shadow & (pageSize - 1);
		while (n < pageSize && progress < size) {
			assert(p[n] == static_cast<int8_t>(0xFF));
			p[n] = value;
			++n;
			progress += kasanScale;
		}
	}
};

void setShadowByte(address_t address, int8_t value) {
	assert(!(address & (kasanScale - 1)));

	auto shadow = kasanToShadow(address);
	auto page = shadow & ~address_t{pageSize - 1};
	auto physical = getSingle4kPage(page);
	assert(physical != static_cast<address_t>(-1));

	auto p = physToVirt<int8_t>(physical);
	auto n = shadow & (pageSize - 1);
	assert(p[n] == static_cast<int8_t>(0xFF));
	p[n] = value;
};
} // namespace

void mapKasanShadow(address_t base, size_t size) {
	if (!eirDebugCapabilities.kasan)
		return;

	assert(!(base & (kasanScale - 1)));

	eir::infoLogger() << "eir: Mapping KASAN shadow for 0x" << frg::hex_fmt{base} << ", size: 0x"
	                  << frg::hex_fmt{size} << frg::endlog;

	size = (size + kasanScale - 1) & ~(kasanScale - 1);

	for (address_t page = (kasanToShadow(base) & ~address_t{pageSize - 1});
	     page < ((kasanToShadow(base + size) + pageSize - 1) & ~address_t{pageSize - 1});
	     page += pageSize) {
		auto physical = getSingle4kPage(page);
		if (physical != static_cast<address_t>(-1))
			continue;
		physical = allocPage();
		memset(physToVirt<uint8_t>(physical), 0xFF, pageSize);
		mapSingle4kPage(page, physical, PageFlags::write | PageFlags::global);
	}
}

void unpoisonKasanShadow(address_t base, size_t size) {
	if (!eirDebugCapabilities.kasan)
		return;

	assert(!(base & (kasanScale - 1)));

	eir::infoLogger() << "eir: Unpoisoning KASAN shadow for 0x" << frg::hex_fmt{base}
	                  << ", size: 0x" << frg::hex_fmt{size} << frg::endlog;

	setShadowRange(base, size & ~(kasanScale - 1), 0);
	if (size & (kasanScale - 1))
		setShadowByte(base + (size & ~(kasanScale - 1)), size & (kasanScale - 1));
}

// ----------------------------------------------------------------------------

void mapRegionsAndStructs() {
	const auto &ml = getMemoryLayout();

#ifdef __x86_64__
	// This region should be available RAM on every PC.
	for (size_t page = 0x8000; page < 0x80000; page += pageSize) {
		mapSingle4kPage(ml.directPhysical + page, page, PageFlags::write | PageFlags::global);
		mapSingle4kPage(page, page, PageFlags::write | PageFlags::global | PageFlags::execute);
	}

	mapKasanShadow(ml.directPhysical + 0x8000, 0x80000);
	unpoisonKasanShadow(ml.directPhysical + 0x8000, 0x80000);
#endif

	for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		if (regions[i].regionType != RegionType::allocatable)
			continue;

		// Map the region itself.
		for (address_t page = 0; page < regions[i].size; page += pageSize)
			mapSingle4kPage(
			    ml.directPhysical + regions[i].address + page,
			    regions[i].address + page,
			    PageFlags::write | PageFlags::global
			);
		mapKasanShadow(ml.directPhysical + regions[i].address, regions[i].size);
		unpoisonKasanShadow(ml.directPhysical + regions[i].address, regions[i].size);

		// Map the buddy tree (also to the direct physical map).
		address_t buddyMapping = ml.directPhysical + regions[i].buddyTree;
		for (address_t page = 0; page < regions[i].buddyOverhead; page += pageSize) {
			mapSingle4kPage(
			    buddyMapping + page,
			    regions[i].buddyTree + page,
			    PageFlags::write | PageFlags::global
			);
		}
		mapKasanShadow(buddyMapping, regions[i].buddyOverhead);
		unpoisonKasanShadow(buddyMapping, regions[i].buddyOverhead);
		regions[i].buddyMap = buddyMapping;
	}
}

void allocLogRingBuffer() {
	const auto &ml = getMemoryLayout();
	for (size_t i = 0; i < ml.allocLogSize; i += pageSize)
		mapSingle4kPage(ml.allocLog + i, allocPage(), PageFlags::write | PageFlags::global);
	mapKasanShadow(ml.allocLog, ml.allocLogSize);
	unpoisonKasanShadow(ml.allocLog, ml.allocLogSize);
}

// ----------------------------------------------------------------------------
// Bootstrap information handling.
// ----------------------------------------------------------------------------

address_t bootstrapDataPointer = 0;

BootstrapData BootstrapData::place(size_t size, size_t alignment) {
	// Placements always start at a page boundary.
	assert(alignment <= pageSize);

	if (!bootstrapDataPointer)
		bootstrapDataPointer = getMemoryLayout().bootstrapData;

	auto address = bootstrapDataPointer;
	bootstrapDataPointer += (size + pageSize - 1) & ~(pageSize - 1);
	if (bootstrapDataPointer > getMemoryLayout().bootstrapData + bootstrapDataSize)
		panicLogger() << "eir: Bootstrap data region is exhausted" << frg::endlog;
	return BootstrapData{address, size};
}

void BootstrapData::writeBytes(std::span<const std::byte> bytes) {
	assert(offset_ + bytes.size() <= size_);

	while (!bytes.empty()) {
		auto misalign = offset_ & (pageSize - 1);
		// Map a new page whenever the placement crosses a page boundary.
		if (!misalign) {
			auto physical = allocPage();
			mapSingle4kPage(address_ + offset_, physical, 0);
			mapKasanShadow(address_ + offset_, pageSize);
			unpoisonKasanShadow(address_ + offset_, pageSize);
			window_ = physToVirt<std::byte>(physical);
		}

		auto chunk = frg::min(bytes.size(), pageSize - misalign);
		memcpy(window_ + misalign, bytes.data(), chunk);
		offset_ += chunk;
		bytes = bytes.subspan(chunk);
	}
}

// ----------------------------------------------------------------------------

uint64_t kernelEntry;

namespace {

template <typename F>
void forEachManagarmElfNote(char *image, F fn) {
	// Note that the EHDR magic is already validated at this point.
	Elf64_Ehdr ehdr;
	memcpy(&ehdr, image, sizeof(Elf64_Ehdr));

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(&phdr, image + ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(Elf64_Phdr));
		if (phdr.p_type != PT_NOTE)
			continue;
		if (phdr.p_memsz != phdr.p_filesz)
			panicLogger() << "Eir does not support p_filesz != p_memsz for PT_NOTE" << frg::endlog;
		size_t offset = 0;
		while (offset < phdr.p_memsz) {
			Elf64_Nhdr nhdr;
			memcpy(&nhdr, image + phdr.p_offset + offset, sizeof(Elf64_Nhdr));
			offset += sizeof(Elf64_Nhdr);

			auto *namePtr = image + phdr.p_offset + offset;
			offset += nhdr.n_namesz + 1;
			offset = (offset + 7) & ~size_t{7};
			auto *descPtr = image + phdr.p_offset + offset;
			offset += nhdr.n_descsz;
			offset = (offset + 7) & ~size_t{7};

			frg::string_view name{namePtr, nhdr.n_namesz};
			frg::span<char> desc{descPtr, nhdr.n_descsz};
			if (name != "Managarm")
				continue;
			fn(nhdr.n_type, desc);
		}
	}
}

bool parseGenericManagarmElfNote(unsigned int type, frg::span<char> desc) {
	if (type == elf_note_type::perCpuRegion) {
		if (desc.size() != sizeof(PerCpuRegion))
			panicLogger() << "PerCpuRegion size does not match ELF note" << frg::endlog;
		memcpy(&perCpuRegion, desc.data(), sizeof(PerCpuRegion));
		return true;
	} else if (type == elf_note_type::debugCapabilities) {
		if (desc.size() != sizeof(DebugCapabilities))
			panicLogger() << "DebugCapabilities size does not match ELF note" << frg::endlog;
		memcpy(&eirDebugCapabilities, desc.data(), sizeof(DebugCapabilities));
		return true;
	}
	return false;
}

} // namespace

void parseInitrd(void *initrd) {
	CpioRange cpio_range{reinterpret_cast<void *>(initrd)};
	auto initrd_end = reinterpret_cast<uintptr_t>(cpio_range.eof());
	eir::infoLogger() << "Initrd ends at " << (void *)initrd_end << frg::endlog;
	initrd_image = frg::span<uint8_t>{
	    reinterpret_cast<uint8_t *>(initrd), initrd_end - reinterpret_cast<uintptr_t>(initrd)
	};

	for (auto entry : cpio_range) {
		if (entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	if (!kernel_image.data() || !kernel_image.size())
		eir::panicLogger() << "eir: could not find thor in the initrd.cpio" << frg::endlog;

	auto image = reinterpret_cast<char *>(kernel_image.data());

	// Validate the kernel's ELF magic.
	Elf64_Ehdr ehdr;
	memcpy(&ehdr, image, sizeof(Elf64_Ehdr));
	if (ehdr.e_ident[0] != '\x7F' || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L'
	    || ehdr.e_ident[3] != 'F') {
		eir::panicLogger() << "Illegal magic fields" << frg::endlog;
	}
	assert(ehdr.e_type == ET_EXEC);

	forEachManagarmElfNote(image, [](unsigned int type, frg::span<char> desc) {
		if (elf_note_type::isThorConfiguration(type))
			return;
		if (elf_note_type::isThorGenericCapability(type)) {
			if (!parseGenericManagarmElfNote(type, desc))
				panicLogger() << "Failed to parse generic Managarm ELF note"
				              << " with type 0x" << frg::hex_fmt{type} << frg::endlog;
		} else {
			panicLogger() << "Managarm ELF note type 0x" << frg::hex_fmt{type}
			              << " is not within known range" << frg::endlog;
		}
	});
}

namespace {

bool patchGenericManagarmElfNote(unsigned int type, frg::span<char> desc) {
	if (type == elf_note_type::memoryLayout) {
		if (desc.size() != sizeof(MemoryLayout))
			panicLogger() << "MemoryLayout size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &getMemoryLayout(), sizeof(MemoryLayout));
		return true;
	} else if (type == elf_note_type::cpuConfig) {
		if (desc.size() != sizeof(CpuConfig))
			panicLogger() << "CpuConfig size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &cpuConfig, sizeof(CpuConfig));
		return true;
	} else if (type == elf_note_type::smbiosData) {
		if (desc.size() != sizeof(SmbiosData))
			panicLogger() << "SmbiosData size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &eirSmbios3Addr, sizeof(SmbiosData));
		return true;
	} else if (type == elf_note_type::bootUartConfig) {
		if (desc.size() != sizeof(BootUartConfig))
			panicLogger() << "BootUartConfig size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &uart::bootUartConfig, sizeof(BootUartConfig));
		return true;
	} else if (type == elf_note_type::debugOptions) {
		if (desc.size() != sizeof(DebugOptions))
			panicLogger() << "DebugOptions size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &debugOptions, sizeof(DebugOptions));
		return true;
	} else if (type == elf_note_type::acpiData) {
		if (desc.size() != sizeof(AcpiData))
			panicLogger() << "AcpiData size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &acpiDataNote, sizeof(AcpiData));
		return true;
	} else if (type == elf_note_type::dtData) {
		if (desc.size() != sizeof(DtData))
			panicLogger() << "DtData size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &dtDataNote, sizeof(DtData));
		return true;
	} else if (type == elf_note_type::framebuffer) {
		if (desc.size() != sizeof(EirFramebuffer))
			panicLogger() << "EirFramebuffer size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &framebufferNote, sizeof(EirFramebuffer));
		return true;
	} else if (type == elf_note_type::initrd) {
		if (desc.size() != sizeof(Initrd))
			panicLogger() << "Initrd size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &initrdNote, sizeof(Initrd));
		return true;
	} else if (type == elf_note_type::commandLine) {
		if (desc.size() != sizeof(CommandLine))
			panicLogger() << "CommandLine size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &commandLineNote, sizeof(CommandLine));
		return true;
	} else if (type == elf_note_type::physicalMemory) {
		if (desc.size() != sizeof(PhysicalMemory))
			panicLogger() << "PhysicalMemory size does not match ELF note" << frg::endlog;
		memcpy(desc.data(), &physicalMemoryNote, sizeof(PhysicalMemory));
		return true;
	}
	return false;
}

} // namespace

void loadKernelImage(void *imagePtr) {
	auto image = reinterpret_cast<char *>(imagePtr);

	// Note that the EHDR magic is already validated at this point.
	Elf64_Ehdr ehdr;
	memcpy(&ehdr, image, sizeof(Elf64_Ehdr));

	// Read and patch Thor's ELF notes.
	forEachManagarmElfNote(image, [](unsigned int type, frg::span<char> desc) {
		if (elf_note_type::isThorCapability(type))
			return;
		if (elf_note_type::isThorGenericConfiguration(type)) {
			if (!patchGenericManagarmElfNote(type, desc))
				panicLogger() << "Failed to patch generic Managarm ELF note"
				              << " with type 0x" << frg::hex_fmt{type} << frg::endlog;
		} else if (elf_note_type::isThorArchSpecificConfiguration(type)) {
			if (!patchArchSpecificManagarmElfNote(type, desc))
				panicLogger() << "Failed to patch arch-specific Managarm ELF note"
				              << " with type 0x" << frg::hex_fmt{type} << frg::endlog;
		} else {
			panicLogger() << "Managarm ELF note type 0x" << frg::hex_fmt{type}
			              << " is not within known range" << frg::endlog;
		}
	});

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		memcpy(
		    &phdr,
		    (void *)((uintptr_t)image + (uintptr_t)ehdr.e_phoff + i * ehdr.e_phentsize),
		    sizeof(Elf64_Phdr)
		);
		if (phdr.p_type != PT_LOAD)
			continue;
		assert(!(phdr.p_offset & (pageSize - 1)));
		assert(!(phdr.p_vaddr & (pageSize - 1)));

		uint32_t map_flags = PageFlags::global;
		if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
			// no additional flags
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= PageFlags::write;
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= PageFlags::execute;
		} else if ((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W | PF_X)) {
			eir::infoLogger() << "eir: warning: Mapping PHDR with RWX permissions" << frg::endlog;
			map_flags |= PageFlags::write | PageFlags::execute;
		} else {
			eir::panicLogger() << "Illegal combination of segment permissions" << frg::endlog;
		}

		uintptr_t pg = 0;
		while (pg < (uintptr_t)phdr.p_memsz) {
			auto backing = allocPage();
			auto backingVirt = physToVirt<uint8_t>(backing);
			memset(backingVirt, 0, pageSize);
			if (pg < (uintptr_t)phdr.p_filesz)
				memcpy(
				    backingVirt,
				    reinterpret_cast<void *>((uintptr_t)image + (uintptr_t)phdr.p_offset + pg),
				    frg::min(pageSize, (uintptr_t)phdr.p_filesz - pg)
				);
			mapSingle4kPage(phdr.p_vaddr + pg, backing, map_flags);
			pg += pageSize;
		}
		mapKasanShadow(phdr.p_paddr, phdr.p_memsz);
		unpoisonKasanShadow(phdr.p_paddr, phdr.p_memsz);
	}

	// Map the per-CPU regions for all CPUs.
	{
		if (!cpuConfig.effectiveCpus)
			panicLogger() << "eir: Could not detect number of CPUs" << frg::endlog;
		assert(perCpuRegion.start && perCpuRegion.end);

		auto singleSize = perCpuRegion.end - perCpuRegion.start;
		assert(!(perCpuRegion.start & 0xFFF));
		assert(!(perCpuRegion.end & 0xFFF));
		assert(!(singleSize & 0xFFF));

		// Allocate and map regions for CPUs > 0.
		for (size_t cpu = 1; cpu < cpuConfig.effectiveCpus; ++cpu) {
			auto address = perCpuRegion.end + singleSize * (cpu - 1);
			for (size_t pg = 0; pg < singleSize; pg += pageSize) {
				auto physical = allocPage();
				memset(physToVirt<uint8_t>(physical), 0, pageSize);
				mapSingle4kPage(address + pg, physical, PageFlags::write | PageFlags::global);
			}

			mapKasanShadow(address, singleSize);
			unpoisonKasanShadow(address, singleSize);
		}
	}

	kernelEntry = ehdr.e_entry;
}

namespace {

static initgraph::Task parseCmdlineTask{
    &globalInitEngine,
    "generic.parse-cmdline",
    initgraph::Requires{getCmdlineAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    bool serial{false};
	    bool kernelProfile{false};
	    bool sif{false};
	    frg::string_view ubsan;
	    frg::array options = {
	        frg::option{"serial", frg::store_true(serial)},
	        frg::option{"kernel-profile", frg::store_true(kernelProfile)},
	        frg::option{"sif", frg::store_true(sif)},
	        frg::option{"thor-ubsan", frg::as_string_view(ubsan)},
	    };
	    parseCmdline(options);

	    if (serial)
		    debugOptions.flags |= eirDebugSerial;
	    if (logE9)
		    debugOptions.flags |= eirDebugBochs;
	    if (kernelProfile)
		    debugOptions.flags |= eirDebugKernelProfile;
	    if (sif)
		    debugOptions.useSif = true;

	    if (ubsan.size()) {
		    if (ubsan == "ignore") {
			    debugOptions.ubsanAbort = false;
		    } else if (ubsan == "abort") {
			    debugOptions.ubsanAbort = true;
		    } else {
			    infoLogger() << "eir: Unknown value for 'ubsan' command line: " << ubsan
			                 << frg::endlog;
		    }
	    }
    }
};

static initgraph::Task composeCommandLine{
    &globalInitEngine,
    "generic.compose-cmdline",
    initgraph::Requires{getCmdlineAvailableStage(), getKernelMappableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    auto cmdlineChunks = getCmdline();

	    // For each chunk: we either have a trailing space or null terminator.
	    // Without any chunks, we still write the null terminator.
	    auto cmdlineLength = frg::max(cmdlineChunks.size(), size_t{1});
	    for (auto chunk : cmdlineChunks)
		    cmdlineLength += chunk.size();

	    auto bootstrapData = BootstrapData::place(cmdlineLength, alignof(char));

	    auto logger = infoLogger();
	    logger << "eir: Kernel command line: '";
	    bool first = true;
	    for (auto chunk : cmdlineChunks) {
		    if (!chunk.size())
			    continue;
		    if (!first) {
			    bootstrapData.write(' ');
			    logger << ' ';
		    }
		    bootstrapData.writeArray(std::span{chunk.data(), chunk.size()});
		    logger << chunk;
		    first = false;
	    }
	    bootstrapData.write('\0');
	    logger << "'" << frg::endlog;

	    commandLineNote.ptr = bootstrapData.kernelAddress();
    }
};

} // namespace

} // namespace eir
