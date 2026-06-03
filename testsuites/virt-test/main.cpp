#include <hel.h>
#include <protocols/posix/procdata.hpp>
#include <hel-syscalls.h>
#include <iostream>
#include <assert.h>

#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>


int main() {
	posix::ManagarmProcessData pd;
	HEL_CHECK(helSyscall1(kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&pd)));

	HelHandle vspace, vcpu, mem;
	HEL_CHECK(helCreateVirtualizedSpace(pd.hierarchyHandle, &vspace));

	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), 0x10000, 0, nullptr, &mem));

	void *fake_ptr;
	HEL_CHECK(helMapMemory(mem, vspace, nullptr, 0x0, 0x10000, kHelMapFixed | kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute, &fake_ptr));
	assert(fake_ptr == nullptr);

	void *actual_ptr;
	HEL_CHECK(helMapMemory(mem, kHelNullHandle, nullptr, 0, 0x10000, kHelMapProtRead | kHelMapProtWrite, &actual_ptr));

	uint8_t code[] = {0xF4};
	memcpy((uint8_t *)actual_ptr + 0x7C00, code, sizeof(code));

	HEL_CHECK(helCreateVirtualizedCpu(vspace, &vcpu));

	HelX86VirtualizationRegs regs{};
	HEL_CHECK(helLoadRegisters(vcpu, kHelRegsVirtualization, (void *)&regs));

	regs.rip = 0x7C00;
	regs.rflags = (1 << 1);
	regs.cs = {.base = 0, .limit = 0xFFFF, .selector = 0, .type = 3, .present = 1, .dpl = {},
		.db = {}, .s = 1, .l = {}, .g = {}, .avl = {} };

	HelX86SegmentRegister seg{};
	seg.limit = 0xFFFF;
	seg.type = 3;
	seg.s = 1;
	seg.present = 1;
	regs.ds = seg;
	regs.es = seg;
	regs.fs = seg;
	regs.gs = seg;
	regs.ss = seg;

	regs.ldt.type = 2;
	regs.ldt.present = 1;
	regs.ldt.limit = 0xFFFF;

	regs.tr.limit = 0xFFFF;
	regs.tr.type = 3;
	regs.tr.present = 1;

	regs.gdt.limit = 0xFFFF;
	regs.idt.limit = 0xFFFF;

	HEL_CHECK(helStoreRegisters(vcpu, kHelRegsVirtualization, &regs));

	HelVmexitReason reason;
	HEL_CHECK(helRunVirtualizedCpu(vcpu, &reason));

	int32_t exitReason = static_cast<int32_t>(reason.exitReason);
	if(exitReason == kHelVmexitHlt)
		std::cout << "HLT Instruction" << std::endl;
	else if(exitReason == kHelVmexitError)
		std::cout << "VMExit error" << std::endl;
	else if(exitReason == kHelVmexitUnknownPlatformSpecificExitCode)
		std::cout << "Unknown platform specific exit code: 0x" << std::hex << reason.code << std::dec << std::endl;
	else if(exitReason == kHelVmexitTranslationFault)
		std::cout << "Translation fault: 0x" << std::hex << reason.address << std::dec << std::endl;
	else
		std::cout << "Unknown reason: " << exitReason << std::endl;

	helCloseDescriptor(kHelThisUniverse, vcpu);
	helCloseDescriptor(kHelThisUniverse, vspace);
	helCloseDescriptor(kHelThisUniverse, mem);

	return 0;
}
