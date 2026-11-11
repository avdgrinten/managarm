#pragma once

#include <assert.h>
#include <frg/array.hpp>
#include <stddef.h>
#include <stdint.h>

namespace common::x86 {

enum {
	kCpuIndexFeatures = 1,
	kCpuIndexStructuredExtendedFeaturesEnum = 7,
	kCpuIndexExtendedFeatures = 0x80000001,
	kCpuIndexSvmFeatures = 0x8000000A
};

enum {
	// Normal features, EDX register
	kCpuFlagPat = (1 << 16),

	// Structured extended features enumeration, EBX register
	kCpuFlagFsGsBase = 1,

	// Extendend features, EDX register
	kCpuFlagSyscall = 0x800,
	kCpuFlagNx = 0x100000,
	kCpuFlagLongMode = 0x20000000,

	// Flexible Return and Event Delivery, EAX Register, Subleaf 1
	kCpuFred = (1 << 17)
};

inline frg::array<uint32_t, 4> cpuid(uint32_t eax, uint32_t ecx = 0) {
	frg::array<uint32_t, 4> out;
	asm volatile("cpuid"
	             : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
	             : "a"(eax), "c"(ecx));
	return out;
}

enum {
	kMsrLocalApicBase = 0x0000001B,
	kMsrPkgCstConfigControl = 0x000000E2,
	kMsrIa32PowerCtl = 0x000001FC,
	kMsrPAT = 0x00000277,
	kMsrIa32PmEnable = 0x00000770,
	kMsrIa32HwpCapabilities = 0x00000771,
	kMsrIa32HwpRequest = 0x00000774,
	kMsrIa32TscDeadline = 0x000006E0,
	kMsrEfer = 0xC0000080,
	kMsrStar = 0xC0000081,
	kMsrLstar = 0xC0000082,
	kMsrFmask = 0xC0000084,
	kMsrIndexFsBase = 0xC0000100,
	kMsrIndexGsBase = 0xC0000101,
	kMsrIndexKernelGsBase = 0xC0000102,
	kMsrIndexVmCr = 0xC0010114,

	kMsrFredConfig = 0x000001D4,
	kMsrFredStkLvl = 0x000001D0,
	kMsrFredRSP0 = 0x000001CC,
	kMsrFredRSP1 = 0x000001CD,
	kMsrFredRSP2 = 0x000001CE,
	kMsrFredRSP3 = 0x000001CF
};

enum { kMsrSyscallEnable = 1 };

inline void xsave(uint8_t *area, uint64_t rfbm) {
	assert(!((uintptr_t)area & 0x3F));

	uintptr_t low = rfbm & 0xFFFFFFFF;
	uintptr_t high = (rfbm >> 32) & 0xFFFFFFFF;
	asm volatile("xsave %0" : : "m"(*area), "a"(low), "d"(high) : "memory");
}

inline void xrstor(uint8_t *area, uint64_t rfbm) {
	assert(!((uintptr_t)area & 0x3F));

	uintptr_t low = rfbm & 0xFFFFFFFF;
	uintptr_t high = (rfbm >> 32) & 0xFFFFFFFF;
	asm volatile("xrstor %0" : : "m"(*area), "a"(low), "d"(high) : "memory");
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void wrmsr(uint32_t index, uint64_t value) {
	uint32_t low = value;
	uint32_t high = value >> 32;
	asm volatile("wrmsr" : : "c"(index), "a"(low), "d"(high) : "memory");
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline uint64_t rdmsr(uint32_t index) {
	uint32_t low, high;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
	return ((uint64_t)high << 32) | (uint64_t)low;
}

inline void wrxcr(uint32_t index, uint64_t value) {
	uint32_t low = value;
	uint32_t high = value >> 32;
	asm volatile("xsetbv" : : "c"(index), "a"(low), "d"(high) : "memory");
}

inline uint64_t rdxcr(uint32_t index) {
	uint32_t low, high;
	asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(index) : "memory");
	return ((uint64_t)high << 32) | (uint64_t)low;
}

inline uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile("inb %%dx, %%al" : "=r"(out_value) : "r"(in_port));
	return out_value;
}

inline uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile("inw %%dx, %%ax" : "=r"(out_value) : "r"(in_port));
	return out_value;
}

inline void ioPeekMultiple(uint16_t port, uint16_t *dest, size_t count) {
	asm volatile("cld\n"
	             "\trep insw"
	             :
	             : "d"(port), "D"(dest), "c"(count));
}

inline void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile("outb %%al, %%dx" : : "r"(in_port), "r"(in_value));
}

} // namespace common::x86
