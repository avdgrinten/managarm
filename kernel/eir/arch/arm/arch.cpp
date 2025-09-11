#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/memory-layout.hpp>

extern "C" void eirExcVectors();

extern "C" [[noreturn]] void eirEnterKernel(uint64_t entry, uint64_t stack);

namespace eir {

namespace {

void disableMmuEl1(uintptr_t flushStart, uintptr_t flushEnd, size_t dcLineSize) {
	uint64_t sctlr;
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr &= ~UINT64_C(1);

	asm volatile(
	    // clang-format off
		// Clear the dcache in range [x0, x1) to PoC.
		     "mov x0, %0" "\n"
		"1:" "\n"
		"\t" "dc cvac, x0" "\n"
		"\t" "add x0, x0, %2" "\n"
		"\t" "cmp x0, %1" "\n"
		"\t" "b.lo 1b" "\n"
		// Ensure that the cache flush is finished.
		"\t" "dsb ish" "\n"
		"\t" "isb" "\n"
		// Disable paging.
		"\t" "msr sctlr_el1, %3" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(flushStart), // %0
	      "r"(flushEnd),   // %1
	      "r"(dcLineSize), // %2
	      "r"(sctlr)       // %3
	    : "x0", "memory"
	);
}

void disableMmuEl2(uintptr_t flushStart, uintptr_t flushEnd, size_t dcLineSize) {
	uint64_t sctlr;
	asm volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
	sctlr &= ~UINT64_C(1);

	asm volatile(
	    // clang-format off
		// Clear the dcache in range [x0, x1) to PoC.
		     "mov x0, %0" "\n"
		"1:" "\n"
		"\t" "dc cvac, x0" "\n"
		"\t" "add x0, x0, %2" "\n"
		"\t" "cmp x0, %1" "\n"
		"\t" "b.lo 1b" "\n"
		// Ensure that the cache flush is finished.
		"\t" "dsb ish" "\n"
		"\t" "isb" "\n"
		// Disable paging.
		"\t" "msr sctlr_el2, %3" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(flushStart), // %0
	      "r"(flushEnd),   // %1
	      "r"(dcLineSize), // %2
	      "r"(sctlr)       // %3
	    : "x0", "memory"
	);
}

void enterE2h() {
	infoLogger() << "eir: Entering VHE mode" << frg::endlog;
	uint64_t hcr;
	asm volatile("mrs %0, hcr_el2" : "=r"(hcr) : : "memory");
	hcr |= UINT64_C(1) << 34;

	asm volatile(
	    // clang-format off
		// Set the E2H bit.
		     "msr hcr_el2, %0" "\n"
		"\t" "isb" "\n"
		// Flush the TLB since the E2H bit may be cached.
		"\t" "tlbi alle2" "\n"
		"\t" "tlbi alle1" "\n"
		"\t" "dsb ish" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(hcr | (UINT64_C(1) << 34))
	    : "memory"
	);
}

// Must only be called in either EL1 or in EL2 with E2H=1.
void enterKernelPaging() {
	uint64_t aa64mmfr0;
	asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(aa64mmfr0));

	// Setup system registers for paging (MAIR and TCR).
	if (aa64mmfr0 & (uint64_t(0xF) << 28))
		eir::panicLogger() << "PANIC! This CPU doesn't support 4K memory translation granules"
		                   << frg::endlog;

	auto pa = frg::min(uint64_t(5), aa64mmfr0 & 0xF);

	uint64_t mair = UINT64_C(0b11111111) |         // Normal, Write-back RW-Allocate non-transient
	                (UINT64_C(0b00001100) << 8) |  // Device, GRE
	                (UINT64_C(0b00000000) << 16) | // Device, nGnRnE
	                (UINT64_C(0b00000100) << 24) | // Device, nGnRE
	                (UINT64_C(0b01000100) << 32);  // Normal Non-cacheable

	uint64_t tcr = (16 << 0) |            // T0SZ=16
	               (16 << 16) |           // T1SZ=16
	               (1 << 8) |             // TTBR0 Inner WB RW-Allocate
	               (1 << 10) |            // TTBR0 Outer WB RW-Allocate
	               (1 << 24) |            // TTBR1 Inner WB RW-Allocate
	               (1 << 26) |            // TTBR1 Outer WB RW-Allocate
	               (2 << 12) |            // TTBR0 Inner shareable
	               (2 << 28) |            // TTBR1 Inner shareable
	               (uint64_t(pa) << 32) | // 48-bit intermediate address
	               (uint64_t(2) << 30);   // TTBR1 4K granule

	// TODO: If paging is already enabled, we should not overwrite MAIR and TCR
	//       with potentially conflicting values here.
	//       Instead, ensure that the current values are sane and error out if they are not.
	//       This does not apply if paging is off.
	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr mair_el1, %0" "\n"
		"\t" "msr tcr_el1, %1" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(mair), "r"(tcr)
	    : "memory"
	);

	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr ttbr0_el1, %0" "\n"
		"\t" "msr ttbr1_el1, %1" "\n"
		"\t" "isb" "\n"
		// Invalidate TLB to clear old mappings.
		"\t" "tlbi vmalle1" "\n"
		"\t" "dsb ish" "\n"
		"\t" "isb" "\n"
	    // clang-format on
	    :
	    : "r"(eirTTBR[0] + 1), "r"(eirTTBR[1] + 1)
	    : "memory"
	);

	// Enable the MMU.
	uint64_t sctlr;
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= UINT64_C(1);

	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr sctlr_el1, %0" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(sctlr)
	    : "memory"
	);
}

} // anonymous namespace

uintptr_t eirTTBR[2];

void setupPaging() {
	eirTTBR[0] = allocPage();
	eirTTBR[1] = allocPage();

	for (size_t i = 0; i < 512; i++) {
		((uint64_t *)eirTTBR[0])[i] = 0;
		((uint64_t *)eirTTBR[1])[i] = 0;
	}
}

static inline constexpr uint64_t kPageValid = 1;
static inline constexpr uint64_t kPageTable = (1 << 1);
static inline constexpr uint64_t kPageL3Page = (1 << 1);
static inline constexpr uint64_t kPageXN = (uint64_t(1) << 54);
static inline constexpr uint64_t kPagePXN = (uint64_t(1) << 53);
static inline constexpr uint64_t kPageNotGlobal = (1 << 11);
static inline constexpr uint64_t kPageAccess = (1 << 10);
static inline constexpr uint64_t kPageRO = (1 << 7);
static inline constexpr uint64_t kPageInnerSh = (3 << 8);
static inline constexpr uint64_t kPageWb = (0 << 2);
static inline constexpr uint64_t kPageGRE = (1 << 2);
static inline constexpr uint64_t kPagenGnRnE = (2 << 2);

void
mapSingle4kPage(address_t address, address_t physical, uint32_t flags, CachingMode caching_mode) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0_ent = ((uint64_t *)eirTTBR[ttbr])[l0];
	auto l1_ptr = l0_ent & 0xFFFFFFFFF000;
	if (!(l0_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for (int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)eirTTBR[ttbr])[l0] = addr | kPageValid | kPageTable;

		l1_ptr = addr;
	}

	auto l1_ent = ((uint64_t *)l1_ptr)[l1];
	auto l2_ptr = l1_ent & 0xFFFFFFFFF000;
	if (!(l1_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for (int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)l1_ptr)[l1] = addr | kPageValid | kPageTable;

		l2_ptr = addr;
	}

	auto l2_ent = ((uint64_t *)l2_ptr)[l2];
	auto l3_ptr = l2_ent & 0xFFFFFFFFF000;
	if (!(l2_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for (int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)l2_ptr)[l2] = addr | kPageValid | kPageTable;

		l3_ptr = addr;
	}

	auto l3_ent = ((uint64_t *)l3_ptr)[l3];

	if (l3_ent & kPageValid)
		eir::panicLogger() << "eir: Trying to map 0x" << frg::hex_fmt{address} << " twice!"
		                   << frg::endlog;

	uint64_t new_entry = physical | kPageValid | kPageL3Page | kPageAccess | kPageInnerSh;

	if (!(flags & PageFlags::write))
		new_entry |= kPageRO;
	if (!(flags & PageFlags::execute))
		new_entry |= kPageXN | kPagePXN;
	if (!(flags & PageFlags::global))
		new_entry |= kPageNotGlobal;

	if (caching_mode == CachingMode::writeCombine) {
		new_entry |= kPageGRE;
	} else if (caching_mode == CachingMode::mmio) {
		new_entry |= kPagenGnRnE;
	} else {
		assert(caching_mode == CachingMode::null);
		new_entry |= kPageWb;
	}

	if (new_entry & (0b111ULL << 48)) {
		eir::infoLogger() << "Oops, reserved bits set when mapping 0x" << frg::hex_fmt{physical}
		                  << " to 0x" << frg::hex_fmt{address} << frg::endlog;

		eir::panicLogger() << "New entry value: 0x" << frg::hex_fmt{new_entry} << frg::endlog;
	}

	((uint64_t *)l3_ptr)[l3] = new_entry;
}

address_t getSingle4kPage(address_t address) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0_ent = ((uint64_t *)eirTTBR[ttbr])[l0];
	auto l1_ptr = l0_ent & 0xFFFFFFFFF000;
	if (!(l0_ent & kPageValid))
		return -1;

	auto l1_ent = ((uint64_t *)l1_ptr)[l1];
	auto l2_ptr = l1_ent & 0xFFFFFFFFF000;
	if (!(l1_ent & kPageValid))
		return -1;

	auto l2_ent = ((uint64_t *)l2_ptr)[l2];
	auto l3_ptr = l2_ent & 0xFFFFFFFFF000;
	if (!(l2_ent & kPageValid))
		return -1;

	auto l3_ent = ((uint64_t *)l3_ptr)[l3];
	auto page_ptr = l3_ent & 0xFFFFFFFFF000;
	if (!(l3_ent & kPageValid))
		return -1;

	return page_ptr;
}

int getKernelVirtualBits() { return 49; }

void initProcessorEarly() {
	uint64_t currentel;
	asm volatile("mrs %0, currentel" : "=r"(currentel));
	eir::infoLogger() << "Starting Eir in EL " << (currentel >> 2) << frg::endlog;

	// Install exception handlers (in case the boot protocol did not do that already).
	auto vbar = reinterpret_cast<void *>(eirExcVectors);
	if ((currentel >> 2) == 1) {
		asm volatile("msr vbar_el1, %0" : : "r"(vbar) : "memory");
	} else {
		assert((currentel >> 2) == 2);
		asm volatile("msr vbar_el2, %0" : : "r"(vbar) : "memory");
	}
}

void initProcessorPaging() {
	setupPaging();
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                  << " KiB"
	                     " after setting up paging"
	                  << frg::endlog;

	// Identically map the first 128 MiB so that we can activate paging
	// without causing a page fault.
#if !defined(EIR_UEFI)
	auto floor = reinterpret_cast<address_t>(&eirImageFloor) & ~address_t{0xFFF};
	auto ceiling = (reinterpret_cast<address_t>(&eirImageCeiling) + 0xFFF) & ~address_t{0xFFF};
	for (address_t addr = floor; addr < ceiling; addr += 0x1000)
		mapSingle4kPage(addr, addr, PageFlags::write | PageFlags::execute);
#endif

	mapRegionsAndStructs();
#ifdef KERNEL_LOG_ALLOCATIONS
	allocLogRingBuffer();
#endif
}

bool patchArchSpecificManagarmElfNote(unsigned int, frg::span<char>) { return false; }

[[noreturn]] void enterKernel() {
	uint64_t aa64mmfr1;
	asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(aa64mmfr1));

	uint64_t currentel;
	asm volatile("mrs %0, currentel" : "=r"(currentel));

	if (!physOffset) {
		// Running from identity mapping. Paging may or may not be enabled.
		// Reconfigure paging.
		infoLogger() << "eir: Will reprogram MMU before jumping to kernel" << frg::endlog;

		uint64_t ctr;
		asm("mrs %0, ctr_el0" : "=r"(ctr));
		auto dcLineSize = ((ctr >> 16) & 0b1111) << 4;

		const auto &bootCaps = BootCaps::get();
		auto flushStart = bootCaps.imageStart & ~(dcLineSize - 1);
		auto flushEnd = (bootCaps.imageEnd + (dcLineSize - 1)) & ~(dcLineSize - 1);

		if ((currentel >> 2) == 2) {
			disableMmuEl2(flushStart, flushEnd, dcLineSize);

			// Enter E2H mode if VHE is supported.
			if (((aa64mmfr1 >> 8) & 0xF) == 1) {
				enterE2h();
			} else {
				// TODO: Instead of dropping to EL1 in early boot, we should drop to EL1 here
				//       (after doing the VHE detection).
				if ((currentel >> 2) == 2)
					panicLogger() << "eir: We are in EL2 but VHE is unsupported" << frg::endlog;
			}
		} else {
			if ((currentel >> 2) != 1)
				panicLogger() << "eir: Unexpected exception level: in EL" << (currentel >> 2)
				              << frg::endlog;

			disableMmuEl1(flushStart, flushEnd, dcLineSize);
		}
	} else {
		// TODO: We can continue here if E2H is already set.
		if ((currentel >> 2) != 1)
			panicLogger() << "eir: Unexpected exception level: in EL" << (currentel >> 2)
			              << " with non-identity mapping" << frg::endlog;

		// Running from non-identity mapping with paging enabled.
		// We cannot reconfigure paging.
		infoLogger()
		    << "eir: Will not reprogram MMU before jumping to kernel (non-identity mapping)"
		    << frg::endlog;
	}

	enterKernelPaging();
	eirEnterKernel(kernelEntry, getKernelStackPtr());
}

} // namespace eir
