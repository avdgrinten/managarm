#include <expected>

#ifdef __x86_64__
#include <arch/io_space.hpp>
#endif
#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/arch-generic/stack.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/dtb/dtb.hpp>
#include <eir-internal/error.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/array.hpp>
#include <frg/scope_exit.hpp>
#include <stdbool.h>

#include "efi.hpp"
#include "helpers.hpp"
#include "pe.hpp"

static_assert(
    sizeof(char16_t) == sizeof(wchar_t),
    "Strings are not UTF-16-ish, are you missing -fshort-wchar?"
);

extern "C" int _purecall() {
	eir::panicLogger() << "eir: Pure virtual function called" << frg::endlog;
	__builtin_trap();
}

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
efi_device_path_to_text_protocol *devPathToText = nullptr;

constexpr frg::string_view defaultInitrdPath = "managarm\\initrd.cpio";
constexpr frg::string_view defaultZstdInitrdPath = "managarm\\initrd.cpio.zst";

frg::string_view initrdPath = defaultInitrdPath;

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

frg::string<UefiAllocator> devPathToString(efi_device_path_protocol *devPath) {
	auto devPathUtf16 = devPathToText->convert_device_path_to_text(devPath, true, true);
	assert(devPathUtf16);
	frg::scope_exit freeDevPathUtf16{[&] { EFI_CHECK(bs->free_pool(devPathUtf16)); }};

	auto n = frg::basic_string_view<char16_t>{devPathUtf16}.size();
	return ucs2ToAscii(devPathUtf16, n);
}

std::expected<frg::string<UefiAllocator>, Error>
devPathOfHandle(efi_handle handle, efi_guid devPathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID) {
	efi_device_path_protocol *devPath = nullptr;
	TRY_EFI(bs->handle_protocol(handle, &devPathGuid, reinterpret_cast<void **>(&devPath)));

	return devPathToString(devPath);
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
        getBootservicesDoneStage(),
        getKernelLoadableStage(),
        acpi::getRsdpAvailableStage(),
        getDtbAvailableStage()
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

// Loads a file via TFTP into freshly allocated pages.
// Returns the status of the file size query so that callers can fall back to other files.
efi_status pxeLoadFile(
    efi_pxe_base_code_protocol *pxe, frg::string_view filePath, void *&fileData, size_t &fileSize
) {
	char *path = nullptr;
	EFI_CHECK(
	    bs->allocate_pool(EfiLoaderData, filePath.size() + 1, reinterpret_cast<void **>(&path))
	);
	frg::scope_exit freePath{[&] { EFI_CHECK(bs->free_pool(path)); }};
	memcpy(path, filePath.data(), filePath.size());
	path[filePath.size()] = '\0';

	// normalize slashes in paths
	for (size_t i = 0; i < filePath.size(); i++) {
		if (path[i] == '\\')
			path[i] = '/';
	}

	uint64_t size = 0;
	auto status = pxe->mtftp(
	    pxe,
	    EfiPxeBaseCodeTftpGetFileSize,
	    nullptr,
	    false,
	    &size,
	    nullptr,
	    &pxeInfo->server_ip,
	    path,
	    nullptr,
	    false
	);
	if (status != EFI_SUCCESS)
		return status;

	size_t pages = (size >> 12) + 1;
	efi_physical_addr addr = 0;
	EFI_CHECK(bs->allocate_pages(AllocateAnyPages, EfiLoaderData, pages, &addr));

	uint64_t bufferSize = static_cast<uint64_t>(pages) << 12;
	EFI_CHECK(pxe->mtftp(
	    pxe,
	    EfiPxeBaseCodeTftpReadFile,
	    reinterpret_cast<void *>(addr),
	    false,
	    &bufferSize,
	    nullptr,
	    &pxeInfo->server_ip,
	    path,
	    nullptr,
	    false
	));

	fileData = reinterpret_cast<void *>(addr);
	fileSize = size;
	return EFI_SUCCESS;
}

initgraph::Task preparePxe{
    &globalInitEngine,
    "uefi.pxe-setup",
    initgraph::Entails{getBootservicesDoneStage(), getInitrdAvailableStage()},
    [] {
	    efi_guid pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
	    efi_pxe_base_code_protocol *pxe = nullptr;

	    auto status = bs->handle_protocol(
	        loadedImage->device_handle, &pxe_guid, reinterpret_cast<void **>(&pxe)
	    );
	    if (status != EFI_SUCCESS)
		    return;

	    EFI_CHECK(
	        bs->allocate_pool(EfiLoaderData, sizeof(pxe_info), reinterpret_cast<void **>(&pxeInfo))
	    );
	    new (pxeInfo) pxe_info{};

	    if (devPathToText) {
		    efi_guid devpath_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
		    efi_device_path_protocol *devpath = nullptr;
		    EFI_CHECK(bs->handle_protocol(
		        loadedImage->device_handle, &devpath_guid, reinterpret_cast<void **>(&devpath)
		    ));

		    auto devpathstr = devPathToText->convert_device_path_to_text(devpath, true, true);
		    assert(devpathstr);
		    frg::scope_exit freeDevPathStr{[&] { EFI_CHECK(bs->free_pool(devpathstr)); }};

		    auto n = frg::basic_string_view<char16_t>{devpathstr}.size();
		    auto converted = ucs2ToAscii(devpathstr, n);

		    char *devpathascii = nullptr;
		    EFI_CHECK(
		        bs->allocate_pool(EfiLoaderData, n + 1, reinterpret_cast<void **>(&devpathascii))
		    );
		    memcpy(devpathascii, converted.data(), n);
		    devpathascii[n] = '\0';

		    pxeInfo->device_path = frg::string_view{devpathascii};
	    }

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

	    // Prefer the zstd-compressed initrd if no explicit path was given
	    if (initrdPath == defaultInitrdPath) {
		    if (pxeLoadFile(pxe, defaultZstdInitrdPath, rawInitrd, rawInitrdSize) == EFI_SUCCESS)
			    return;

		    eir::infoLogger() << "eir: Failed to load " << defaultZstdInitrdPath
		                      << " via TFTP, falling back to " << initrdPath << frg::endlog;
	    }

	    EFI_CHECK(pxeLoadFile(pxe, initrdPath, rawInitrd, rawInitrdSize));
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
    initgraph::Entails{getKernelLoadableStage()},
    [] { eirBootHartId = boot_hart; }
};
#endif

initgraph::Task readInitrd{
    &globalInitEngine,
    "uefi.read-initrd",
    initgraph::Requires{&preparePxe},
    initgraph::Entails{getBootservicesDoneStage(), getInitrdDecompressableStage()},
    [] {
	    if (rawInitrd)
		    return;

	    infoLogger() << "eir: Trying to read initrd from simple filesystem" << frg::endlog;

	    efi_file_protocol *initrdFile = nullptr;
	    EFI_CHECK(fsOpen(&initrdFile, asciiToUcs2(initrdPath)));
	    rawInitrdSize = fsGetSize(initrdFile);

	    // Read initrd.
	    efi_physical_addr initrd_addr = 0;
	    EFI_CHECK(bs->allocate_pages(
	        AllocateAnyPages, EfiLoaderData, (rawInitrdSize >> 12) + 1, &initrd_addr
	    ));
	    EFI_CHECK(fsRead(initrdFile, rawInitrdSize, 0, initrd_addr));

	    rawInitrd = reinterpret_cast<void *>(initrd_addr);
    }
};

initgraph::Task setupGop{
    &globalInitEngine,
    "uefi.setup-gop",
    initgraph::Entails{getBootservicesDoneStage(), getFramebufferAvailableStage()},
    [] {
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
    initgraph::Entails{getFirmwareDoneStage(), getReservedRegionsKnownStage()},
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
		    status = bs->get_memory_map(
		        &memMapSize,
		        reinterpret_cast<efi_memory_descriptor *>(memMap),
		        &mapKey,
		        &descriptorSize,
		        &descriptorVersion
		    );
		    if (status == EFI_BUFFER_TOO_SMALL)
			    continue;

		    EFI_CHECK(status);

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

initgraph::Task setupReservedRegions{
    &globalInitEngine,
    "uefi.setup-reserved-regions",
    initgraph::Requires{&exitBootServices},
    initgraph::Entails{getReservedRegionsKnownStage()},
    [] {
	    reservedRegions[nReservedRegions++] = {
	        reinterpret_cast<uintptr_t>(loadedImage->image_base), loadedImage->image_size
	    };
	    reservedRegions[nReservedRegions++] = {
	        reinterpret_cast<uintptr_t>(rawInitrd), rawInitrdSize
	    };

	    if (eirDtbPtr) {
		    DeviceTree dt{physToVirt<void>(eirDtbPtr)};

		    reservedRegions[nReservedRegions++] = {eirDtbPtr, dt.size()};
	    }
    }
};

initgraph::Task setupMemoryMap{
    &globalInitEngine,
    "uefi.setup-memory-map",
    initgraph::Requires{&exitBootServices, getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
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
				    // TODO: It would be nice to make EfiBootServicesData as usable;
				    //       however, we cannot do that right now since it also contains
				    //       the UEFI page tables (that we still use for some time after boot).
				    return true;
			    default:
				    return false;
		    }
	    };

	    for (size_t i = 0; i < entries; i++) {
		    auto e = reinterpret_cast<const efi_memory_descriptor *>(
		        uintptr_t(memMap) + (i * descriptorSize)
		    );

		    EirMemoryType type;
		    switch (e->type) {
			    case EfiConventionalMemory:
			    case EfiLoaderCode:
			    case EfiLoaderData:
			    case EfiBootServicesCode:
			    case EfiBootServicesData:
				    // Eir is responsible for dealing with bootloader reclaimable memory.
				    // Hence, we map it to usableRam.
				    type = EirMemoryType::usableRam;
				    break;
			    case EfiRuntimeServicesCode:
			    case EfiRuntimeServicesData:
				    type = EirMemoryType::runtimeServices;
				    break;
			    case EfiACPIReclaimMemory:
				    type = EirMemoryType::acpiReclaimable;
				    break;
			    case EfiACPIMemoryNVS:
				    type = EirMemoryType::acpiNvs;
				    break;
			    case EfiMemoryMappedIO:
			    case EfiMemoryMappedIOPortSpace:
				    type = EirMemoryType::mmio;
				    break;
			    case EfiReservedMemoryType:
			    case EfiUnusableMemory:
			    case EfiPalCode:
			    case EfiPersistentMemory:
			    case EfiUnacceptedMemoryType:
				    type = EirMemoryType::reserved;
				    break;
			    default:
				    eir::infoLogger() << "eir: Unknown EFI memory type " << e->type << frg::endlog;
				    type = EirMemoryType::reserved;
		    }

		    uint32_t attrs = 0;
		    if (e->attribute & EFI_MEMORY_UC)
			    attrs |= eir_memory_attrs::uc;
		    if (e->attribute & EFI_MEMORY_WC)
			    attrs |= eir_memory_attrs::wc;
		    if (e->attribute & EFI_MEMORY_WT)
			    attrs |= eir_memory_attrs::wt;
		    if (e->attribute & EFI_MEMORY_WB)
			    attrs |= eir_memory_attrs::wb;

		    reportFirmwareMemory(
		        e->physical_start, e->number_of_pages * eir::pageSize, type, attrs
		    );
	    }

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

initgraph::Task passPxeCmdline{
    &globalInitEngine,
    "uefi.pass-pxe-cmdline",
    initgraph::Requires{&preparePxe, getAllocationAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    EirAllocator alloc{};

	    frg::string<EirAllocator> cmdline_extras{alloc};

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

	    auto cmd_length = cmdline_extras.size();
	    assert(cmd_length <= pageSize);
	    auto cmd_buffer = bootAlloc<char>(cmd_length);
	    memcpy(cmd_buffer, cmdline_extras.data(), cmd_length + 1);
	    extendCmdline(cmd_buffer);
    }
};

void handleUki() {
	auto base = reinterpret_cast<const char *>(loadedImage->image_base);

	// From the DOS header we only check the magic and the PE signature offset at 0x3c.
	uint16_t dosMagic;
	uint32_t ntOffset;
	memcpy(&dosMagic, base, sizeof(uint16_t));
	if (dosMagic != 0x5a4d) {
		infoLogger() << "eir: Bad DOS stub magic of PE file" << frg::endlog;
		return;
	}
	memcpy(&ntOffset, base + 0x3c, sizeof(uint32_t));

	// The PE signature is referenced by the DOS header.
	uint32_t peSignature;
	memcpy(&peSignature, base + ntOffset, sizeof(uint32_t));
	if (peSignature != 0x4550) {
		infoLogger() << "eir: Bad PE signature" << frg::endlog;
		return;
	}

	// The file header follows the PE signature.
	auto fileHeaderOffset = ntOffset + sizeof(uint32_t);
	auto fileHeader = reinterpret_cast<const PeFileHeader *>(base + fileHeaderOffset);

	// The optional header follows the file header.
	auto optionalHeaderOffset = fileHeaderOffset + sizeof(PeFileHeader);

	// Section headers follow the optional header.
	auto sectionHeadersOffset = optionalHeaderOffset + fileHeader->SizeOfOptionalHeader;
	auto sectionHeaders = reinterpret_cast<const PeSectionHeader *>(base + sectionHeadersOffset);
	for (size_t i = 0; i < fileHeader->NumberOfSections; ++i) {
		auto sectionHeader = &sectionHeaders[i];
		auto name = frg::string_view{sectionHeader->Name, 8};
		if (auto n = name.find_first('\0'); n != static_cast<size_t>(-1))
			name = name.sub_string(0, n);

		if (name == ".cmdline") {
			infoLogger() << "eir: Found .cmdline in UKI at offset 0x"
			             << frg::hex_fmt{sectionHeader->VirtualAddress} << frg::endlog;
			extendCmdline(
			    frg::string_view{base + sectionHeader->VirtualAddress, sectionHeader->VirtualSize}
			);
		} else if (name == ".initrd") {
			infoLogger() << "eir: Found .initrd in UKI at offset 0x"
			             << frg::hex_fmt{sectionHeader->VirtualAddress} << frg::endlog;
			rawInitrd = const_cast<char *>(base + sectionHeader->VirtualAddress);
			rawInitrdSize = sectionHeader->VirtualSize;
		}
	}
}

constinit BootCaps uefiCaps = {
    .hasMemoryMap = true,
};

} // namespace

const BootCaps &BootCaps::get() { return uefiCaps; };

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

	// Get some essential protocols.
	efi_guid devPathToTextGuid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
	if (bs->locate_protocol(&devPathToTextGuid, nullptr, reinterpret_cast<void **>(&devPathToText))
	    != EFI_SUCCESS)
		devPathToText = nullptr;

	// Get a handle to this binary in order to get the command line.
	efi_guid loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_CHECK(
	    bs->handle_protocol(handle, &loadedImageGuid, reinterpret_cast<void **>(&loadedImage))
	);
	uefiCaps.imageStart = reinterpret_cast<uintptr_t>(loadedImage->image_base);
	uefiCaps.imageEnd =
	    reinterpret_cast<uintptr_t>(loadedImage->image_base) + loadedImage->image_size;

	// Print device paths of the loaded image for debugging purposes.
	if (devPathToText) {
		auto loadedImageDevPath =
		    devPathOfHandle(handle, EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID);
		auto loadDeviceDevPath = devPathOfHandle(loadedImage->device_handle);
		if (loadedImageDevPath)
			infoLogger() << "eir: Device path of loaded image: " << *loadedImageDevPath
			             << frg::endlog;
		// Note that the device path from EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID can differ
		// from the EFI_DEVICE_PATH_PROTOCOL_GUID of loadedImage->device_handle.
		if (loadDeviceDevPath)
			infoLogger() << "eir: Device path of load device: " << *loadDeviceDevPath
			             << frg::endlog;
		infoLogger() << "eir: File path of loaded image: "
		             << devPathToString(loadedImage->file_path) << frg::endlog;
	}

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
	extendCmdline(ascii_cmdline);

	bool eir_gdb_ready_val = true;

	frg::array args = {
	    // allow for attaching GDB to eir
	    frg::option{"eir.efidebug", frg::store_false(eir_gdb_ready_val)},
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

	handleUki();

	// Enter a stack that is part of Eir's image.
	// This ensures that we can still access the stack when paging in enabled.
	runOnStack([] { eirMain(); }, eirStackTop);

	return EFI_SUCCESS;
}

} // namespace eir
