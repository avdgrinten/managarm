#include <assert.h>
#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/spec.hpp>
#include <eir/interface.hpp>
#include <stdint.h>
#include <uacpi/acpi.h>

namespace eir {

namespace {

static constinit BootCaps mb2Caps = {
    .hasMemoryMap = true,
};

[[gnu::constructor]] void initBootCaps() {
	mb2Caps.imageStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	mb2Caps.imageEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
}

eir::Mb2Info *mbInfo = nullptr;

uintptr_t mmapStart = 0;
uintptr_t mmapEnd = 0;

eir::Mb2Tag *acpiTag = nullptr;

initgraph::Task setupAcpiInfo{
    &globalInitEngine,
    "mb2.setup-acpi-info",
    initgraph::Requires{getAllocationAvailableStage()},
    initgraph::Entails{getKernelLoadableStage(), acpi::getRsdpAvailableStage()},
    [] {
	    if (acpiTag) {
		    auto *rsdpPtr = bootAlloc<uint8_t>(acpiTag->size - sizeof(Mb2TagRSDP));
		    memcpy(rsdpPtr, acpiTag->data, acpiTag->size - sizeof(Mb2TagRSDP));
		    eirRsdpAddr = reinterpret_cast<uint64_t>(rsdpPtr);
	    }
    }
};

initgraph::Task setupMemoryRegions{
    &globalInitEngine,
    "mb2.setup-memory-regions",
    initgraph::Requires{getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    assert(mmapStart);
	    assert(mmapEnd > mmapStart);

	    eir::infoLogger() << "Memory map:" << frg::endlog;
	    for (Mb2MmapEntry *map = (Mb2MmapEntry *)mmapStart; map < (Mb2MmapEntry *)mmapEnd; map++) {
		    eir::infoLogger() << "    Type " << map->type << " mapping."
		                      << " Base: 0x" << frg::hex_fmt{map->base} << ", length: 0x"
		                      << frg::hex_fmt{map->length} << frg::endlog;

		    // The entry types are the E820 memory types.
		    switch (map->type) {
			    case 1: // Available RAM.
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::usableRam, eir_memory_attrs::wb
				    );
				    break;
			    case 3: // ACPI reclaimable.
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::acpiReclaimable, eir_memory_attrs::wb
				    );
				    break;
			    case 4: // ACPI NVS.
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::acpiNvs, eir_memory_attrs::wb
				    );
				    break;
			    case 2: // Reserved.
			    case 5: // Defective RAM.
				    reportFirmwareMemory(map->base, map->length, EirMemoryType::reserved, 0);
				    break;
			    default:
				    eir::infoLogger()
				        << "eir: Unknown E820 memory type " << map->type << frg::endlog;
				    reportFirmwareMemory(map->base, map->length, EirMemoryType::reserved, 0);
		    }

		    if (map->type == 1)
			    createInitialRegions(
			        {map->base, map->length}, {reservedRegions.data(), nReservedRegions}
			    );
	    }
    }
};

} // namespace

const BootCaps &BootCaps::get() { return mb2Caps; };

extern "C" void eirMultiboot2Main(uint32_t info, uint32_t magic) {
	initPlatform();

	if (magic != MB2_MAGIC)
		eir::panicLogger() << "eir: Invalid multiboot2 signature, halting..." << frg::endlog;

	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {0, eirEnd};

	mbInfo = reinterpret_cast<Mb2Info *>(info);
	size_t add_size = 0;
	size_t n_modules = 0;

	Mb2Tag *oldAcpiTag = nullptr, *newAcpiTag = nullptr;

	for (size_t i = 8 /* Skip size and reserved fields*/; i < mbInfo->size; i += add_size) {
		Mb2Tag *tag = (Mb2Tag *)((uint8_t *)info + i);

		if (tag->type == kMb2TagEnd)
			break;

		add_size = tag->size;
		if ((add_size % 8) != 0)
			add_size += (8 - add_size % 8); // Align 8byte

		switch (tag->type) {
			case kMb2TagFramebuffer: {
				auto *framebuffer_tag = reinterpret_cast<Mb2TagFramebuffer *>(tag);

				initFramebuffer(
				    EirFramebuffer{
				        .fbAddress = framebuffer_tag->address,
				        .fbPitch = framebuffer_tag->pitch,
				        .fbWidth = framebuffer_tag->width,
				        .fbHeight = framebuffer_tag->height,
				        .fbBpp = framebuffer_tag->bpp,
				    }
				);
				break;
			}
			case kMb2TagModule: {
				auto *module = reinterpret_cast<Mb2TagModule *>(tag);

				if (n_modules)
					eir::panicLogger() << "eir: only one module is supported!" << frg::endlog;

				rawInitrd = reinterpret_cast<void *>(module->start);
				rawInitrdSize = module->end - module->start;
				reservedRegions[nReservedRegions++] = {module->start, module->end - module->start};

				n_modules++;
				break;
			}
			case kMb2TagMmap: {
				auto *mmap = reinterpret_cast<Mb2TagMmap *>(tag);

				mmapStart = (uintptr_t)mmap->entries;
				mmapEnd = (uintptr_t)mmap + mmap->length;

				break;
			}
			case kMb2TagCmdline: {
				auto *cmdline_tag = reinterpret_cast<Mb2TagCmdline *>(tag);

				extendCmdline({cmdline_tag->string});

				break;
			}
			case kMb2TagAcpiOld: {
				oldAcpiTag = tag;
				break;
			}
			case kMb2TagAcpiNew: {
				newAcpiTag = tag;
				break;
			}
			default:
				infoLogger() << "eir: Unknown MB2 tag " << tag->type << frg::endlog;
		}
	}

	if (newAcpiTag)
		acpiTag = newAcpiTag;
	else
		acpiTag = oldAcpiTag;

	eirMain();
}

} // namespace eir
