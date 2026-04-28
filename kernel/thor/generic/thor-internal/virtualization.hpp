#pragma once

#include <hel.h>

#include <thor-internal/address-space.hpp>
#include <thor-internal/error.hpp>

namespace thor {
	struct GuestState {
		uint64_t rax;
		uint64_t rbx;
		uint64_t rcx;
		uint64_t rdx;
		uint64_t rsi;
		uint64_t rdi;
		uint64_t rbp;
		uint64_t r8;
		uint64_t r9;
		uint64_t r10;
		uint64_t r11;
		uint64_t r12;
		uint64_t r13;
		uint64_t r14;
		uint64_t r15;
	} __attribute__((packed));

	struct VirtualizedCpu {
			virtual HelVmexitReason run() = 0;
			virtual void storeRegs(const HelX86VirtualizationRegs *regs) = 0;
			virtual void loadRegs(HelX86VirtualizationRegs *res) = 0;

	protected:
		~VirtualizedCpu() = default;
	};

	struct VirtualizedPageSpace : VirtualSpace {
		VirtualizedPageSpace(VirtualOperations *ops, smarter::shared_ptr<Hierarchy> hierarchy)
		: VirtualSpace{ops, std::move(hierarchy)} {}

	protected:
		~VirtualizedPageSpace() = default;
	};
}
