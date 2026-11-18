#include <optional>

#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/arch/pic.hpp>
#include <x86/machine.hpp>

using namespace thor::x86_virt;

namespace {
	inline int vmptrld(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmptrld %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	inline int vmclear(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmclear %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	inline int vmwrite(uint64_t encoding, uint64_t value) {
		uint8_t ret;
		asm volatile (
			"vmwrite %1, %2;"
			"setna %[ret]"
			: [ret]"=rm"(ret)
			: "rm"(value), "r"(encoding)
			: "cc", "memory");

		return ret;
	}

	inline uint64_t vmread(uint64_t encoding) {
		uint64_t tmp;
		uint8_t ret;
		asm volatile(
			"vmread %[encoding], %[value];"
			"setna %[ret];"
			: [value]"=rm"(tmp), [ret]"=rm"(ret)
			: [encoding]"r"(encoding)
			: "cc", "memory");

		return tmp;
	}

	inline uint32_t adjustControls(uint64_t msr, uint32_t desired, uint32_t required, const char *name) {
		auto caps = common::x86::rdmsr(msr);
		// The low half of the IA32_VMX_*_CTLS MSRs holds the controls that must be 1
		// and the high half holds the controls that may be 1.
		uint32_t controls = (desired | static_cast<uint32_t>(caps)) & static_cast<uint32_t>(caps >> 32);
		if((controls & required) != required) {
			thor::infoLogger() << "vmx: CPU lacks required " << name << " controls 0x"
					<< frg::hex_fmt{required & ~controls} << frg::endlog;
		}
		return controls;
	}

	inline bool haveTrueControls() {
		return common::x86::rdmsr(thor::vmx::IA32_VMX_BASIC_MSR) & (UINT64_C(1) << 55);
	}

	// The IA32_VMX_CR{0,4}_FIXED{0,1} MSRs, read once by vmxon().
	uint64_t cr0Fixed0, cr0Fixed1, cr4Fixed0, cr4Fixed1;

	// Bits of CR0/CR4 that VMX forces to 1 in the guest.
	// The guest sees its own values of these bits through the read shadows.
	uint64_t cr0Forced() {
		// Unrestricted guests can run without paging and protected mode.
		return cr0Fixed0 & ~UINT64_C(0x8000'0001);
	}

	uint64_t cr4Forced() {
		return cr4Fixed0;
	}

	uint64_t guestCr0(uint64_t cr0) {
		return (cr0 | cr0Forced()) & cr0Fixed1;
	}

	uint64_t guestCr4(uint64_t cr4) {
		return (cr4 | cr4Forced()) & cr4Fixed1;
	}

	constexpr uint64_t CR0_PE = UINT64_C(1) << 0;
	constexpr uint64_t CR0_TS = UINT64_C(1) << 3;
	constexpr uint64_t CR0_NW = UINT64_C(1) << 29;
	constexpr uint64_t CR0_CD = UINT64_C(1) << 30;
	constexpr uint64_t CR0_PG = UINT64_C(1) << 31;
	constexpr uint64_t CR4_PAE = UINT64_C(1) << 5;
	constexpr uint64_t CR4_VMXE = UINT64_C(1) << 13;
	constexpr uint64_t CR4_OSXSAVE = UINT64_C(1) << 18;
	constexpr uint64_t EFER_LME = UINT64_C(1) << 8;
	constexpr uint64_t EFER_LMA = UINT64_C(1) << 10;
	constexpr uint32_t MSR_CSTAR = 0xC0000083;

	// CR4 bits whose writes exit: the bits that VMX forces to 1 and OSXSAVE
	// which tells us whether XCR0 needs to be switched.
	uint64_t cr4Mask() {
		return cr4Forced() | CR4_OSXSAVE;
	}

	// The host values of the MSRs that are used by SYSCALL; read once by vmxon().
	uint64_t hostStar, hostLstar, hostCstar, hostSfmask;

	// Writes the host selectors, which are the same on all CPUs.
	void writeHostSelectors() {
		uint16_t es, cs, ss, ds, fs, gs, tr;
		asm volatile ("str %[tr]" : [tr]"=rm"(tr));
		asm volatile ("mov %%es,%0" : "=r"(es));
		asm volatile ("mov %%cs,%0" : "=r"(cs));
		asm volatile ("mov %%ss,%0" : "=r"(ss));
		asm volatile ("mov %%ds,%0" : "=r"(ds));
		asm volatile ("mov %%fs,%0" : "=r"(fs));
		asm volatile ("mov %%gs,%0" : "=r"(gs));

		vmwrite(thor::vmx::HOST_ES_SELECTOR, es);
		vmwrite(thor::vmx::HOST_CS_SELECTOR, cs);
		vmwrite(thor::vmx::HOST_SS_SELECTOR, ss);
		vmwrite(thor::vmx::HOST_DS_SELECTOR, ds);
		vmwrite(thor::vmx::HOST_FS_SELECTOR, fs);
		vmwrite(thor::vmx::HOST_GS_SELECTOR, gs);
		vmwrite(thor::vmx::HOST_TR_SELECTOR, tr);
	}

	// Writes the host state that differs between CPUs.
	void writeHostCpuState() {
		vmwrite(thor::vmx::HOST_GS_BASE, common::x86::rdmsr(thor::vmx::MSR_GS_BASE));

		common::x86::Gdtr gdtr;
		asm volatile("sgdt %[gdt]": [gdt]"=m"(gdtr));
		common::x86::Idtr idtr;
		asm volatile("sidt %[idt]": [idt]"=m"(idtr));

		uint32_t* gdt = gdtr.pointer;
		uint32_t entry1 = (gdt[thor::kGdtIndexTask * 2] >> 16) & 0xFFFF;
		uint32_t entry2 = (gdt[thor::kGdtIndexTask * 2 + 1]) & 0xFF;
		uint32_t entry3 = (gdt[thor::kGdtIndexTask * 2 + 1] >> 24) & 0xFF;
		uint32_t entry4 = (gdt[thor::kGdtIndexTask * 2 + 2]);
		uint64_t trAddr = ((uint64_t)entry4 << 32) |
			(entry1 | ((entry2) << 16) | ((entry3) << 24));

		vmwrite(thor::vmx::HOST_TR_BASE, trAddr);
		vmwrite(thor::vmx::HOST_GDTR_BASE, (size_t)gdtr.pointer);
		vmwrite(thor::vmx::HOST_IDTR_BASE, (size_t)idtr.pointer);
	}

	// Translates the qualification of an EPT violation to kHelVmFault* flags.
	size_t translateEptFaultFlags(uint64_t qualification) {
		size_t flags = 0;
		if(qualification & (1 << 0))
			flags |= kHelVmFaultRead;
		if(qualification & (1 << 1))
			flags |= kHelVmFaultWrite;
		if(qualification & (1 << 2))
			flags |= kHelVmFaultExecute;
		return flags;
	}

	// Returns the GPR with the given encoding in instructions.
	uint64_t *gprSlot(thor::vmx::GuestState &state, unsigned index) {
		uint64_t *slots[16] = {
			&state.rax, &state.rcx, &state.rdx, &state.rbx,
			nullptr /* RSP is part of the VMCS. */, &state.rbp, &state.rsi, &state.rdi,
			&state.r8, &state.r9, &state.r10, &state.r11,
			&state.r12, &state.r13, &state.r14, &state.r15,
		};
		return slots[index];
	}
}

extern "C" bool vmxVmRun(thor::vmx::Vmcs* vm, void* state, bool launched);

namespace thor::vmx {
	// The VMCS that is current on each CPU (VMPTRLD), or null.
	extern PerCpu<Vmcs *> currentVmcs;
	THOR_DEFINE_PERCPU(currentVmcs);
}

extern "C" uintptr_t vmxDoVmExit[]; // Actually a function that cannot be called in the conventional sense, just need the address

extern "C" void vmxUpdateHostRsp(thor::vmx::Vmcs* vm, uintptr_t rsp) {
	if(vm->saved_host_rsp != rsp) {
		vmwrite(thor::vmx::HOST_RSP, rsp);
		vm->saved_host_rsp = rsp;
	}
}

namespace thor::vmx {
	bool vmxon() {
		infoLogger() << "vmx: enabling vmx" << frg::endlog;

		auto vmxonRegion = physicalAllocator->allocateInfallible(PhysicalKind::pinned);

		PageAccessor vmxonAccessor{vmxonRegion};
		memset(vmxonAccessor.get(), 0, kPageSize);
		uint64_t control = common::x86::rdmsr(common::x86::kMsrIa32FeatureControl);
		if(!(control & FEATURE_CONTROL_LOCK)) {
			//Enabled outside of SMX and lock bit.
			control |= FEATURE_CONTROL_LOCK;
			control |= FEATURE_CONTROL_VMXON_OUTSIDE_SMX;
			common::x86::wrmsr(common::x86::kMsrIa32FeatureControl, control);
		} else {
			// The MSR is read-only once the lock bit is set.
			assert(control & FEATURE_CONTROL_VMXON_OUTSIDE_SMX);
		}

		uint64_t cr0;
		asm volatile ("mov %%cr0, %0" : "=r" (cr0));
		cr0 &= common::x86::rdmsr(0x487);
		cr0 |= common::x86::rdmsr(0x486);
		asm volatile ("mov %0, %%cr0" : : "r" (cr0));

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= 1 << 13;
		cr4 &= common::x86::rdmsr(0x489);
		cr4 |= common::x86::rdmsr(0x488);
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		eptAccessedDirty = common::x86::rdmsr(IA32_VMX_EPT_VPID_CAP_MSR) & (1 << 21);
		cr0Fixed0 = common::x86::rdmsr(IA32_VMX_CR0_FIXED0_MSR);
		cr0Fixed1 = common::x86::rdmsr(IA32_VMX_CR0_FIXED1_MSR);
		cr4Fixed0 = common::x86::rdmsr(IA32_VMX_CR4_FIXED0_MSR);
		cr4Fixed1 = common::x86::rdmsr(IA32_VMX_CR4_FIXED1_MSR);
		hostStar = common::x86::rdmsr(common::x86::kMsrStar);
		hostLstar = common::x86::rdmsr(common::x86::kMsrLstar);
		hostCstar = common::x86::rdmsr(MSR_CSTAR);
		hostSfmask = common::x86::rdmsr(common::x86::kMsrFmask);

		//Set vmx revision
		uint32_t vmxRevision = common::x86::rdmsr(0x480);
		*(uint32_t*)vmxonAccessor.get() = static_cast<uint32_t>(vmxRevision);
		uint16_t successful = 0;
		asm volatile(
			"vmxon %1;"
			"jnc success;"
			"movq $0, %%rdx;"
			"success:"
			"movq $1, %%rdx;"
			:"=d"(successful)
			:"m"(vmxonRegion)
			:"memory", "cc"
		);

		if(successful) {
			infoLogger() << "thor: CPU entered vmxon operation" << frg::endlog;
		} else {
			urgentLogger() << "thor: vmxon failed; this will be a hard error in the future"
					<< frg::endlog;
		}

		return successful;
	}

	Vmcs::Vmcs(CtorToken, smarter::shared_ptr<EptSpace> ept)
	: space(std::move(ept)) {
		infoLogger() << "vmx: Creating VMCS" << frg::endlog;
		region = (void*)physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		PageAccessor regionAccessor{(PhysicalAddr)region};
		memset(regionAccessor.get(), 0, kPageSize);
		uint32_t vmxRevision = common::x86::rdmsr(0x480);
		*(uint32_t*)regionAccessor.get() = static_cast<uint32_t>(vmxRevision);

		// Allocate before disabling interrupts (the allocator must not run with interrupts disabled).
		msrArea_ = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		PageAccessor msrAccessor{msrArea_};
		memset(msrAccessor.get(), 0, kPageSize);
		// The physical memory stays mapped after the accessor is gone.
		guestMsrs_ = reinterpret_cast<MsrEntry *>(msrAccessor.get());
		hostMsrs_ = guestMsrs_ + numSwitchedMsrs;

		// A VMCS is only active (and thus may be cached by the CPU) while we cannot migrate
		// to another CPU. Afterwards, we VMCLEAR it (see acquire_() and release_()).
		StatelessIrqLock irqLock;
		vmclear((PhysicalAddr)region);
		acquire_();

		//Set up basic controls.
		auto trueControls = haveTrueControls();
		// External interrupt exiting and (if available) the VMX-preemption timer.
		uint32_t pinControls = adjustControls(
				trueControls ? IA32_VMX_TRUE_PINBASED_CTLS_MSR : MSR_IA32_VMX_PINBASED_CTLS,
				1 | PIN_BASED_PREEMPTION_TIMER, 1, "pin-based");
		vmwrite(PIN_BASED_VM_EXEC_CONTROLS, pinControls);
		havePreemptionTimer_ = pinControls & PIN_BASED_PREEMPTION_TIMER;
		if(havePreemptionTimer_) {
			preemptionTimerShift_ = common::x86::rdmsr(IA32_VMX_MISC_MSR) & 0x1F;
			vmwrite(GUEST_VMX_PREEMPTION_TIMER, UINT32_MAX);
		}
		// Without a TPR shadow, CR8 accesses would otherwise reach the host's TPR.
		uint32_t procControls = VMEXIT_ON_HLT
				| VMEXIT_ON_PIO
				| SECONDARY_CONTROLS_ON
				| CR8_LOAD_EXITING
				| CR8_STORE_EXITING;
		// Fail early if interrupt-window exiting is unsupported; it is only enabled on demand.
		procControls_ = adjustControls(
				trueControls ? IA32_VMX_TRUE_PROCBASED_CTLS_MSR : MSR_IA32_VMX_PROCBASED_CTLS,
				procControls | INTERRUPT_WINDOW_EXITING,
				procControls | INTERRUPT_WINDOW_EXITING, "processor-based")
				& ~INTERRUPT_WINDOW_EXITING;
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS, procControls_);
		currentProcControls_ = procControls_;
		uint32_t secondaryControls = EPT_ENABLE | UNRESTRICTED_GUEST;
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS2, adjustControls(IA32_VMX_SEC_PROCBASED_CTLS_MSR,
				secondaryControls, secondaryControls, "secondary processor-based"));
		vmwrite(EXCEPTION_BITMAP, 0);

		uint32_t exitControls = VMEXIT_CONTROLS_LONG_MODE | VMEXIT_CONTROLS_LOAD_IA32_EFER;
		// Saving the guest's EFER keeps EFER.LMA up to date (the CPU sets it when entering long mode).
		vmwrite(VM_EXIT_CONTROLS, adjustControls(
				trueControls ? IA32_VMX_TRUE_EXIT_CTLS_MSR : MSR_IA32_VMX_EXIT_CTLS,
				exitControls | VMEXIT_CONTROLS_SAVE_IA32_EFER, exitControls, "VM-exit"));

		uint32_t entryControls = 1 << 15; // Load IA32_EFER.
		entryControls_ = adjustControls(
				trueControls ? IA32_VMX_TRUE_ENTRY_CTLS_MSR : MSR_IA32_VMX_ENTRY_CTLS,
				entryControls, entryControls, "VM-entry") & ~VM_ENTRY_IA32E_MODE;
		vmwrite(VM_ENTRY_CONTROLS, entryControls_);

		// Hide the bits that VMX forces to 1 from the guest.
		vmwrite(CR0_GUEST_HOST_MASK, cr0Forced());
		vmwrite(CR4_GUEST_HOST_MASK, cr4Mask());

		// The CPU switches these MSRs through the MSR areas. Unlike WRMSR, this does not
		// cause additional exits under nested virtualization. The guest's KERNEL_GS_BASE
		// entry doubles as the VM-exit MSR-store area.
		uint32_t msrIndices[numSwitchedMsrs] = {
			common::x86::kMsrStar, common::x86::kMsrLstar, MSR_CSTAR, common::x86::kMsrFmask,
			common::x86::kMsrIndexKernelGsBase,
		};
		// The host's KERNEL_GS_BASE (i.e., the user GS base) is set by run().
		uint64_t hostValues[numSwitchedMsrs] = {hostStar, hostLstar, hostCstar, hostSfmask, 0};
		for(unsigned i = 0; i < numSwitchedMsrs; i++) {
			guestMsrs_[i].index = msrIndices[i];
			hostMsrs_[i].index = msrIndices[i];
			hostMsrs_[i].data = hostValues[i];
		}
		vmwrite(VM_ENTRY_MSR_LOAD_ADDR, msrArea_);
		vmwrite(VM_EXIT_MSR_LOAD_ADDR, msrArea_ + numSwitchedMsrs * sizeof(MsrEntry));
		vmwrite(VM_EXIT_MSR_STORE_ADDR, msrArea_ + kernelGsBaseEntry * sizeof(MsrEntry));
		vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
		vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
		vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);

		// Thor does not use SYSENTER.
		vmwrite(HOST_IA32_SYSENTER_CS, 0);
		vmwrite(HOST_IA32_SYSENTER_ESP, 0);
		vmwrite(HOST_IA32_SYSENTER_EIP, 0);
		vmwrite(GUEST_IA32_SYSENTER_CS, 0);
		vmwrite(GUEST_IA32_SYSENTER_ESP, 0);
		vmwrite(GUEST_IA32_SYSENTER_EIP, 0);

		uint64_t cr0, cr4;
		asm volatile ("mov %%cr0,%0" : "=r"(cr0));
		asm volatile ("mov %%cr4,%0" : "=r"(cr4));

		//Set up host state on vmexit.
		uint32_t cr0Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR0_FIXED0_MSR);
		vmwrite(HOST_CR0, cr0Fixed | cr0);
		uint32_t cr4Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR4_FIXED0_MSR);
		vmwrite(HOST_CR4, cr4Fixed | cr4);

		vmwrite(HOST_EFER_FULL, common::x86::rdmsr(0xc0000080));
		vmwrite(HOST_RIP, (size_t)vmxDoVmExit);

		//Set up guest state on vm entry.
		vmwrite(GUEST_ES_SELECTOR, 0x0);
		vmwrite(GUEST_CS_SELECTOR, 0);
		vmwrite(GUEST_DS_SELECTOR, 0x0);
		vmwrite(GUEST_FS_SELECTOR, 0x0);
		vmwrite(GUEST_GS_SELECTOR, 0x0);
		vmwrite(GUEST_SS_SELECTOR, 0x0);
		vmwrite(GUEST_TR_SELECTOR, 0x0);
		vmwrite(GUEST_LDTR_SELECTOR, 0x0);
		vmwrite(GUEST_CS_BASE, 0x0);
		vmwrite(GUEST_DS_BASE, 0x0);
		vmwrite(GUEST_ES_BASE, 0x0);
		vmwrite(GUEST_FS_BASE, 0x0);
		vmwrite(GUEST_GS_BASE, 0x0);
		vmwrite(GUEST_SS_BASE, 0x0);
		vmwrite(GUEST_LDTR_BASE, 0x0);
		vmwrite(GUEST_IDTR_BASE, 0x0);
		vmwrite(GUEST_GDTR_BASE, 0x0);
		vmwrite(GUEST_TR_BASE, 0x0);
		vmwrite(GUEST_CS_LIMIT, 0xffff);
		vmwrite(GUEST_DS_LIMIT, 0xffff);
		vmwrite(GUEST_ES_LIMIT, 0xffff);
		vmwrite(GUEST_FS_LIMIT, 0xffff);
		vmwrite(GUEST_GS_LIMIT, 0xffff);
		vmwrite(GUEST_SS_LIMIT, 0xffff);
		vmwrite(GUEST_LDTR_LIMIT, 0xffff);
		vmwrite(GUEST_TR_LIMIT, 0xffff);
		vmwrite(GUEST_GDTR_LIMIT, 0xffff);
		vmwrite(GUEST_IDTR_LIMIT, 0xffff);

		vmwrite(GUEST_CS_ACCESS_RIGHT, CODE_ACCESS_RIGHT);
		vmwrite(GUEST_DS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_ES_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_FS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_GS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_SS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_LDTR_ACCESS_RIGHT, LDTR_ACCESS_RIGHT);
		vmwrite(GUEST_TR_ACCESS_RIGHT, TR_ACCESS_RIGHT);
		vmwrite(GUEST_INTERRUPTIBILITY_STATE, 0x0);
		vmwrite(GUEST_ACTIVITY_STATE, 0x0);
		vmwrite(GUEST_DR7, 0x0);
		vmwrite(GUEST_RSP, 0x0);
		vmwrite(GUEST_RIP, 0x1000);
		vmwrite(GUEST_RFLAG, RFLAG_RESERVED);
		vmwrite(VMCS_LINK_POINTER, -1ll);
		vmwrite(VMCS_FIELD_GUEST_EFER_FULL, 0x0);

		vmwrite(GUEST_INTR_STATUS, 0);
		vmwrite(GUEST_PML_INDEX, 0);
		vmwrite(GUEST_CR0, guestCr0(0));
		vmwrite(CR0_READ_SHADOW, 0);
		vmwrite(GUEST_CR3, 0x0);
		vmwrite(GUEST_CR4, guestCr4(0));
		vmwrite(CR4_READ_SHADOW, 0);

		auto eptCaps = common::x86::rdmsr(IA32_VMX_EPT_VPID_CAP_MSR);
		if(!(eptCaps & (1 << 6)) || !(eptCaps & (1 << 14)))
			infoLogger() << "vmx: CPU lacks 4-level EPT or write-back EPT structures" << frg::endlog;
		if(!(eptCaps & (1 << 25)))
			infoLogger() << "vmx: CPU lacks single-context INVEPT" << frg::endlog;
		uint64_t eptp = eptPointer(space->rootTable());
		vmwrite(CTLS_EPTP, eptp);

		release_();
		irqLock.unlock();
		state = {};


		if(getGlobalCpuFeatures()->haveXsave){
			hostFstate = (uint8_t*)kernelAlloc->allocate(getGlobalCpuFeatures()->xsaveRegionSize);
			memset((void*)hostFstate, 0, getGlobalCpuFeatures()->xsaveRegionSize);

			guestFstate = (uint8_t*)kernelAlloc->allocate(getGlobalCpuFeatures()->xsaveRegionSize);
			memset((void*)guestFstate, 0, getGlobalCpuFeatures()->xsaveRegionSize);
		} else {
			hostFstate = (uint8_t*)kernelAlloc->allocate(512);
			memset((void*)hostFstate, 0, 512);

			guestFstate = (uint8_t*)kernelAlloc->allocate(512);
			memset((void*)guestFstate, 0, 512);
		}
	}

	frg::expected<Error, HelVmexitReason> Vmcs::run(const HelVmexitCompletion *completion) {
		HelVmexitReason exitInfo{};

		// Apply the completion. The GPRs are not part of the VMCS. Skipping the instruction
		// advances the cached RIP; the VMCS is updated before the next entry (even if run()
		// returns before entering the guest).
		bool skipCompleted;
		if(auto error = applyCompletion(completion, state.rax, state.rbx, state.rcx, state.rdx,
				skipCompleted); error)
			return *error;
		if(skipCompleted) {
			loadBasicRegsCache();
			cachedRip_ += pendingCompletion_.instructionLength;
			if(regsCacheValid_)
				regsCache_.rip = cachedRip_;
			ripDirty_ = true;
		}

		// While we are in the kernel, KERNEL_GS_BASE holds the user GS base of this thread
		// (and thread switches preserve it).
		if(msrCount_)
			hostMsrs_[kernelGsBaseEntry].data = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);

		uint64_t hostFsBase = common::x86::rdmsr(MSR_FS_BASE);
		uint64_t hostXcr0 = getGlobalCpuFeatures()->xcr0Mask;

		// While the VMCS is resident, it stays active on this CPU between entries.
		std::optional<MigrationGuard> migrationGuard;
		auto leaveResidency = [&] {
			if(!resident_)
				return;
			{
				StatelessIrqLock irqLock;
				resident_ = false;
				release_();
			}
			migrationGuard.reset();
		};
		frg::scope_exit residencyScope{leaveResidency};

		while(1) {
			if(getCurrentThread()->checkCancelConditions())
				return Error::cancelled;

			lapic_.checkTimer(getClockNanos());

			// HLT ends once an interrupt can be delivered.
			if(halted_) {
				// HLT exits cache RFLAGS and helStoreRegisters() updates the cache.
				bool interruptsEnabled = cachedRflags_ & (1 << 9);
				if(interruptsEnabled && lapic_.deliverableVector() >= 0) {
					halted_ = false;
				}else{
					if(consumeKick()) {
						exitInfo.exitReason = kHelVmexitKicked;
						return exitInfo;
					}
					auto deadline = lapic_.timerDeadline();
					leaveResidency();
					waitForGuestInterrupt(deadline, interruptsEnabled);
					continue;
				}
			}

			if(!resident_) {
				migrationGuard.emplace();
				resident_ = true;
			}

			bool entryFailed;
			uint64_t error, reason;
			// Exit information that we read while the VMCS is active and use afterwards.
			uint64_t qualification = 0;
			bool stringIo = false;
			uint64_t eptAddress = 0;
			bool msrExit = false;
			bool lapicMsr = false;
			bool needsCompletion = false;
			VirtualLapic::WriteEffects lapicEffects;
			uint64_t instructionLength = 0;

			{
				StatelessIrqLock irqLock;
				if(enterGuest(getCpuData())) {
					exitInfo.exitReason = kHelVmexitKicked;
					return exitInfo;
				}
				acquire_();
				// VMX instructions are expensive under nested virtualization; avoid needless ones.
				// The host state only differs between CPUs and threads.
				auto cpu = getCpuData();
				if(!hostSelectorsWritten_) {
					writeHostSelectors();
					hostSelectorsWritten_ = true;
				}
				if(hostStateCpu_ != cpu) {
					writeHostCpuState();
					hostStateCpu_ = cpu;
				}
				if(!hostFsBaseWritten_ || hostFsBase_ != hostFsBase) {
					vmwrite(HOST_FS_BASE, hostFsBase);
					hostFsBase_ = hostFsBase;
					hostFsBaseWritten_ = true;
				}
				// CR3 differs between threads.
				// Even for the same thread, it changes when the PCID of our address space is rebound.
				uint64_t cr3;
				asm volatile ("mov %%cr3, %0" : "=r"(cr3));
				if(cr3 != hostCr3_) {
					vmwrite(HOST_CR3, cr3);
					hostCr3_ = cr3;
				}
				if(!space->enterGuest(cpu)) {
					leaveGuest();
					return Error::illegalState;
				}
				// Write the RIP of the instruction that userspace completed.
				if(ripDirty_) {
					vmwrite(GUEST_RIP, cachedRip_);
					clearInterruptShadow();
					ripDirty_ = false;
				}
				// The guest changes its state while it runs.
				regsCacheValid_ = false;
				basicCacheValid_ = false;

				// Inject an event: an interrupted or queued event takes precedence over
				// interrupts from the local APIC. External interrupts need to wait until the guest
				// can accept them; we exit once an interrupt window opens.
				bool wantWindow = false;
				Event interrupt;
				const Event *injected = nullptr;
				int vector = lapic_.deliverableVector();
				if(reinjectValid_) {
					injected = &reinject_;
					wantWindow = vector >= 0;
				}else if(vector >= 0) {
					bool shadow = vmread(GUEST_INTERRUPTIBILITY_STATE) & 3;
					bool interruptsEnabled = vmread(GUEST_RFLAG) & (1 << 9);
					if(!interruptsEnabled || shadow) {
						wantWindow = true;
					}else{
						lapic_.acknowledge(vector);
						interrupt = {kEventExternalInterrupt, static_cast<uint8_t>(vector), false, 0, 0};
						injected = &interrupt;
					}
				}
				// Without an event, VM_ENTRY_INTR_INFO needs no write: VM exits clear its valid bit.
				if(injected) {
					uint32_t info = injected->vector
							| (static_cast<uint32_t>(injected->type) << 8)
							| (UINT32_C(1) << 31);
					// Error codes are only delivered if guest's CR0.PE is set.
					if(injected->hasErrorCode && (vmread(GUEST_CR0) & CR0_PE)) {
						info |= 1 << 11;
						vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, injected->errorCode);
					}
					if(injected->type >= kEventSoftwareInterrupt)
						vmwrite(VM_ENTRY_INSTRUCTION_LEN, injected->instructionLength);
					vmwrite(VM_ENTRY_INTR_INFO, info);
				}
				uint32_t controls = procControls_ | (wantWindow ? INTERRUPT_WINDOW_EXITING : 0);
				if(controls != currentProcControls_) {
					vmwrite(PROC_BASED_VM_EXEC_CONTROLS, controls);
					currentProcControls_ = controls;
				}

				// Exit when the local APIC timer expires: through the VMX-preemption timer or
				// (if the CPU lacks it) through a timer interrupt of the host.
				auto timerDeadline = lapic_.timerDeadline();
				if(havePreemptionTimer_) {
					// The CPU does not save the remaining time on exits; hence, we write it
					// before each entry while the timer is armed.
					uint32_t value = UINT32_MAX;
					if(timerDeadline) {
						uint64_t now = getClockNanos();
						uint64_t ticks = nanosToTscTicks(*timerDeadline > now ? *timerDeadline - now : 0);
						// Round up to avoid needless exits before the deadline.
						uint64_t mask = (UINT64_C(1) << preemptionTimerShift_) - 1;
						uint64_t units = (ticks >> preemptionTimerShift_) + ((ticks & mask) ? 1 : 0);
						value = units > UINT32_MAX ? UINT32_MAX : units;
					}
					if(timerDeadline || value != preemptionTimerValue_) {
						vmwrite(GUEST_VMX_PREEMPTION_TIMER, value);
						preemptionTimerValue_ = value;
					}
				}else if(timerDeadline) {
					setGuestDeadline(timerDeadline);
				}

				if(getGlobalCpuFeatures()->haveXsave){
					common::x86::xsave((uint8_t*)hostFstate, ~0);
					common::x86::xrstor((uint8_t*)guestFstate, ~0);
				} else {
					asm volatile ("fxsaveq %0" : : "m" (*hostFstate));
					asm volatile ("fxrstorq %0" : : "m" (*guestFstate));
				}
				// XCR0 is only observable by the guest if it set CR4.OSXSAVE.
				// The FPU state is saved and restored using the (larger) host XCR0.
				bool switchXcr0 = getGlobalCpuFeatures()->haveXsave
						&& guestOsxsave_ && guestXcr0_ != hostXcr0;
				if(switchXcr0)
					common::x86::wrxcr(0, guestXcr0_);

				entryFailed = vmxVmRun(this, &state, launched_);
				if(!entryFailed)
					launched_ = true;

				if(switchXcr0)
					common::x86::wrxcr(0, hostXcr0);
				if(getGlobalCpuFeatures()->haveXsave){
					common::x86::xsave((uint8_t*)guestFstate, ~0);
					common::x86::xrstor((uint8_t*)hostFstate, ~0);
				} else {
					asm volatile ("fxsaveq %0" : : "m" (*guestFstate));
					asm volatile ("fxrstorq %0" : : "m" (*hostFstate));
				}

				//Vm exits don't restore the gdt limit
				common::x86::Gdtr gdtr;
				asm volatile("sgdt %[gdt]": [gdt]"=m"(gdtr));
				gdtr.limit = HOST_GDT_LIMIT;
				asm volatile("lgdt %[gdt]": [gdt]"=m"(gdtr));

				if(!havePreemptionTimer_ && timerDeadline)
					setGuestDeadline(frg::null_opt);

				error = entryFailed ? vmread(VM_INSTRUCTION_ERROR) : 0;
				reason = entryFailed ? ~UINT64_C(0) : vmread(VM_EXIT_REASON);
				if(reason == VMEXIT_EPT_VIOLATION || reason == VMEXIT_IO_INSTRUCTION
						|| reason == VMEXIT_CR_ACCESS) {
					qualification = vmread(EXIT_QUALIFICATION);
					stringIo = reason == VMEXIT_IO_INSTRUCTION && (qualification & (1 << 4));
				}
				if(reason == VMEXIT_EPT_VIOLATION)
					eptAddress = vmread(EPT_VIOLATION_ADDRESS);
				msrExit = reason == VMEXIT_RDMSR || reason == VMEXIT_WRMSR;
				lapicMsr = msrExit && isVirtualLapicMsr(state.rcx & 0xFFFF'FFFF);
				// Instructions that we emulate in the kernel (and skip unless they fault).
				bool skipInstruction = reason == VMEXIT_HLT || reason == VMEXIT_CR_ACCESS
						|| reason == VMEXIT_XSETBV || lapicMsr;
				// Instructions that userspace completes.
				needsCompletion = (msrExit && !lapicMsr) || reason == VMEXIT_CPUID
						|| reason == VMEXIT_IO_INSTRUCTION || reason == VMEXIT_VMCALL;
				if(skipInstruction || needsCompletion)
					instructionLength = vmread(VM_EXIT_INSTRUCTION_LEN);

				// The RIP is needed to skip instructions.
				uint64_t rip = 0;
				if(skipInstruction)
					rip = vmread(GUEST_RIP);

				// The injected event was delivered unless its delivery caused this exit.
				// In the latter case, and also if the guest's own event delivery was interrupted,
				// IDT-vectoring information describes the event, so we inject it again.
				// Exits caused by instructions do not happen during event delivery.
				bool instructionExit = msrExit || reason == VMEXIT_CPUID
						|| reason == VMEXIT_IO_INSTRUCTION || reason == VMEXIT_HLT
						|| reason == VMEXIT_VMCALL || reason == VMEXIT_XSETBV
						|| reason == VMEXIT_CR_ACCESS;
				reinjectValid_ = false;
				if(!entryFailed && !instructionExit) {
					auto vectoring = vmread(IDT_VECTORING_INFO);
					if(vectoring & (UINT32_C(1) << 31)) {
						reinjectValid_ = true;
						reinject_.vector = vectoring & 0xFF;
						reinject_.type = (vectoring >> 8) & 7;
						reinject_.hasErrorCode = (vectoring >> 11) & 1;
						reinject_.errorCode = reinject_.hasErrorCode ? vmread(IDT_VECTORING_ERROR_CODE) : 0;
						// Software interrupts and exceptions need the instruction length.
						reinject_.instructionLength = reinject_.type >= kEventSoftwareInterrupt
								? vmread(VM_EXIT_INSTRUCTION_LEN) : 0;
					}
				}

				// Emulate instructions that we handle in the kernel.
				// If they raise an exception, the guest RIP is not advanced.
				if(reason == VMEXIT_CR_ACCESS)
					skipInstruction = emulateCrAccess(qualification);
				else if(reason == VMEXIT_XSETBV)
					skipInstruction = emulateXsetbv();
				else if(lapicMsr)
					skipInstruction = emulateLapicMsr(state.rcx & 0xFFFF'FFFF, reason == VMEXIT_WRMSR,
							getClockNanos(), lapicEffects, state.rax, state.rdx);

				if(skipInstruction) {
					vmwrite(GUEST_RIP, rip + instructionLength);
					clearInterruptShadow();
				}

				// Completions and EOI exits hand the basic registers to userspace, and run() needs
				// RFLAGS after HLT; caching saves an acquire_()/release_() round-trip in loadRegs().
				bool cacheBasic = !entryFailed && (needsCompletion || reason == VMEXIT_HLT
						|| (lapicMsr && lapicEffects.levelEoi >= 0));
				if(cacheBasic) {
					if(!skipInstruction)
						rip = vmread(GUEST_RIP);
					cachedRip_ = skipInstruction ? rip + instructionLength : rip;
					cachedRsp_ = vmread(GUEST_RSP);
					cachedRflags_ = vmread(GUEST_RFLAG);
					basicCacheValid_ = true;
				}
				space->leaveGuest();
				leaveGuest();
			}

			if(entryFailed) {
				infoLogger() << "vmx: VM entry failed with error " << error << frg::endlog;
				exitInfo.exitReason = kHelVmexitError;
				exitInfo.code = error;
				return exitInfo;
			}

			if(needsCompletion) {
				if(msrExit) {
					return requestMsrCompletion(reason == VMEXIT_WRMSR, state.rax, state.rcx,
							state.rdx, instructionLength);
				} else if(reason == VMEXIT_CPUID) {
					return requestCpuidCompletion(state.rax, state.rcx, instructionLength);
				} else if(reason == VMEXIT_VMCALL) {
					return requestHyperCallCompletion(instructionLength);
				} else {
					return requestIoCompletion(qualification >> 16, (qualification & 7) + 1,
							qualification & (1 << 3), stringIo, state.rax, instructionLength);
				}
			}

			if(reason == VMEXIT_HLT) {
				halted_ = true;
			} else if(reason == VMEXIT_EPT_VIOLATION) {
				// Bit 0 of the qualification is set for reads, bit 1 for writes
				// and bit 2 for instruction fetches.
				uint32_t flags = AddressSpace::kFaultSynchronous;
				if(qualification & (1 << 1))
					flags |= AddressSpace::kFaultWrite;
				if(qualification & (1 << 2))
					flags |= AddressSpace::kFaultExecute;

				leaveResidency();
				auto faultOutcome = Thread::asyncBlockCurrent(
					space->handleFault(eptAddress, flags),
					getCurrentThread()->pagingWorkQueue().get()
				);
				if(!faultOutcome) {
					exitInfo.exitReason = kHelVmexitTranslationFault;
					exitInfo.address = eptAddress;
					exitInfo.flags = translateEptFaultFlags(qualification);
					return exitInfo;
				}
			} else if(reason == VMEXIT_EXTERNAL_INTERRUPT || reason == VMEXIT_INTERRUPT_WINDOW
					|| reason == VMEXIT_PREEMPTION_TIMER) {
				// The interrupt was handled when we re-enabled interrupts. Interrupt windows
				// and the local APIC timer are handled before the next entry.
			} else if(reason == VMEXIT_TRIPLE_FAULT) {
				exitInfo.exitReason = kHelVmexitShutdown;
				return exitInfo;
			} else if(reason == VMEXIT_CR_ACCESS || reason == VMEXIT_XSETBV) {
				// Emulated above.
			} else if(lapicMsr) {
				// Emulated above. The I/O APIC (in userspace) needs to know about EOIs of
				// level-triggered interrupts.
				if(lapicEffects.levelEoi >= 0) {
					exitInfo.exitReason = kHelVmexitEoi;
					exitInfo.code = lapicEffects.levelEoi;
					return exitInfo;
				}
			} else {
				infoLogger() << "vmx: Unknown VMExit code: " << reason << frg::endlog;
				exitInfo.exitReason = kHelVmexitUnknownPlatformSpecificExitCode;
				exitInfo.code = reason;
				return exitInfo;
			}
		}
	}

	namespace {
		bool sameValue(uint64_t a, uint64_t b) { return a == b; }

		// The register structs contain padding, so we compare them member-wise.
		bool sameValue(const HelX86SegmentRegister &a, const HelX86SegmentRegister &b) {
			return a.base == b.base && a.limit == b.limit && a.selector == b.selector
					&& a.type == b.type && a.present == b.present && a.dpl == b.dpl
					&& a.db == b.db && a.s == b.s && a.l == b.l && a.g == b.g
					&& a.avl == b.avl && a.unusable == b.unusable;
		}

		bool sameValue(const HelX86DescriptorTable &a, const HelX86DescriptorTable &b) {
			return a.base == b.base && a.limit == b.limit;
		}
	}

	void Vmcs::storeRegs(const HelX86VirtualizationRegs *regs) {
		// Only write the fields that differ from the VMCS
		bool writeAll = !regsCacheValid_;
		auto differs = [&] (const auto &value, const auto &cached) {
			return writeAll || !sameValue(value, cached);
		};

		// See the constructor.
		StatelessIrqLock irqLock;
		acquire_();

		if(differs(regs->rsp, regsCache_.rsp))
			vmwrite(GUEST_RSP, regs->rsp);
		if(differs(regs->rip, regsCache_.rip))
			vmwrite(GUEST_RIP, regs->rip);
		if(differs(regs->rflags, regsCache_.rflags))
			vmwrite(GUEST_RFLAG, regs->rflags);

		state.rax = regs->rax;
		state.rbx = regs->rbx;
		state.rcx = regs->rcx;
		state.rdx = regs->rdx;
		state.rsi = regs->rsi;
		state.rdi = regs->rdi;
		state.rbp = regs->rbp;
		state.r8 = regs->r8;
		state.r9 = regs->r9;
		state.r10 = regs->r10;
		state.r11 = regs->r11;
		state.r12 = regs->r12;
		state.r13 = regs->r13;
		state.r14 = regs->r14;
		state.r15 = regs->r15;
		state.cr2 = regs->cr2;

		#define SET_SEGMENT(segment, segment_capital) \
            if(differs(regs->segment, regsCache_.segment)) { \
                vmwrite(GUEST_##segment_capital##_BASE, regs->segment.base); \
                vmwrite(GUEST_##segment_capital##_LIMIT, regs->segment.limit); \
                vmwrite(GUEST_##segment_capital##_SELECTOR, regs->segment.selector); \
                uint32_t attrib = regs->segment.type | (regs->segment.s << 4) | \
                                  (regs->segment.dpl << 5) | (regs->segment.present << 7) | \
                                  (regs->segment.avl << 12) | (regs->segment.l << 13) | \
                                  (regs->segment.db << 14) | (regs->segment.g << 15) | \
                                  (regs->segment.unusable << 16); \
                vmwrite(GUEST_##segment_capital##_ACCESS_RIGHT, attrib); \
            }

		SET_SEGMENT(cs, CS);
		SET_SEGMENT(ds, DS);
		SET_SEGMENT(ss, SS);
		SET_SEGMENT(es, ES);
		SET_SEGMENT(fs, FS);
		SET_SEGMENT(gs, GS);

		SET_SEGMENT(ldt, LDTR);
		SET_SEGMENT(tr, TR);

		if(differs(regs->gdt, regsCache_.gdt)) {
			vmwrite(GUEST_GDTR_BASE, regs->gdt.base);
			vmwrite(GUEST_GDTR_LIMIT, regs->gdt.limit);
		}
		if(differs(regs->idt, regsCache_.idt)) {
			vmwrite(GUEST_IDTR_BASE, regs->idt.base);
			vmwrite(GUEST_IDTR_LIMIT, regs->idt.limit);
		}
		if(differs(regs->cr0, regsCache_.cr0)) {
			vmwrite(GUEST_CR0, guestCr0(regs->cr0));
			vmwrite(CR0_READ_SHADOW, regs->cr0);
		}
		if(differs(regs->cr3, regsCache_.cr3))
			vmwrite(GUEST_CR3, regs->cr3);
		if(differs(regs->cr4, regsCache_.cr4)) {
			vmwrite(GUEST_CR4, guestCr4(regs->cr4));
			vmwrite(CR4_READ_SHADOW, regs->cr4);
			guestOsxsave_ = regs->cr4 & CR4_OSXSAVE;
		}
		lapic_.setCr8(regs->cr8);
		// regs->apic_base is ignored: the APIC base is fixed.

		if(differs(regs->efer, regsCache_.efer)) {
			// The CPU updates the IA-32e mode guest control on exits; always write it.
			setLongModeActive(regs->efer & EFER_LMA, regs->efer);
			guestLme_ = regs->efer & EFER_LME;
			updateMsrSwitching();
		}
		release_();

		regsCache_ = *regs;
		regsCacheValid_ = true;
		cachedRsp_ = regs->rsp;
		cachedRip_ = regs->rip;
		cachedRflags_ = regs->rflags;
		basicCacheValid_ = true;
	}

	void Vmcs::loadRegs(HelX86VirtualizationRegs *regs) {
		// Avoid VMREADs if nothing changed since the last load or store.
		// The GPRs and CR2 are not part of the VMCS, so they are not cached.
		if(regsCacheValid_)
			*regs = regsCache_;

		regs->rax = state.rax;
		regs->rbx = state.rbx;
		regs->rcx = state.rcx;
		regs->rdx = state.rdx;
		regs->rsi = state.rsi;
		regs->rdi = state.rdi;
		regs->rbp = state.rbp;
		regs->r8 = state.r8;
		regs->r9 = state.r9;
		regs->r10 = state.r10;
		regs->r11 = state.r11;
		regs->r12 = state.r12;
		regs->r13 = state.r13;
		regs->r14 = state.r14;
		regs->r15 = state.r15;
		regs->cr2 = state.cr2;

		if(regsCacheValid_) {
			regs->cr8 = lapic_.cr8();
			regs->apic_base = VirtualLapic::apicBase();
			return;
		}

		// See storeRegs().
		StatelessIrqLock irqLock;
		acquire_();

		regs->rsp = vmread(GUEST_RSP);
		regs->rip = ripDirty_ ? cachedRip_ : vmread(GUEST_RIP);
		regs->rflags = vmread(GUEST_RFLAG);

		#define GET_SEGMENT(segment, segment_capital) \
            regs->segment.base = vmread(GUEST_##segment_capital##_BASE); \
            regs->segment.limit = vmread(GUEST_##segment_capital##_LIMIT); \
            regs->segment.selector = vmread(GUEST_##segment_capital##_SELECTOR); \
            { \
                auto seg = vmread(GUEST_##segment_capital##_ACCESS_RIGHT); \
                regs->segment.type = seg & 0xF; \
                regs->segment.s = (seg >> 4) & 1; \
                regs->segment.dpl = (seg >> 5) & 3; \
                regs->segment.present = (seg >> 7) & 1; \
                regs->segment.avl = (seg >> 12) & 1; \
                regs->segment.l = (seg >> 13) & 1; \
                regs->segment.db = (seg >> 14) & 1; \
                regs->segment.g = (seg >> 15) & 1; \
                regs->segment.unusable = (seg >> 16) & 1; \
            }

		GET_SEGMENT(cs, CS);
		GET_SEGMENT(ds, DS);
		GET_SEGMENT(ss, SS);
		GET_SEGMENT(es, ES);
		GET_SEGMENT(fs, FS);
		GET_SEGMENT(gs, GS);

		GET_SEGMENT(ldt, LDTR);
		GET_SEGMENT(tr, TR);

		regs->gdt.base = vmread(GUEST_GDTR_BASE);
		regs->gdt.limit = vmread(GUEST_GDTR_LIMIT);

		regs->idt.base = vmread(GUEST_IDTR_BASE);
		regs->idt.limit = vmread(GUEST_IDTR_LIMIT);

		// The guest sees the read shadows for the bits that VMX forces to 1.
		regs->cr0 = guestCr0Value();
		regs->cr3 = vmread(GUEST_CR3);
		regs->cr4 = (vmread(GUEST_CR4) & ~cr4Mask()) | (vmread(CR4_READ_SHADOW) & cr4Mask());
		regs->cr8 = lapic_.cr8();
		regs->apic_base = VirtualLapic::apicBase();

		regs->efer = vmread(VMCS_FIELD_GUEST_EFER_FULL);
		release_();

		regsCache_ = *regs;
		regsCacheValid_ = true;
		cachedRsp_ = regs->rsp;
		cachedRip_ = regs->rip;
		cachedRflags_ = regs->rflags;
		basicCacheValid_ = true;
	}

	void Vmcs::storeBasicRegs(const HelX86VirtualizationBasicRegs *regs) {
		state.rax = regs->rax;
		state.rbx = regs->rbx;
		state.rcx = regs->rcx;
		state.rdx = regs->rdx;
		state.rsi = regs->rsi;
		state.rdi = regs->rdi;
		state.rbp = regs->rbp;
		state.r8 = regs->r8;
		state.r9 = regs->r9;
		state.r10 = regs->r10;
		state.r11 = regs->r11;
		state.r12 = regs->r12;
		state.r13 = regs->r13;
		state.r14 = regs->r14;
		state.r15 = regs->r15;

		bool writeRsp = !basicCacheValid_ || regs->rsp != cachedRsp_;
		bool writeRip = !basicCacheValid_ || regs->rip != cachedRip_;
		bool writeRflags = !basicCacheValid_ || regs->rflags != cachedRflags_;
		if(writeRsp || writeRip || writeRflags) {
			// See the constructor.
			StatelessIrqLock irqLock;
			acquire_();
			if(writeRsp)
				vmwrite(GUEST_RSP, regs->rsp);
			if(writeRip)
				vmwrite(GUEST_RIP, regs->rip);
			if(writeRflags)
				vmwrite(GUEST_RFLAG, regs->rflags);
			release_();
		}

		cachedRsp_ = regs->rsp;
		cachedRip_ = regs->rip;
		cachedRflags_ = regs->rflags;
		basicCacheValid_ = true;
		if(regsCacheValid_) {
			regsCache_.rsp = regs->rsp;
			regsCache_.rip = regs->rip;
			regsCache_.rflags = regs->rflags;
		}
	}

	void Vmcs::loadBasicRegsCache() {
		if(basicCacheValid_)
			return;
		// See the constructor.
		StatelessIrqLock irqLock;
		acquire_();
		cachedRsp_ = vmread(GUEST_RSP);
		cachedRip_ = vmread(GUEST_RIP);
		cachedRflags_ = vmread(GUEST_RFLAG);
		release_();
		basicCacheValid_ = true;
	}

	void Vmcs::loadBasicRegs(HelX86VirtualizationBasicRegs *regs) {
		regs->rax = state.rax;
		regs->rbx = state.rbx;
		regs->rcx = state.rcx;
		regs->rdx = state.rdx;
		regs->rsi = state.rsi;
		regs->rdi = state.rdi;
		regs->rbp = state.rbp;
		regs->r8 = state.r8;
		regs->r9 = state.r9;
		regs->r10 = state.r10;
		regs->r11 = state.r11;
		regs->r12 = state.r12;
		regs->r13 = state.r13;
		regs->r14 = state.r14;
		regs->r15 = state.r15;
		loadBasicRegsCache();
		regs->rsp = cachedRsp_;
		regs->rip = cachedRip_;
		regs->rflags = cachedRflags_;
	}

	bool Vmcs::storeMsrs(const HelX86VirtualizationMsrs *msrs) {
		// Invalid values would make VM entries fail (or fault when we switch XCR0).
		if(!isValidXcr0(msrs->xcr0))
			return false;
		if(!isCanonical(msrs->kernelGsBase) || !isCanonical(msrs->lstar)
				|| !isCanonical(msrs->cstar) || (msrs->sfmask >> 32)
				|| !isCanonical(msrs->sysenterEsp) || !isCanonical(msrs->sysenterEip))
			return false;

		guestMsrs_[0].data = msrs->star;
		guestMsrs_[1].data = msrs->lstar;
		guestMsrs_[2].data = msrs->cstar;
		guestMsrs_[3].data = msrs->sfmask;
		guestMsrs_[kernelGsBaseEntry].data = msrs->kernelGsBase;
		guestXcr0_ = msrs->xcr0;

		// See the constructor.
		StatelessIrqLock irqLock;
		acquire_();
		if(msrs->sysenterCs != sysenterCs_)
			vmwrite(GUEST_IA32_SYSENTER_CS, msrs->sysenterCs);
		if(msrs->sysenterEsp != sysenterEsp_)
			vmwrite(GUEST_IA32_SYSENTER_ESP, msrs->sysenterEsp);
		if(msrs->sysenterEip != sysenterEip_)
			vmwrite(GUEST_IA32_SYSENTER_EIP, msrs->sysenterEip);
		updateMsrSwitching();
		release_();

		sysenterCs_ = msrs->sysenterCs;
		sysenterEsp_ = msrs->sysenterEsp;
		sysenterEip_ = msrs->sysenterEip;
		return true;
	}

	void Vmcs::loadMsrs(HelX86VirtualizationMsrs *msrs) {
		// The guest can only change these MSRs through exits to userspace,
		// except for KERNEL_GS_BASE, which the CPU stores on exits.
		msrs->star = guestMsrs_[0].data;
		msrs->lstar = guestMsrs_[1].data;
		msrs->cstar = guestMsrs_[2].data;
		msrs->sfmask = guestMsrs_[3].data;
		msrs->kernelGsBase = guestMsrs_[kernelGsBaseEntry].data;
		msrs->sysenterCs = sysenterCs_;
		msrs->sysenterEsp = sysenterEsp_;
		msrs->sysenterEip = sysenterEip_;
		msrs->xcr0 = guestXcr0_;
	}

	void Vmcs::updateMsrSwitching() {
		// The guest can only use SWAPGS (which changes KERNEL_GS_BASE) after setting EFER.LME.
		bool used = guestLme_;
		for(unsigned i = 0; i < numSwitchedMsrs; i++)
			used = used || guestMsrs_[i].data;
		unsigned count = used ? numSwitchedMsrs : 0;
		if(count == msrCount_)
			return;
		vmwrite(VM_ENTRY_MSR_LOAD_COUNT, count);
		vmwrite(VM_EXIT_MSR_LOAD_COUNT, count);
		vmwrite(VM_EXIT_MSR_STORE_COUNT, count ? 1 : 0);
		msrCount_ = count;
	}

	uint64_t Vmcs::readGpr(unsigned index) {
		if(index == 4)
			return vmread(GUEST_RSP);
		return *gprSlot(state, index);
	}

	void Vmcs::writeGpr(unsigned index, uint64_t value) {
		if(index == 4) {
			vmwrite(GUEST_RSP, value);
			return;
		}
		*gprSlot(state, index) = value;
	}

	void Vmcs::clearInterruptShadow() {
		auto interruptibility = vmread(GUEST_INTERRUPTIBILITY_STATE);
		if(interruptibility & 3)
			vmwrite(GUEST_INTERRUPTIBILITY_STATE, interruptibility & ~UINT64_C(3));
	}

	uint64_t Vmcs::guestCr0Value() {
		// The guest sees the read shadow for the bits that VMX forces to 1.
		return (vmread(GUEST_CR0) & ~cr0Forced()) | (vmread(CR0_READ_SHADOW) & cr0Forced());
	}

	uint64_t Vmcs::guestEfer() {
		return vmread(VMCS_FIELD_GUEST_EFER_FULL);
	}

	void Vmcs::setLongModeActive(bool active, uint64_t efer) {
		efer = active ? (efer | EFER_LMA) : (efer & ~EFER_LMA);
		vmwrite(VMCS_FIELD_GUEST_EFER_FULL, efer);
		vmwrite(VM_ENTRY_CONTROLS, entryControls_ | (active ? VM_ENTRY_IA32E_MODE : 0));
	}

	bool Vmcs::emulateCrAccess(uint64_t qualification) {
		auto cr = qualification & 0xF;
		auto accessType = (qualification >> 4) & 3;
		auto reg = (qualification >> 8) & 0xF;

		// Outside of long mode, only the low 32 bits of GPRs are used.
		auto longMode = guestEfer() & EFER_LMA;
		auto truncate = [&] (uint64_t value) {
			return longMode ? value : (value & 0xFFFF'FFFF);
		};

		if(accessType == 0) { // MOV to CR.
			auto value = truncate(readGpr(reg));
			if(cr == 0)
				return writeCr0(value);
			if(cr == 3) {
				// Bit 63 only suppresses TLB flushes (with PCIDs).
				vmwrite(GUEST_CR3, value & ~(UINT64_C(1) << 63));
				return true;
			}
			if(cr == 4)
				return writeCr4(value);
			if(cr == 8) {
				if(value & ~UINT64_C(0xF)) {
					queueException(GP_VECTOR, true, 0);
					return false;
				}
				lapic_.setCr8(value);
				return true;
			}
		} else if(accessType == 1) { // MOV from CR.
			if(cr == 3) {
				writeGpr(reg, truncate(vmread(GUEST_CR3)));
				return true;
			}
			if(cr == 8) {
				writeGpr(reg, lapic_.cr8());
				return true;
			}
		} else if(accessType == 2) { // CLTS.
			return writeCr0(guestCr0Value() & ~CR0_TS);
		} else { // LMSW: sets the low 4 bits of CR0 but cannot clear PE.
			auto data = (qualification >> 16) & 0xF;
			return writeCr0((guestCr0Value() & ~UINT64_C(0xE)) | data);
		}

		infoLogger() << "vmx: Unexpected CR access, qualification 0x"
				<< frg::hex_fmt{qualification} << frg::endlog;
		queueException(UD_VECTOR, false, 0);
		return false;
	}

	bool Vmcs::writeCr0(uint64_t value) {
		if((value >> 32) || ((value & CR0_PG) && !(value & CR0_PE))
				|| ((value & CR0_NW) && !(value & CR0_CD))) {
			queueException(GP_VECTOR, true, 0);
			return false;
		}

		// The instruction did not execute, so we need to (de)activate long mode
		// like the CPU would.
		auto old = guestCr0Value();
		auto efer = guestEfer();
		if((value & CR0_PG) && !(old & CR0_PG) && (efer & EFER_LME)) {
			if(!(vmread(GUEST_CR4) & CR4_PAE)) {
				queueException(GP_VECTOR, true, 0);
				return false;
			}
			setLongModeActive(true, efer);
		} else if(!(value & CR0_PG) && (old & CR0_PG) && (efer & EFER_LMA)) {
			setLongModeActive(false, efer);
		} else if((value & CR0_PG) && !(old & CR0_PG) && (vmread(GUEST_CR4) & CR4_PAE)) {
			// TODO: Load the PDPTEs into the VMCS (the CPU does not do that on VM entry).
			infoLogger() << "vmx: PAE paging without long mode is not supported" << frg::endlog;
		}

		vmwrite(GUEST_CR0, guestCr0(value));
		vmwrite(CR0_READ_SHADOW, value);
		return true;
	}

	bool Vmcs::writeCr4(uint64_t value) {
		// We do not support nested virtualization.
		if((value >> 32) || (value & CR4_VMXE)
				|| ((guestEfer() & EFER_LMA) && !(value & CR4_PAE))) {
			queueException(GP_VECTOR, true, 0);
			return false;
		}
		// Without VPIDs, VM entries flush the TLB; hence, we do not need to flush it here.
		vmwrite(GUEST_CR4, guestCr4(value));
		vmwrite(CR4_READ_SHADOW, value);
		guestOsxsave_ = value & CR4_OSXSAVE;
		return true;
	}

	bool Vmcs::emulateXsetbv() {
		if(!guestOsxsave_) {
			queueException(UD_VECTOR, false, 0);
			return false;
		}
		// XSETBV requires CPL 0 (i.e., SS.DPL 0).
		bool cpl0 = !((vmread(GUEST_SS_ACCESS_RIGHT) >> 5) & 3);
		uint32_t index = state.rcx;
		uint64_t value = (state.rdx << 32) | static_cast<uint32_t>(state.rax);
		if(!cpl0 || index != 0 || !isValidXcr0(value)) {
			queueException(GP_VECTOR, true, 0);
			return false;
		}
		guestXcr0_ = value;
		return true;
	}

	void Vmcs::acquire_() {
		auto &current = currentVmcs.get();
		if(current == this)
			return;
		assert(!activeCpu_ || activeCpu_ == getCpuData());
		if(vmptrld((PhysicalAddr)region))
			infoLogger() << "vmx: VMCS load failed" << frg::endlog;
		current = this;
		activeCpu_ = getCpuData();
	}

	void Vmcs::release_() {
		if(resident_ || !activeCpu_)
			return;
		assert(activeCpu_ == getCpuData());
		vmclear((PhysicalAddr)region);
		auto &current = currentVmcs.get();
		if(current == this)
			current = nullptr;
		activeCpu_ = nullptr;
		launched_ = false;
	}

	Vmcs::~Vmcs() {
		assert(!activeCpu_);
		physicalAllocator->free((size_t)region, kPageSize);
		physicalAllocator->free(msrArea_, kPageSize);
	}
}
