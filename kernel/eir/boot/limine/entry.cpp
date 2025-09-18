#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/arch-generic/stack.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir/interface.hpp>
#include <frg/cmdline.hpp>
#include <stdint.h>
#include <uacpi/acpi.h>

#include "limine.h"

namespace eir {

namespace {

#define LIMINE_REQUEST(request, tag, rev)                                                          \
	[[gnu::used, gnu::section(".requests")]]                                                       \
	volatile struct limine_##request request = {                                                   \
	    .id = tag,                                                                                 \
	    .revision = rev,                                                                           \
	    .response = nullptr,                                                                       \
	}

[[gnu::used, gnu::section(".requestsStartMarker")]] volatile LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".requests")]] volatile LIMINE_BASE_REVISION(3);
LIMINE_REQUEST(memmap_request, LIMINE_MEMMAP_REQUEST, 0);
LIMINE_REQUEST(hhdm_request, LIMINE_HHDM_REQUEST, 0);
LIMINE_REQUEST(riscv_bsp_hartid_request, LIMINE_RISCV_BSP_HARTID_REQUEST, 0);
LIMINE_REQUEST(framebuffer_request, LIMINE_FRAMEBUFFER_REQUEST, 1);
LIMINE_REQUEST(module_request, LIMINE_MODULE_REQUEST, 0);
LIMINE_REQUEST(kernel_file_request, LIMINE_KERNEL_FILE_REQUEST, 0);
LIMINE_REQUEST(kernel_address_request, LIMINE_KERNEL_ADDRESS_REQUEST, 0);
LIMINE_REQUEST(rsdp_request, LIMINE_RSDP_REQUEST, 0);
LIMINE_REQUEST(dtb_request, LIMINE_DTB_REQUEST, 0);
LIMINE_REQUEST(smbios_request, LIMINE_SMBIOS_REQUEST, 0);
[[gnu::used, gnu::section(".requestsEndMarker")]] volatile LIMINE_REQUESTS_END_MARKER;

initgraph::Task obtainFirmwareTables{
    &globalInitEngine,
    "limine.obtain-firmware-tables",
    initgraph::Entails{getInfoStructAvailableStage(), acpi::getRsdpAvailableStage()},
    [] {
	    if (rsdp_request.response) {
		    eirRsdpAddr = reinterpret_cast<uint64_t>(rsdp_request.response->address);
	    }
	    if (smbios_request.response) {
		    eirSmbios3Addr = reinterpret_cast<uint64_t>(smbios_request.response->entry_64);
	    }
    }
};

initgraph::Task setupMiscInfo{
    &globalInitEngine,
    "limine.setup-misc-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
#ifdef __riscv
	    if (!riscv_bsp_hartid_request.response)
		    panicLogger() << "eir: Missing response for Limine BSP hart ID request" << frg::endlog;
	    info_ptr->hartId = riscv_bsp_hartid_request.response->bsp_hartid;
#endif
    }
};

initgraph::Task setupFramebufferInfo{
    &globalInitEngine,
    "limine.setup-framebuffer-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
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

	if (!LIMINE_BASE_REVISION_SUPPORTED)
		panicLogger() << "eir-limine was not booted with correct base revision" << frg::endlog;

	// Must be available before we can use virtToPhys below.
	physOffset = hhdm_request.response->offset;

	if (dtb_request.response) {
		eir::infoLogger() << "DTB accessible at " << dtb_request.response->dtb_ptr << frg::endlog;
		eirDtbPtr = virtToPhys(dtb_request.response->dtb_ptr);
	} else {
		eir::infoLogger() << "Limine did not pass a DTB" << frg::endlog;
	}

	assert(kernel_file_request.response);
	cmdline = frg::string_view{kernel_file_request.response->kernel_file->cmdline};
	eir::infoLogger() << "Command line: " << cmdline << frg::endlog;

	frg::array args = {
	    frg::option{"bochs", frg::store_true(log_e9)},
	};
	frg::parse_arguments(cmdline, args);

	assert(module_request.response);
	assert(module_request.response->module_count > 0);
	auto initrd_file = module_request.response->modules[0];

	initrd = initrd_file->address;
	kernel_physical = kernel_address_request.response->physical_base;

	// Enter a stack that is part of Eir's image.
	// This ensures that we can still access the stack when paging in enabled.
	runOnStack([] { eirMain(); }, eirStackTop);
}

} // namespace eir
