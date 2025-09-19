#ifdef __x86_64__
#include <arch/io_space.hpp>
#endif
#include <assert.h>
#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/arch-generic/stack.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/array.hpp>
#include <frg/cmdline.hpp>
#include <frg/scope_exit.hpp>
#include <stdbool.h>

#include "efi.hpp"
#include "helpers.hpp"

static_assert(
    sizeof(char16_t) == sizeof(wchar_t),
    "Strings are not UTF-16-ish, are you missing -fshort-wchar?"
);

namespace eir {

const efi_system_table *st = nullptr;
const efi_boot_services *bs = nullptr;
efi_handle handle = nullptr;

namespace {

// The console output protocol is not terribly useful. In particular, it is only available
// before ExitBootServices. Also, it can easily collide with UART loggers provided by the platform
// code, causing characters to be printed twice.
constexpr bool useConOut = false;

efi_graphics_output_protocol *gop{nullptr};
unsigned int gopBpp{0};

efi_loaded_image_protocol *loadedImage = nullptr;

frg::string_view initrdPath = "managarm\\initrd.cpio";
size_t initrdSize = 0;

size_t memMapSize = 0;
size_t mapKey = 0;
size_t descriptorSize = 0;
uint32_t descriptorVersion = 0;
void *memMap = nullptr;

struct pxe_info {
	efi_ip_address station_ip;
	efi_ip_address subnet_mask;
	efi_ip_address server_ip;
	efi_ip_address gateway_ip;
	frg::string_view device_path{};
};

struct pxe_info *pxeInfo = nullptr;

bool overrideStation = false;
frg::string_view stationStr = "";
bool overrideSubnet = false;
frg::string_view subnetStr = "";
bool overrideGateway = false;
frg::string_view gatewayStr = "";
bool overrideServer = false;
frg::string_view serverStr = "";

// by reaching this we've performed all tasks that depend on EFI Boot Services
initgraph::Stage *getBootservicesDoneStage() {
	static initgraph::Stage s{&globalInitEngine, "uefi.bootservices-done"};
	return &s;
}

struct EirAllocator {
	void *allocate(size_t size) { return bootAlloc<char>(size); }

	void free(void *) { return; }
};

struct ConOutLogHandler : LogHandler {
	void emit(frg::string_view line) override {
		assert(bs);
		for (size_t i = 0; i < line.size(); ++i) {
			auto c = line[i];
			if (c == '\n') {
				frg::array<char16_t, 3> newline = {u"\r\n"};
				st->con_out->output_string(st->con_out, newline.data());
			} else {
				frg::array<char16_t, 2> converted = {static_cast<char16_t>(c), 0};
				st->con_out->output_string(st->con_out, converted.data());
			}
		}
		frg::array<char16_t, 3> newline = {u"\r\n"};
		st->con_out->output_string(st->con_out, newline.data());
	}
};

constinit ConOutLogHandler conOutLogHandler;

std::optional<physaddr_t> findConfigurationTable(efi_guid guid) {
	const efi_configuration_table *t = st->configuration_table;
	for (size_t i = 0; i < st->number_of_table_entries && t; i++, t++) {
		if (!memcmp(&guid, &t->vendor_guid, sizeof(guid))) {
			return reinterpret_cast<physaddr_t>(t->vendor_table);
		}
	}

	return std::nullopt;
}

initgraph::Task obtainFirmwareTables{
    &globalInitEngine,
    "uefi.obtain-firmware-tables",
    initgraph::Entails{
        getBootservicesDoneStage(), getInfoStructAvailableStage(), acpi::getRsdpAvailableStage()
    },
    [] {
	    eirDtbPtr = findConfigurationTable(EFI_DTB_TABLE_GUID).value_or(0);
	    eirRsdpAddr = findConfigurationTable(ACPI_20_TABLE_GUID).value_or(0);
	    eirSmbios3Addr = findConfigurationTable(SMBIOS3_TABLE_GUID).value_or(0);
    }
};

uint32_t convertIp(frg::string_view ip) {
	uint32_t res = 0;

	for (size_t i = 0; i < 4; i++) {
		auto dot = ip.find_first('.');
		if (dot == size_t(-1))
			dot = ip.size();

		auto num = ip.sub_string(0, dot).to_number<uint8_t>();
		assert(num);
		res |= num.value() << (8 * i);

		ip = ip.sub_string(dot + 1, ip.size() - dot - 1);
	}

	return res;
}

initgraph::Task preparePxe{
    &globalInitEngine, "uefi.pxe-setup", initgraph::Entails{getBootservicesDoneStage()}, [] {
	    efi_guid pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
	    efi_guid devpath_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	    efi_guid devpath2text_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
	    efi_pxe_base_code_protocol *pxe = nullptr;
	    efi_device_path_protocol *devpath = nullptr;
	    efi_device_path_to_text_protocol *devpath2text = nullptr;

	    auto status = bs->handle_protocol(
	        loadedImage->device_handle, &pxe_guid, reinterpret_cast<void **>(&pxe)
	    );
	    if (status != EFI_SUCCESS)
		    return;

	    EFI_CHECK(
	        bs->allocate_pool(EfiLoaderData, sizeof(pxe_info), reinterpret_cast<void **>(&pxeInfo))
	    );
	    new (pxeInfo) pxe_info{};

	    EFI_CHECK(bs->handle_protocol(
	        loadedImage->device_handle, &devpath_guid, reinterpret_cast<void **>(&devpath)
	    ));
	    EFI_CHECK(bs->locate_protocol(
	        &devpath2text_guid, nullptr, reinterpret_cast<void **>(&devpath2text)
	    ));

	    auto devpathstr = devpath2text->convert_device_path_to_text(devpath, true, true);
	    assert(devpathstr);

	    char *devpathascii = nullptr;
	    size_t devpathstr_len = 0;
	    while (devpathstr[devpathstr_len])
		    devpathstr_len++;

	    EFI_CHECK(bs->allocate_pool(
	        EfiLoaderData, devpathstr_len + 1, reinterpret_cast<void **>(&devpathascii)
	    ));

	    size_t i = 0;
	    for (; i < devpathstr_len; i++) {
		    auto c = devpathstr[i];
		    // we only use printable ASCII characters, everything else gets discarded
		    if (c >= 0x20 && c <= 0x7E) {
			    devpathascii[i] = static_cast<char>(c);
		    } else {
			    devpathascii[i] = '?';
		    }
	    }

	    // Null-terminate the buffer.
	    devpathascii[i] = '\0';

	    pxeInfo->device_path = frg::string_view{devpathascii};

	    eir::infoLogger() << "eir: PXE booted from device '" << pxeInfo->device_path << "'"
	                      << frg::endlog;

	    // TODO: support IPv6
	    if (pxe->mode->using_ipv6) {
		    eir::infoLogger() << "eir: PXE over IPv6 is unsupported" << frg::endlog;
		    return;
	    }

	    eir::infoLogger() << "eir: PXE available, " << (pxe->mode->started ? "started" : "stopped")
	                      << frg::endlog;

	    if (!pxe->mode->started) {
		    eir::infoLogger() << "eir: PXE protocol is not yet started, skipping" << frg::endlog;
		    return;
	    }

	    if (!overrideStation)
		    pxeInfo->station_ip = pxe->mode->station_ip;
	    else
		    pxeInfo->station_ip.addr[0] = convertIp(stationStr);

	    if (!overrideSubnet)
		    pxeInfo->subnet_mask = pxe->mode->subnet_mask;
	    else
		    pxeInfo->subnet_mask.addr[0] = convertIp(subnetStr);

	    if (!overrideServer) {
		    if (pxe->mode->pxe_reply_received) {
			    pxeInfo->server_ip.v4 = {};
			    memcpy(
			        &pxeInfo->server_ip.v4,
			        pxe->mode->pxe_reply.dhcpv4.bootp_si_addr,
			        sizeof(pxeInfo->server_ip.v4)
			    );
		    } else if (pxe->mode->proxy_offer_received) {
			    pxeInfo->server_ip.v4 = {};
			    memcpy(
			        &pxeInfo->server_ip.v4,
			        pxe->mode->proxy_offer.dhcpv4.bootp_si_addr,
			        sizeof(pxeInfo->server_ip.v4)
			    );
		    } else {
			    pxeInfo->server_ip.v4 = {};
			    memcpy(
			        &pxeInfo->server_ip.v4,
			        pxe->mode->dhcp_ack.dhcpv4.bootp_si_addr,
			        sizeof(pxeInfo->server_ip.v4)
			    );
		    }
	    } else {
		    pxeInfo->server_ip.addr[0] = convertIp(serverStr);
	    }

	    if (!overrideGateway) {
		    if (!pxeInfo->gateway_ip.addr[0]) {
			    size_t offset = 0;
			    auto packet = &pxe->mode->dhcp_ack.dhcpv4;

			    while (packet->dhcp_options[offset] != 0xff) {
				    auto code = packet->dhcp_options[offset];
				    auto len = packet->dhcp_options[offset + 1];

				    if (code == 3) {
					    pxeInfo->gateway_ip = {};
					    memcpy(
					        &pxeInfo->gateway_ip.v4,
					        &packet->dhcp_options[offset + 2],
					        sizeof(pxeInfo->gateway_ip.v4)
					    );
					    break;
				    }

				    offset += 2 + len;
			    }
		    }

		    if (!pxeInfo->gateway_ip.addr[0]) {
			    pxeInfo->gateway_ip = {};
			    memcpy(
			        &pxeInfo->gateway_ip.v4,
			        pxe->mode->dhcp_ack.dhcpv4.bootp_gi_addr,
			        sizeof(pxeInfo->gateway_ip.v4)
			    );
		    }
	    } else {
		    pxeInfo->gateway_ip.addr[0] = convertIp(gatewayStr);
	    }

	    // TODO: fall back to using DHCP option 54 or DHCP next-server
	    if (!pxeInfo->server_ip.addr[0]) {
		    eir::infoLogger() << "eir: failed to determine PXE server address" << frg::endlog;
		    return;
	    }

	    size_t file_size = 0;
	    char *path = nullptr;
	    EFI_CHECK(bs->allocate_pool(
	        EfiLoaderData, initrdPath.size() + 1, reinterpret_cast<void **>(&path)
	    ));
	    memcpy(path, initrdPath.data(), initrdPath.size());
	    path[initrdPath.size()] = '\0';

	    // normalize slashes in paths
	    for (size_t i = 0; i < strlen(path); i++) {
		    if (path[i] == '\\')
			    path[i] = '/';
	    }

	    EFI_CHECK(pxe->mtftp(
	        pxe,
	        EfiPxeBaseCodeTftpGetFileSize,
	        nullptr,
	        false,
	        &file_size,
	        nullptr,
	        &pxeInfo->server_ip,
	        path,
	        nullptr,
	        false
	    ));

	    initrdSize = file_size;
	    efi_physical_addr initrd_addr = 0;
	    EFI_CHECK(
	        bs->allocate_pages(AllocateAnyPages, EfiLoaderData, (file_size >> 12) + 1, &initrd_addr)
	    );

	    file_size = ((file_size >> 12) + 1) << 12;

	    EFI_CHECK(pxe->mtftp(
	        pxe,
	        EfiPxeBaseCodeTftpReadFile,
	        (void *)initrd_addr,
	        false,
	        &file_size,
	        nullptr,
	        &pxeInfo->server_ip,
	        path,
	        nullptr,
	        false
	    ));

	    bs->free_pool(path);

	    initrd = reinterpret_cast<void *>(initrd_addr);
    }
};

#if defined(__riscv)
size_t boot_hart = 0;

initgraph::Task findRiscVBootHart{
    &globalInitEngine,
    "uefi.find-riscv-boot-hart",
    initgraph::Entails{getBootservicesDoneStage()},
    [] {
	    riscv_efi_boot_protocol *boot_table = nullptr;
	    efi_guid riscv_boot_guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
	    auto status = bs->locate_protocol(&riscv_boot_guid, nullptr, (void **)&boot_table);
	    assert(status == EFI_SUCCESS);
	    assert(boot_table);

	    status = boot_table->get_boot_hartid(boot_table, &boot_hart);
	    assert(status == EFI_SUCCESS);

	    eir::infoLogger() << "eir: boot HART ID " << boot_hart << frg::endlog;
    }
};

initgraph::Task setupBootHartId{
    &globalInitEngine,
    "uefi.setup-riscv-boot-hard-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] { info_ptr->hartId = boot_hart; }
};
#endif

initgraph::Task readInitrd{
    &globalInitEngine,
    "uefi.read-initrd",
    initgraph::Requires{&preparePxe},
    initgraph::Entails{getBootservicesDoneStage()},
    [] {
	    if (initrd)
		    return;

	    efi_file_protocol *initrdFile = nullptr;
	    EFI_CHECK(fsOpen(&initrdFile, asciiToUcs2(initrdPath)));
	    initrdSize = fsGetSize(initrdFile);

	    // Read initrd.
	    efi_physical_addr initrd_addr = 0;
	    EFI_CHECK(bs->allocate_pages(
	        AllocateAnyPages, EfiLoaderData, (initrdSize >> 12) + 1, &initrd_addr
	    ));
	    EFI_CHECK(fsRead(initrdFile, initrdSize, 0, initrd_addr));

	    initrd = reinterpret_cast<void *>(initrd_addr);
    }
};

initgraph::Task setupGop{
    &globalInitEngine, "uefi.setup-gop", initgraph::Entails{getBootservicesDoneStage()}, [] {
	    // Get the frame buffer.
	    efi_guid gop_protocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	    efi_status status =
	        bs->locate_protocol(&gop_protocol, nullptr, reinterpret_cast<void **>(&gop));
	    // Clear the global gop pointer unless the whole task succeeds.
	    frg::scope_exit clearOnFailure{[&] { gop = nullptr; }};
	    if (status != EFI_SUCCESS) {
		    infoLogger() << "eir: Failed to locate GOP, status: " << status << frg::endlog;
		    return;
	    }

	    infoLogger() << "eir: framebuffer " << gop->mode->info->horizontal_resolution << "x"
	                 << gop->mode->info->vertical_resolution << " address=0x"
	                 << frg::hex_fmt{gop->mode->framebuffer_base} << frg::endlog;

	    switch (gop->mode->info->pixel_format) {
		    case PixelBlueGreenRedReserved8BitPerColor:
			    gopBpp = 32;
			    break;
		    case PixelRedGreenBlueReserved8BitPerColor:
			    gopBpp = 32;
			    break;
		    case PixelBitMask: {
			    assert(gop->mode->info->pixel_information.red_mask);
			    assert(gop->mode->info->pixel_information.green_mask);
			    assert(gop->mode->info->pixel_information.blue_mask);

			    size_t hbred = (sizeof(uint32_t) * 8)
			                   - __builtin_clz(gop->mode->info->pixel_information.red_mask);
			    size_t hbgreen = (sizeof(uint32_t) * 8)
			                     - __builtin_clz(gop->mode->info->pixel_information.green_mask);
			    size_t hbblue = (sizeof(uint32_t) * 8)
			                    - __builtin_clz(gop->mode->info->pixel_information.blue_mask);

			    frg::optional<size_t> hbres = {};
			    if (gop->mode->info->pixel_information.reserved_mask)
				    hbres = (sizeof(uint32_t) * 8)
				            - __builtin_clz(gop->mode->info->pixel_information.reserved_mask);

			    size_t highest_bit = frg::max(hbred, hbgreen);
			    highest_bit = frg::max(highest_bit, hbblue);
			    if (hbres)
				    highest_bit = frg::max(highest_bit, hbres.value());

			    assert(highest_bit % 8 == 0);
			    gopBpp = highest_bit;
			    break;
		    }
		    default:
			    infoLogger() << "eir: unhandled GOP pixel format" << frg::endlog;
			    return;
	    }

	    clearOnFailure.release();

	    initFramebuffer(
	        EirFramebuffer{
	            .fbAddress = gop->mode->framebuffer_base,
	            .fbPitch = gop->mode->info->pixels_per_scan_line * (gopBpp / 8),
	            .fbWidth = gop->mode->info->horizontal_resolution,
	            .fbHeight = gop->mode->info->vertical_resolution,
	            .fbBpp = gopBpp,
	        }
	    );

	    disableLogHandler(&conOutLogHandler);
    }
};

initgraph::Task exitBootServices{
    &globalInitEngine,
    "uefi.exit-boot-services",
    initgraph::Requires{getBootservicesDoneStage()},
    initgraph::Entails{getReservedRegionsKnownStage()},
    [] {
	    disableLogHandler(&conOutLogHandler);

	    memMapSize = sizeof(efi_memory_descriptor);
	    efi_memory_descriptor dummy;

	    // First get the size of the memory map buffer to allocate.
	    efi_status status =
	        bs->get_memory_map(&memMapSize, &dummy, &mapKey, &descriptorSize, &descriptorVersion);
	    assert(status == EFI_BUFFER_TOO_SMALL);

	    // the number of descriptors we overallocate the buffer by; get doubled every iteration
	    size_t overallocation = 8;

	    while (status != EFI_SUCCESS) {
		    // needing more than that would be quite unreasonable
		    assert(overallocation <= 0x800);

		    // over-allocate a bit to accomodate the allocation we have to make here
		    // we only get one shot(tm) to allocate an appropriately-sized buffer, as the spec does
		    // not allow for calling any boot services other than GetMemoryMap and ExitBootServices
		    // after a call to ExitBootServices fails
		    memMapSize += overallocation * descriptorSize;
		    EFI_CHECK(bs->allocate_pool(EfiLoaderData, memMapSize, &memMap));
		    overallocation *= 2;

		    // Now, get the actual memory map.
		    EFI_CHECK(bs->get_memory_map(
		        &memMapSize,
		        reinterpret_cast<efi_memory_descriptor *>(memMap),
		        &mapKey,
		        &descriptorSize,
		        &descriptorVersion
		    ));

		    // Exit boot services.
		    status = bs->exit_boot_services(handle, mapKey);
	    }

	    bs = nullptr;

#if defined(__x86_64__)
	    asm volatile("cli");
#elif defined(__aarch64__)
	    asm volatile("msr daifset, #15");
#elif defined(__riscv)
	    asm volatile("csrci sstatus, 0x2" ::: "memory");
#else
#error "Unsupported architecture!"
#endif
    }
};

initgraph::Task setupMemoryMap{
    &globalInitEngine,
    "uefi.setup-memory-map",
    initgraph::Requires{&exitBootServices},
    initgraph::Entails{getReservedRegionsKnownStage()},
    [] {
	    reservedRegions[nReservedRegions++] = {
	        reinterpret_cast<uintptr_t>(loadedImage->image_base), loadedImage->image_size
	    };
	    reservedRegions[nReservedRegions++] = {reinterpret_cast<uintptr_t>(initrd), initrdSize};

	    auto entries = memMapSize / descriptorSize;

	    auto endAddr = [](const efi_memory_descriptor *e) -> efi_physical_addr {
		    return e->physical_start + (e->number_of_pages * eir::pageSize);
	    };

	    auto nextEntry = [&](efi_physical_addr addr) {
		    const efi_memory_descriptor *lowest = nullptr;

		    for (size_t i = 0; i < entries; i++) {
			    auto e = reinterpret_cast<const efi_memory_descriptor *>(
			        uintptr_t(memMap) + (i * descriptorSize)
			    );
			    if (e->physical_start >= addr) {
				    if (!lowest || e->physical_start < lowest->physical_start) {
					    lowest = e;
				    }
			    }
		    }

		    return lowest;
	    };

	    auto isContiguous = [](const efi_memory_descriptor *a, const efi_memory_descriptor *b) {
		    if (a->physical_start + (a->number_of_pages * eir::pageSize) == b->physical_start)
			    return true;
		    return false;
	    };

	    auto isUsable = [](const efi_memory_descriptor *e) {
		    switch (e->type) {
			    case EfiConventionalMemory:
			    case EfiBootServicesCode:
			    case EfiBootServicesData:
				    return true;
			    default:
				    return false;
		    }
	    };

	    eir::infoLogger() << "Memory map:" << frg::endlog;
	    auto entry = nextEntry(0);

	    while (entry) {
		    auto lastContiguousEntry = entry;

		    while (true) {
			    auto next = nextEntry(endAddr(lastContiguousEntry));

			    if (!next || !isContiguous(lastContiguousEntry, next))
				    break;

			    if (isUsable(lastContiguousEntry) != isUsable(next))
				    break;

			    lastContiguousEntry = next;
		    }

		    eir::infoLogger() << "\tbase=0x" << frg::hex_fmt{entry->physical_start} << " length=0x"
		                      << frg::hex_fmt{endAddr(lastContiguousEntry) - entry->physical_start}
		                      << " usable=" << (isUsable(entry) ? "true" : "false") << frg::endlog;

		    if (isUsable(entry)) {
			    createInitialRegions(
			        {entry->physical_start, endAddr(lastContiguousEntry) - entry->physical_start},
			        {reservedRegions.data(), nReservedRegions}
			    );
		    }

		    entry = nextEntry(endAddr(lastContiguousEntry));
	    }
    }
};

initgraph::Task setupInitrdInfo{
    &globalInitEngine,
    "uefi.setup-initrd-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    EirAllocator alloc{};

	    frg::string<EirAllocator> cmdline_extras{alloc, cmdline};

	    auto format_ip = [&](efi_ip_address *addr) {
		    auto ip = frg::to_allocated_string(alloc, addr->v4.addr[0]);
		    ip.push_back('.');
		    ip += frg::to_allocated_string(alloc, addr->v4.addr[1]);
		    ip.push_back('.');
		    ip += frg::to_allocated_string(alloc, addr->v4.addr[2]);
		    ip.push_back('.');
		    ip += frg::to_allocated_string(alloc, addr->v4.addr[3]);
		    return std::move(ip);
	    };

	    if (pxeInfo) {
		    if (!overrideServer) {
			    cmdline_extras += " netserver.server=";
			    cmdline_extras += format_ip(&pxeInfo->server_ip);
		    }

		    if (!overrideGateway) {
			    cmdline_extras += " netserver.gateway=";
			    cmdline_extras += format_ip(&pxeInfo->gateway_ip);
		    }

		    if (!overrideStation) {
			    cmdline_extras += " netserver.ip=";
			    cmdline_extras += format_ip(&pxeInfo->station_ip);
		    }

		    if (!overrideSubnet) {
			    cmdline_extras += " netserver.subnet=";
			    cmdline_extras += format_ip(&pxeInfo->subnet_mask);
		    }

		    if (pxeInfo->device_path.size()) {
			    cmdline_extras += " netserver.device=";
			    cmdline_extras += pxeInfo->device_path;
		    }
	    }

	    cmdline = cmdline_extras;

	    auto cmd_length = cmdline.size();
	    assert(cmd_length <= pageSize);
	    auto cmd_buffer = bootAlloc<char>(cmd_length);
	    memcpy(cmd_buffer, cmdline.data(), cmd_length + 1);
	    info_ptr->commandLine = mapBootstrapData(cmd_buffer);
    }
};

initgraph::Task mapEirImage{
    &globalInitEngine,
    "uefi.map-eir-image",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    auto base = reinterpret_cast<uintptr_t>(loadedImage->image_base);
	    auto pages = (loadedImage->image_size >> 12) + 1;

	    for (size_t i = 0; i < pages; i++) {
		    mapSingle4kPage(
		        base + (i << 12), base + (i << 12), PageFlags::write | PageFlags::execute
		    );
	    }
    }
};

} // namespace

extern "C" efi_status eirUefiMain(const efi_handle h, const efi_system_table *system_table) {
	initPlatform();

	eirRunConstructors();

	// Set the system table so we can use loggers early on.
	st = system_table;
	bs = st->boot_services;
	handle = h;

	if (useConOut)
		enableLogHandler(&conOutLogHandler);

	// Reset the watchdog timer.
	EFI_CHECK(bs->set_watchdog_timer(0, 0, 0, nullptr));
	EFI_CHECK(st->con_out->clear_screen(st->con_out));

	// Get a handle to this binary in order to get the command line.
	efi_guid protocol = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_CHECK(bs->handle_protocol(handle, &protocol, reinterpret_cast<void **>(&loadedImage)));

	// Convert the command line to ASCII.
	char *ascii_cmdline = nullptr;
	{
		EFI_CHECK(bs->allocate_pool(
		    EfiLoaderData,
		    (loadedImage->load_options_size / sizeof(uint16_t)) + 1,
		    reinterpret_cast<void **>(&ascii_cmdline)
		));
		size_t i = 0;
		for (; i < loadedImage->load_options_size / sizeof(char16_t); i++) {
			auto c = reinterpret_cast<char16_t *>(loadedImage->load_options)[i];
			// we only use printable ASCII characters, everything else gets discarded
			if (c >= 0x20 && c <= 0x7E) {
				ascii_cmdline[i] = c;
			} else {
				ascii_cmdline[i] = '\0';
			}
		}
		// Null-terminate the buffer.
		ascii_cmdline[i] = '\0';
	}
	assert(ascii_cmdline);
	cmdline = {ascii_cmdline, strlen(ascii_cmdline)};

	bool eir_gdb_ready_val = true;

	frg::array args = {
	    // allow for attaching GDB to eir
	    frg::option{"eir.efidebug", frg::store_false(eir_gdb_ready_val)},
	    frg::option{"bochs", frg::store_true(log_e9)},
	    frg::option{"eir.initrd", frg::as_string_view(initrdPath)},
	    frg::option{"netserver.gateway", frg::as_string_view(gatewayStr)},
	    frg::option{"netserver.ip", frg::as_string_view(stationStr)},
	    frg::option{"netserver.subnet", frg::as_string_view(subnetStr)},
	    frg::option{"netserver.server", frg::as_string_view(serverStr)},
	};
	frg::parse_arguments(ascii_cmdline, args);

	overrideGateway = gatewayStr.size() != 0;
	overrideStation = stationStr.size() != 0;
	overrideSubnet = subnetStr.size() != 0;
	overrideServer = serverStr.size() != 0;

	eir::infoLogger() << "eir: image base address " << frg::hex_fmt{loadedImage->image_base}
	                  << frg::endlog;
	eir::infoLogger() << "eir: command line='" << ascii_cmdline << "'" << frg::endlog;

	// this needs to be volatile as GDB sets this to true on attach
	volatile bool eir_gdb_ready = eir_gdb_ready_val;

	if (!eir_gdb_ready_val) {
#ifdef __x86_64__
		// exfiltrate our base address for use with gdb
		constexpr arch::scalar_register<uint8_t> offset{0};
		auto port = arch::global_io.subspace(0xCB7);

		for (size_t i = 0; i < sizeof(uintptr_t); i++) {
			uint8_t b = reinterpret_cast<uintptr_t>(loadedImage->image_base) >> (i * 8);
			port.store(offset, b);
		}
#endif

		eir::infoLogger() << "eir: Waiting for GDB to attach" << frg::endlog;
	}

	while (!eir_gdb_ready)
		;

	// Enter a stack that is part of Eir's image.
	// This ensures that we can still access the stack when paging in enabled.
	runOnStack([] { eirMain(); }, eirStackTop);

	return EFI_SUCCESS;
}

} // namespace eir
