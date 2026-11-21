#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/arch-generic/stack.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/dtb/dtb.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir/interface.hpp>
#include <stdint.h>
#include <uacpi/acpi.h>

#include "limine.h"

namespace eir {

namespace {

#define LIMINE_REQUEST(request, tag, rev)                                                          \
	[[gnu::used, gnu::section(".requests")]]                                                       \
	volatile struct limine_##request request = {                                                   \
	    .id = tag, /* NOLINT(bugprone-macro-parentheses) */                                        \
	    .revision = (rev),                                                                         \
	    .response = nullptr,                                                                       \
	}

[[gnu::used, gnu::section(".requestsStartMarker")]] volatile uint64_t start_marker[] =
    LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".requests")]] volatile uint64_t base_revision[] =
    LIMINE_BASE_REVISION(6);

LIMINE_REQUEST(memmap_request, LIMINE_MEMMAP_REQUEST_ID, 0);
LIMINE_REQUEST(hhdm_request, LIMINE_HHDM_REQUEST_ID, 0);
LIMINE_REQUEST(riscv_bsp_hartid_request, LIMINE_RISCV_BSP_HARTID_REQUEST_ID, 0);
LIMINE_REQUEST(framebuffer_request, LIMINE_FRAMEBUFFER_REQUEST_ID, 1);
LIMINE_REQUEST(module_request, LIMINE_MODULE_REQUEST_ID, 0);
LIMINE_REQUEST(executable_file_request, LIMINE_EXECUTABLE_FILE_REQUEST_ID, 0);
LIMINE_REQUEST(executable_address_request, LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, 0);
LIMINE_REQUEST(rsdp_request, LIMINE_RSDP_REQUEST_ID, 0);
LIMINE_REQUEST(dtb_request, LIMINE_DTB_REQUEST_ID, 0);
LIMINE_REQUEST(smbios_request, LIMINE_SMBIOS_REQUEST_ID, 0);

[[gnu::used, gnu::section(".requestsEndMarker")]] volatile uint64_t end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

initgraph::Task obtainFirmwareTables{
    &globalInitEngine,
    "limine.obtain-firmware-tables",
    initgraph::Entails{
        getKernelLoadableStage(), acpi::getRsdpAvailableStage(), getDtbAvailableStage()
    },
    [] {
	    if (rsdp_request.response) {
		    eirRsdpAddr = virtToPhys(rsdp_request.response->address);
	    }
	    if (smbios_request.response) {
		    if (smbios_request.response->entry_64)
			    eirSmbios3Addr = virtToPhys(smbios_request.response->entry_64);
	    }
    }
};

initgraph::Task setupMiscInfo{
    &globalInitEngine, "limine.setup-misc-info", initgraph::Entails{getKernelLoadableStage()}, [] {
#ifdef __riscv
	    if (!riscv_bsp_hartid_request.response)
		    panicLogger() << "eir: Missing response for Limine BSP hart ID request" << frg::endlog;
	    eirBootHartId = riscv_bsp_hartid_request.response->bsp_hartid;
#endif
    }
};

initgraph::Task setupFramebufferInfo{
    &globalInitEngine,
    "limine.setup-framebuffer-info",
    initgraph::Entails{getFramebufferAvailableStage()},
    [] {
	    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0
	        && framebuffer_request.response->framebuffers) {
		    auto *limineFb = framebuffer_request.response->framebuffers[0];

		    initFramebuffer(
		        EirFramebuffer{
		            .fbAddress = virtToPhys(limineFb->address),
		            .fbPitch = limineFb->pitch,
		            .fbWidth = limineFb->width,
		            .fbHeight = limineFb->height,
		            .fbBpp = limineFb->bpp,
		        }
		    );
	    } else {
		    infoLogger() << "eir: Got no framebuffer!" << frg::endlog;
	    }
    }
};

initgraph::Task setupMemoryRegions{
    &globalInitEngine,
    "limine.setup-memory-regions",
    initgraph::Requires{getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    assert(memmap_request.response);

	    eir::infoLogger() << "Memory map:" << frg::endlog;
	    for (size_t entry = 0; entry < memmap_request.response->entry_count; entry++) {
		    auto *map = memmap_request.response->entries[entry];
		    eir::infoLogger() << "    Type " << map->type << " mapping."
		                      << " Base: 0x" << frg::hex_fmt{map->base} << ", length: 0x"
		                      << frg::hex_fmt{map->length} << frg::endlog;

		    // Limine guarantees that all RAM-like entry types are actual memory.
		    switch (map->type) {
			    case LIMINE_MEMMAP_USABLE:
			    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
				    // Eir is responsible for dealing with bootloader reclaimable memory.
				    // Hence, we map it to usableRam.
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::usableRam, eir_memory_attrs::wb
				    );
				    break;
			    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::acpiReclaimable, eir_memory_attrs::wb
				    );
				    break;
			    case LIMINE_MEMMAP_ACPI_NVS:
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::acpiNvs, eir_memory_attrs::wb
				    );
				    break;
			    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::reserved, eir_memory_attrs::wb
				    );
				    break;
			    case LIMINE_MEMMAP_FRAMEBUFFER:
				    reportFirmwareMemory(
				        map->base,
				        map->length,
				        EirMemoryType::framebuffer,
				        eir_memory_attrs::wc | eir_memory_attrs::uc
				    );
				    break;
			    case LIMINE_MEMMAP_RESERVED_MAPPED:
				    // Limine uses this type for ACPI tables that live outside of ACPI
				    // reclaimable memory (and for EFI runtime services).
				    reportFirmwareMemory(
				        map->base, map->length, EirMemoryType::reserved, eir_memory_attrs::wb
				    );
				    break;
			    case LIMINE_MEMMAP_RESERVED:
			    case LIMINE_MEMMAP_BAD_MEMORY:
				    reportFirmwareMemory(map->base, map->length, EirMemoryType::reserved, 0);
				    break;
			    default:
				    eir::infoLogger()
				        << "eir: Unknown Limine memory map type " << map->type << frg::endlog;
				    reportFirmwareMemory(map->base, map->length, EirMemoryType::reserved, 0);
		    }

		    if (map->type == LIMINE_MEMMAP_USABLE
		        || map->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
			    createInitialRegions(
			        {map->base, map->length}, {reservedRegions.data(), nReservedRegions}
			    );
	    }
    }
};

constinit BootCaps limineCaps = {
    .hasMemoryMap = true,
};

} // namespace

const BootCaps &BootCaps::get() { return limineCaps; };

extern "C" void eirLimineMain(void) {
	initPlatform();

	eir::infoLogger() << "Booting Eir from Limine" << frg::endlog;
	eirRunConstructors();

	limineCaps.imageStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	limineCaps.imageEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);

	if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))
		panicLogger() << "eir-limine was not booted with correct base revision" << frg::endlog;

	// Must be available before we can use virtToPhys below.
	physOffset = hhdm_request.response->offset;

	if (dtb_request.response) {
		eir::infoLogger() << "DTB accessible at " << dtb_request.response->dtb_ptr << frg::endlog;
		eirDtbPtr = virtToPhys(dtb_request.response->dtb_ptr);
	}

	assert(executable_file_request.response);
	extendCmdline(frg::string_view{executable_file_request.response->executable_file->string});

	assert(module_request.response);
	assert(module_request.response->module_count > 0);
	auto initrd_file = module_request.response->modules[0];

	rawInitrd = initrd_file->address;
	rawInitrdSize = initrd_file->size;
	kernel_physical = executable_address_request.response->physical_base;

	// Enter a stack that is part of Eir's image.
	// This ensures that we can still access the stack when paging is enabled.
	runOnStack([] { eirMain(); }, eirStackTop);
}

} // namespace eir
