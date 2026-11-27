#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <initgraph.hpp>

namespace eir {

frg::array<eir::InitialRegion, 32> reservedRegions;
size_t nReservedRegions = 0;

void GlobalInitEngine::preActivate(initgraph::Node *node) {
	if (node->type() == initgraph::NodeType::task)
		infoLogger() << "eir: Running task " << node->displayName() << frg::endlog;
}

void GlobalInitEngine::postActivate(initgraph::Node *node) {
	if (node->type() == initgraph::NodeType::stage)
		infoLogger() << "eir: Reached stage " << node->displayName() << frg::endlog;
}

void GlobalInitEngine::onUnreached() {
	infoLogger() << "eir: initgraph has cycles" << frg::endlog;

	while (1) {
	}
}

constinit GlobalInitEngine globalInitEngine;

initgraph::Stage *getReservedRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.reserved-regions-known"};
	return &s;
}

initgraph::Stage *getMemoryRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-regions-known"};
	return &s;
}

initgraph::Stage *getInitrdDecompressableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.initrd-available"};
	return &s;
}

initgraph::Stage *getInitrdAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.initrd-available"};
	return &s;
}

initgraph::Stage *getCmdlineAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.cmdline-available"};
	return &s;
}

initgraph::Stage *getFirmwareDoneStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.firmware-done"};
	return &s;
}

initgraph::Stage *getKernelMappableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-mappable"};
	return &s;
}

initgraph::Stage *getKernelLoadableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-loadable"};
	return &s;
}

initgraph::Stage *getAllocationAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.allocation-available"};
	return &s;
}

struct GlobalCtorTest {
	GlobalCtorTest() { infoLogger() << "Hello world from global ctor" << frg::endlog; }
};

// Runs between the memory map being known and setting up the allocator so that the
// decompressed initrd can be carved out of the allocatable regions.
static initgraph::Task decompressInitrdTask{
    &globalInitEngine,
    "generic.decompress-initrd",
    initgraph::Requires{getInitrdDecompressableStage(), getMemoryRegionsKnownStage()},
    initgraph::Entails{getInitrdAvailableStage()},
    [] { decompressInitrd(); }
};

static initgraph::Task parseInitrdInfo{
    &globalInitEngine,
    "generic.parse-initrd",
    initgraph::Requires{&decompressInitrdTask},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    assert(initrd);
	    parseInitrd(initrd);
	    initrdNote.physicalBase = virtToPhys(initrd);
	    initrdNote.length = initrd_image.size();
    }
};

static initgraph::Task setupRegions{
    &globalInitEngine,
    "generic.setup-regions",
    // Decompressing the initrd might require allocating from memory regions
    initgraph::Requires{getMemoryRegionsKnownStage(), getInitrdAvailableStage()},
    initgraph::Entails{getAllocationAvailableStage()},
    [] {
	    setupRegionStructs();

	    eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
	    for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		    if (regions[i].regionType == RegionType::null)
			    continue;
		    eir::infoLogger() << "    Memory region [" << i << "]."
		                      << " Base: 0x" << frg::hex_fmt{regions[i].address} << ", length: 0x"
		                      << frg::hex_fmt{regions[i].size} << frg::endlog;
		    if (regions[i].regionType == RegionType::allocatable)
			    eir::infoLogger() << "        Buddy tree at 0x"
			                      << frg::hex_fmt{regions[i].buddyTree} << ", overhead: 0x"
			                      << frg::hex_fmt{regions[i].buddyOverhead} << frg::endlog;
	    }
    }
};

static initgraph::Task setupPageTables{
    &globalInitEngine,
    "generic.setup-page-tables",
    initgraph::Requires{getAllocationAvailableStage()},
    initgraph::Entails{getKernelMappableStage()},
    [] { initProcessorPaging(); }
};

static initgraph::Task mapRegions{
    &globalInitEngine,
    "generic.map-regions",
    initgraph::Requires{getKernelMappableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    mapRegionsAndStructs();
#ifdef KERNEL_LOG_ALLOCATIONS
	    allocLogRingBuffer();
#endif

	    int n = 0;
	    for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		    if (regions[i].regionType == RegionType::allocatable)
			    n++;
	    }
	    auto bootstrapData = BootstrapData::place(n * sizeof(EirRegion), alignof(EirRegion));
	    for (size_t i = 0; i < eirMaxMemoryRegions; ++i) {
		    if (regions[i].regionType != RegionType::allocatable)
			    continue;
		    EirRegion regionInfo{};
		    regionInfo.address = regions[i].address;
		    regionInfo.length = regions[i].size;
		    regionInfo.order = regions[i].order;
		    regionInfo.numRoots = regions[i].numRoots;
		    regionInfo.buddyTree = regions[i].buddyMap;
		    bootstrapData.write(regionInfo);
	    }
	    physicalMemoryNote.numRegions = n;
	    physicalMemoryNote.regionInfo = bootstrapData.kernelAddress();

	    serializeFirmwareMemoryMap();
    }
};

static initgraph::Task mapEirImage{
    &globalInitEngine, "generic.map-eir-image", initgraph::Requires{getKernelMappableStage()}, [] {
	    const auto &bootCaps = BootCaps::get();
	    auto floor = static_cast<address_t>(bootCaps.imageStart) & ~address_t{pageSize - 1};
	    auto ceiling =
	        (static_cast<address_t>(bootCaps.imageEnd) + pageSize - 1) & ~address_t{pageSize - 1};

	    for (address_t addr = floor; addr < ceiling; addr += pageSize) {
		    if (kernel_physical != SIZE_MAX) {
			    mapSinglePage(
			        addr, addr - floor + kernel_physical, PageFlags::write | PageFlags::execute
			    );
		    } else {
			    mapSinglePage(addr, addr, PageFlags::write | PageFlags::execute);
		    }
	    }
    }
};

static initgraph::Task loadKernelImageTask{
    &globalInitEngine,
    "generic.load-kernel-image",
    initgraph::Requires{getKernelMappableStage(), getKernelLoadableStage()},
    [] {
	    // Setup the kernel image.
	    loadKernelImage(reinterpret_cast<void *>(kernel_image.data()));
	    eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                      << " KiB after loading the kernel" << frg::endlog;
    }
};

static initgraph::Task prepareFramebufferForThor{
    &globalInitEngine,
    "generic.prepare-framebuffer-for-thor",
    initgraph::Requires{getKernelMappableStage(), getFramebufferAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    auto *fb = getFramebuffer();
	    if (fb) {
		    // Map the framebuffer.
		    assert(fb->fbAddress & ~static_cast<EirPtr>(pageSize - 1));
		    for (address_t pg = 0; pg < fb->fbPitch * fb->fbHeight; pg += pageSize)
			    mapSinglePage(
			        getKernelFrameBuffer() + pg,
			        fb->fbAddress + pg,
			        PageFlags::write,
			        CachingMode::writeCombine
			    );
		    mapKasanShadow(getKernelFrameBuffer(), fb->fbPitch * fb->fbHeight);
		    unpoisonKasanShadow(getKernelFrameBuffer(), fb->fbPitch * fb->fbHeight);

		    framebufferNote = *fb;
		    framebufferNote.fbEarlyWindow = getKernelFrameBuffer();
	    }
    }
};

GlobalCtorTest globalCtorTest;

extern "C" [[noreturn]] void eirMain() {
	infoLogger() << "Entering generic eir setup" << frg::endlog;

	globalInitEngine.run();

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eir::enterKernel();
}

} // namespace eir
