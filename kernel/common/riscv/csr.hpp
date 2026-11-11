#pragma once

#include <stdint.h>

namespace riscv {

enum class Csr : uint16_t {
	// Floating point control and status.
	fcsr = 0x3,
	// Supervisor trap setup.
	sstatus = 0x100,    // Status register.
	sie = 0x104,        // Interrupt enable.
	stvec = 0x105,      // Trap vector address.
	scounteren = 0x106, // Counter enable.
	senvcfg = 0x10A,    // Environment configuration.
	// Supervisor trap handling.
	sscratch = 0x140,
	sepc = 0x141,     // Exception program counter.
	scause = 0x142,   // Cause of exception.
	stval = 0x143,    // Trap value.
	sip = 0x144,      // Interrupt pending.
	stimecmp = 0x14d, // Timer comparison register.
	// Indirect access CSRs.
	siselect = 0x150,
	sireg = 0x151,
	// Supervisor level interrupts.
	stopei = 0x15c,
	// Supervisor protection and translation.
	satp = 0x180, // Address translation.
	// Hypervisor trap setup.
	hstatus = 0x600,    // Hypervisor status register.
	hedeleg = 0x602,    // Hypervisor exception delegation register.
	hideleg = 0x603,    // Hypervisor interrupt delegation register.
	hie = 0x604,        // Hypervisor interrupt-enable register.
	hcounteren = 0x606, // Hypervisor counter enable.
	hgeie = 0x607,      // Hypervisor guest external interrupt-enable register.
	// Hypervisor trap handling.
	htval = 0x643,  // Hypervisor trap value.
	hip = 0x644,    // Hypervisor interrupt pending.
	hvip = 0x645,   // Hypervisor virtual interrupt pending.
	htinst = 0x64a, // Hypervisor trap instruction.
	hgeip = 0xe12,  // Hypervisor guest external interrupt pending.
	// Hypervisor configuration.
	henvcfg = 0x60a, // Hypervisor environment configuration register.
	// Hypervisor protection and translation.
	hgatp = 0x680, // Hypervisor guest address translation and protection.
	// Hypervisor counter/timer virtualization registers.
	htimedelta = 0x605, // Delta for VS/VU-mode timer.
	// Hypervisor state enable registers.
	hstateen0 = 0x60c, // Hypervisor state enable 0 register.
	hstateen1 = 0x60d, // Hypervisor state enable 1 register.
	hstateen2 = 0x60e, // Hypervisor state enable 2 register.
	hstateen3 = 0x60f, // Hypervisor state enable 3 register.
	// Virtual supervisor registers.
	vsstatus = 0x200,  // Virtual supervisor status register.
	vsie = 0x204,      // Virtual supervisor interrupt-enable register.
	vstvec = 0x205,    // Virtual supervisor trap handler base address.
	vsscratch = 0x240, // Virtual supervisor scratch register.
	vsepc = 0x241,     // Virtual supervisor exception program counter.
	vscause = 0x242,   // Virtual supervisor trap cause.
	vstval = 0x243,    // Virtual supervisor trap value.
	vsip = 0x244,      // Virtual supervisor interrupt pending.
	vsatp = 0x280,     // Virtual supervisor address translation and protection.
	// Virtual supervisor timer compare.
	vstimecmp = 0x24d, // Virtual supervisor timer compare.
};

namespace sstatus {

constexpr uint64_t sieBit = UINT64_C(1) << 1;  // Interrupt enable.
constexpr uint64_t spieBit = UINT64_C(1) << 5; // Previous interrupt enable.
constexpr uint64_t ubeBit = UINT64_C(1) << 6;  // U-mode is big endian or not.
constexpr uint64_t sppBit = UINT64_C(1) << 8;  // Previous privilege level (S-mode or not).
constexpr uint64_t sumBit = UINT64_C(1) << 18; // User memory access permitted in S-mode.
constexpr uint64_t mxrBit = UINT64_C(1)
                            << 19; // Executable page permission implies readable in S-mode.

// VS, FS, XS: floating point and vector extension state.
constexpr int vsShift = 9;
constexpr int fsShift = 13;
constexpr int xsShift = 15;

// Values for the VS, FS, XS fields.
constexpr uint64_t extMask = 3;
constexpr uint64_t extOff = 0;
constexpr uint64_t extInitial = 1;
constexpr uint64_t extClean = 2;
constexpr uint64_t extDirty = 3;

// U-mode execution width.
constexpr int uxlShift = 32;
constexpr uint64_t uxlMask = 3;
constexpr uint64_t uxl32 = 1;
constexpr uint64_t uxl64 = 2;
constexpr uint64_t uxl128 = 3;

} // namespace sstatus

namespace senvcfg {

// Enables the execution of `cbo.inval` in U-mode, the instruction performs a flush operation.
constexpr uint64_t cbie = UINT64_C(0b01) << 4;
// Enables the execution of `cbo.clean` and `cbo.flush` instructions in U-mode.
constexpr uint64_t cbcfe = UINT64_C(1) << 6;

} // namespace senvcfg

namespace interrupts {

constexpr uint64_t ssi = 1; // Supervisor software interrupt.
constexpr uint64_t sti = 5; // Supervisor timer interrupt.
constexpr uint64_t sei = 9; // Supervisor external interrupt.

} // namespace interrupts

namespace hstatus {

constexpr uint64_t vsbe = UINT64_C(1) << 5; // VS-mode big endian.
constexpr uint64_t gva = UINT64_C(1) << 6;  // Guest virtual address stored in stval.
constexpr uint64_t spv = UINT64_C(1) << 7;  // Previous virtualizatation mode.
constexpr uint64_t spvp = UINT64_C(1) << 8; // Previous virtual privilege.
constexpr uint64_t hu = UINT64_C(1) << 9;   // Hypervisor in U-mode.
constexpr uint64_t vtvm = UINT64_C(1)
                          << 20; // sfence.vma/sinval.vma/satp raises virtual-instruction exception.
constexpr uint64_t vtw = UINT64_C(1) << 21;  // wfi raises virtual-instruction exception.
constexpr uint64_t vtsr = UINT64_C(1) << 22; // sret raises virtual-instruction exception.

// XLEN for VS-mode.
constexpr uint64_t vsxlShift = 32;
constexpr uint64_t vsxlMask = 0b11;

// Pointer masking.
constexpr uint64_t hupmmShift = 48;
constexpr uint64_t hupmmMask = 0b11;

} // namespace hstatus

namespace scounteren {

constexpr uint64_t cy = UINT64_C(1) << 0; // cycle register enable
constexpr uint64_t tm = UINT64_C(1) << 1; // time register enable
constexpr uint64_t ir = UINT64_C(1) << 2; // instret register enable

} // namespace scounteren

namespace hcounteren {

constexpr uint64_t cy = UINT64_C(1) << 0; // cycle register enable
constexpr uint64_t tm = UINT64_C(1) << 1; // time register enable
constexpr uint64_t ir = UINT64_C(1) << 2; // instret register enable

} // namespace hcounteren

namespace henvcfg {

constexpr uint64_t lpe = UINT64_C(1) << 2; // Branch tracking enable (Zicfilp).
constexpr uint64_t sse = UINT64_C(1) << 3; // Shadow stack enable (Zicfiss).
constexpr uint64_t cbcfe = UINT64_C(1)
                           << 6; // Cache block clean and flush instruction enable (Zicbom).
constexpr uint64_t cbze = UINT64_C(1) << 7;   // Cache block zero instruction enable (Zicboz).
constexpr uint64_t dte = UINT64_C(1) << 59;   // Double-trap enable (Ssdbltrp).
constexpr uint64_t adue = UINT64_C(1) << 61;  // Hardware updating of PTE A/D enable (Svadu).
constexpr uint64_t pbmte = UINT64_C(1) << 62; // Page-based memory types enable (Svpbmt).
constexpr uint64_t stce = UINT64_C(1) << 63;  // STimecmp enable (Sstc).

// Cache block invalidate instruction enable (Zicbom).
constexpr uint64_t cbieShift = 4;
constexpr uint64_t cbieMask = 0b11;

// Guest controls whether cbo.inval performs a flush or invalidate.
constexpr uint64_t cbieGuestControl = 0b11;

// Pointer masking enable (Ssnpm).
constexpr uint64_t pmmShift = 32;
constexpr uint64_t pmmMask = 0b11;

} // namespace henvcfg

namespace hgatp {

constexpr uint64_t modeShift = 60;
constexpr uint64_t modeMask = 0xf;

constexpr uint64_t sv39 = 8;
constexpr uint64_t sv48 = 9;
constexpr uint64_t sv57 = 10;

} // namespace hgatp

// The CSR manipulation instructions on RISC-V take the CSR as an immediate operand.
// Since we do not want to add separate read/write functions for each CSR,
// using a template is out best option to ensure that the CSR is statically known.

// Always inline as this might be called from code that disables coverage
template <Csr csr>
[[gnu::always_inline]] inline uint64_t readCsr() {
	uint64_t u;
	asm volatile("csrr %0, %1" : "=r"(u) : "i"(static_cast<uint16_t>(csr)) : "memory");
	return u;
}

template <Csr csr>
void writeCsr(uint64_t v) {
	asm volatile("csrw %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

template <Csr csr>
uint64_t readWriteCsr(uint64_t v) {
	uint64_t u;
	asm volatile("csrrw %0, %1, %2" : "=r"(u) : "i"(static_cast<uint16_t>(csr)), "r"(v) : "memory");
	return u;
}

// Always inline as this might be called from code that disables coverage
template <Csr csr>
[[gnu::always_inline]] inline void setCsrBits(uint64_t v) {
	asm volatile("csrs %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

// Always inline as this might be called from code that disables coverage
template <Csr csr>
[[gnu::always_inline]] inline void clearCsrBits(uint64_t v) {
	asm volatile("csrc %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

} // namespace riscv
