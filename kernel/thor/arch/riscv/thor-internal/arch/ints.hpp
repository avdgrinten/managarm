#pragma once

#include <riscv/csr.hpp>

namespace thor {

struct CpuData;

void initializeIrqVectors();

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline bool intsAreEnabled() {
	return riscv::readCsr<riscv::Csr::sstatus>() & riscv::sstatus::sieBit;
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void enableInts() {
	return riscv::setCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sieBit);
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void disableInts() {
	return riscv::clearCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sieBit);
}

inline void halt() { asm volatile("wfi"); }

void sendHypervisorIpi(CpuData *dstData);

} // namespace thor
