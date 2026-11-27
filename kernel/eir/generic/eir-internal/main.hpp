#pragma once

#include <eir-internal/generic.hpp>
#include <eir/interface.hpp>
#include <frg/string.hpp>
#include <initgraph.hpp>

namespace eir {

// These fields are set by the boot protocol code.
extern "C" physaddr_t eirDtbPtr;
extern physaddr_t eirRsdpAddr;
extern physaddr_t eirSmbios3Addr;

struct GlobalInitEngine final : initgraph::Engine {
	void preActivate(initgraph::Node *node) override;
	void postActivate(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;

extern "C" [[noreturn]] void eirMain();

extern "C" void eirRunConstructors();

initgraph::Stage *getInitrdDecompressableStage();
initgraph::Stage *getInitrdAvailableStage();
initgraph::Stage *getCmdlineAvailableStage();

// After this stage, the boot protocol does not call into firmware anymore.
// Boot protocols that never invoke firmware services reach this stage immediately.
initgraph::Stage *getFirmwareDoneStage();

// Before this stage, all reserved regions must be available.
initgraph::Stage *getReservedRegionsKnownStage();

// Before this stage, all memory regions must be available.
initgraph::Stage *getMemoryRegionsKnownStage();

// After this stage, physical memory can be allocated.
// Ordered after getReservedRegionsKnownStage() and getMemoryRegionsKnownStage().
initgraph::Stage *getAllocationAvailableStage();

// After this stage, memory can be mapped into Thor's address space.
// Ordered after getAllocationAvailableStage().
initgraph::Stage *getKernelMappableStage();

// Before this stage, everything needed to fill out ELF notes but be available.
// After this stage, the kernel image is loaded and the boot information is finalized.
// Ordered after getKernelMappableStage().
initgraph::Stage *getKernelLoadableStage();

extern void *rawInitrd;
// Size of the initrd as provided by the boot protocol, or zero if unknown.
extern size_t rawInitrdSize;

extern void *initrd;
// Size of the decompressed initrd.
extern size_t initrdSize;

extern frg::array<InitialRegion, 32> reservedRegions;
extern size_t nReservedRegions;

} // namespace eir
