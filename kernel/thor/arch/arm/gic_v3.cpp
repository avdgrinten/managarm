#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/cpu-state.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/paging.hpp>

namespace thor {

namespace {

struct LocalInterruptPins {
	frg::array<smarter::shared_ptr<GicPinV3>, 32> pins{};
};

extern PerCpu<LocalInterruptPins> localPins;
THOR_DEFINE_PERCPU(localPins);

} // namespace anonymous

static frg::manual_box<GicDistributorV3> dist;
static frg::manual_box<frg::vector<GicRedistributorV3, KernelAlloc>> redists;
static frg::manual_box<GicV3> gicV3;

static constexpr uint8_t defaultPrio = 0xA0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"

namespace dist_reg {
	static constexpr arch::bit_register<uint32_t> control{0x0};
	static constexpr arch::bit_register<uint32_t> type{0x4};
	static constexpr arch::bit_register<uint32_t> pidr2{0xffe8};

	static constexpr uintptr_t irqGroupBase = 0x80;
	static constexpr uintptr_t irqConfigBase = 0xC00;
	static constexpr uintptr_t irqGroupModBase = 0xD00;
	static constexpr uintptr_t irqSetEnableBase = 0x100;
	static constexpr uintptr_t irqClearEnableBase = 0x180;
	static constexpr uintptr_t irqPriorityBase = 0x400;
	static constexpr uintptr_t irqRouterBase = 0x6100;
}

namespace dist_control {
	static constexpr arch::field<uint32_t, bool> enableGrp0{0, 1};
	static constexpr arch::field<uint32_t, bool> enableGrp1Ns{1, 1};
	static constexpr arch::field<uint32_t, bool> enableGrp1S{2, 1};
	static constexpr arch::field<uint32_t, bool> areS{4, 1};
	static constexpr arch::field<uint32_t, bool> areNs{5, 1};
	static constexpr arch::field<uint32_t, bool> rwp{30, 1};
}

namespace dist_type {
	static constexpr arch::field<uint32_t, uint8_t> irqLines{0, 5};
	static constexpr arch::field<uint32_t, bool> securityExtensions{10, 1};
}

namespace dist_pidr2 {
	static constexpr arch::field<uint32_t, uint8_t> arch{4, 4};
	static constexpr uint8_t arch_gicv3 = 3;
	static constexpr uint8_t arch_gicv4 = 4;
}

namespace dist_router {
	static constexpr arch::field<uint64_t, uint8_t> aff0{0, 8};
	static constexpr arch::field<uint64_t, uint8_t> aff1{8, 8};
	static constexpr arch::field<uint64_t, uint8_t> aff2{16, 8};
	static constexpr arch::field<uint64_t, uint8_t> aff3{32, 8};
}

namespace redist_reg {
	static constexpr arch::bit_register<uint32_t> control{0x0};
	static constexpr arch::bit_register<uint64_t> type{0x8};
	static constexpr arch::bit_register<uint32_t> waker{0x14};
	static constexpr arch::bit_register<uint32_t> pidr2{dist_reg::pidr2};
}

namespace redist_pidr2 = dist_pidr2;

namespace redist_control {
	static constexpr arch::field<uint32_t, bool> rwp{3, 1};
}

namespace redist_waker {
	static constexpr arch::field<uint32_t, bool> processorSleep{1, 1};
	static constexpr arch::field<uint32_t, bool> childrenAsleep{2, 1};
}

namespace redist_type {
	static constexpr arch::field<uint64_t, bool> vlpis{1, 1};
	static constexpr arch::field<uint64_t, bool> last{4, 1};
	static constexpr arch::field<uint64_t, uint32_t> affinity{32, 32};
}

namespace cpu_sre {
	static constexpr arch::field<uint64_t, bool> sre{0, 1};
}

namespace cpu_ctlr {
	static constexpr arch::field<uint64_t, bool> separateDeact{1, 1};
}

namespace cpu_sgi1r {
	static constexpr arch::field<uint64_t, uint16_t> targetList{0, 16};
	static constexpr arch::field<uint64_t, uint8_t> aff1{16, 8};
	static constexpr arch::field<uint64_t, uint8_t> intId{24, 4};
	static constexpr arch::field<uint64_t, uint8_t> aff2{32, 8};
	static constexpr arch::field<uint64_t, bool> irm{40, 1};
	static constexpr arch::field<uint64_t, uint8_t> aff3{48, 8};
}

#pragma GCC diagnostic pop // -Wunused-const-variable

static GicRedistributorV3& getRedistForThisCpu() {
	auto cpuData = getCpuData();
	auto affinity = cpuData->affinity;

	for(auto &redist : *redists) {
		if(redist.ownedBy(affinity))
			return redist;
	}
	panicLogger() << "thor: GIC redistributor was not found for cpu " << cpuData->cpuIndex << " (affinity " << affinity << ")" << frg::endlog;
	__builtin_unreachable();
}

GicDistributorV3::GicDistributorV3(uintptr_t addr, uintptr_t size)
	: base_{addr}, space_{} {

	auto ptr = KernelVirtualMemory::global().allocate(size);
	for(size_t i = 0; i < size; i += kPageSize) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(ptr) + i, addr + i,
				page_access::write, CachingMode::mmio);
	}
	space_ = arch::mem_space{ptr};
}

void GicDistributorV3::init() {
	space_.store_relaxed(dist_reg::control, dist_control::areS(true) | dist_control::areNs(true));

	while(space_.load_relaxed(dist_reg::control) & dist_control::rwp);

	auto control =
		dist_control::enableGrp0(true) | dist_control::enableGrp1Ns(true) | dist_control::enableGrp1S(true) |
		dist_control::areS(true) | dist_control::areNs(true);
	space_.store_relaxed(dist_reg::control, control);
}

frg::string<KernelAlloc> GicDistributorV3::buildPinName(uint32_t irq) {
	return frg::string<KernelAlloc>{*kernelAlloc, "gic@0x"}
			+ frg::to_allocated_string(*kernelAlloc, base_, 16)
			+ frg::string<KernelAlloc>{*kernelAlloc, ":"}
			+ frg::to_allocated_string(*kernelAlloc, irq);
}

GicRedistributorV3::GicRedistributorV3(arch::mem_space space)
	: space_{space} {}

void GicRedistributorV3::initOnThisCpu() {
	auto waker = space_.load_relaxed(redist_reg::waker);
	waker &= ~redist_waker::processorSleep;
	space_.store_relaxed(redist_reg::waker, waker);
	while(space_.load_relaxed(redist_reg::waker) & redist_waker::childrenAsleep);

	arch::scalar_store_relaxed<uint32_t>(space_, 0x10000 + dist_reg::irqGroupBase, ~0);
	arch::scalar_store_relaxed<uint32_t>(space_, 0x10000 + dist_reg::irqGroupModBase, 0);
}

bool GicRedistributorV3::ownedBy(uint32_t affinity) const {
	return (space_.load_relaxed(redist_reg::type) & redist_type::affinity) == affinity;
}

bool GicPinV3::setMode(TriggerMode trigger) {
	if(irq_ < 16)
		return false;
	if(trigger == TriggerMode::null)
		return false;

	auto bitOffset = irq_ % 16 * 2;
	auto offset = irq_ / 16 * 4;

	auto groupOffset = irq_ / 32 * 4;
	auto groupBitOffset = irq_ % 32;

	uint32_t bitValue = trigger == TriggerMode::edge ? 0b10 : 0b00;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;

	auto v = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqConfigBase + offset);
	v &= ~(0b11 << bitOffset);
	v |= bitValue << bitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqConfigBase + offset, v);

	auto group = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqGroupBase + groupOffset);
	group |= 1U << groupBitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqGroupBase + groupOffset, group);

	auto groupMod = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqGroupModBase + groupOffset);
	groupMod &= ~(1U << groupBitOffset);
	arch::scalar_store_relaxed(space, dist_reg::irqGroupModBase + groupOffset, groupMod);

	return true;
}

IrqStrategy GicPinV3::program(TriggerMode mode, Polarity) {
	bool success = setMode(mode);
	assert(success);

	if(irq_ >= 32)
		setAffinity_(getCpuData()->affinity);

	unmask();

	if(mode == TriggerMode::edge) {
		return irq_strategy::maskable | irq_strategy::endOfInterrupt;
	} else {
		assert(mode == TriggerMode::level);
		return irq_strategy::maskable | irq_strategy::maskInService | irq_strategy::endOfInterrupt;
	}
}

void GicPinV3::mask() {
	auto bit = irq_ % 32;
	auto offset = irq_ / 32 * 4;

	// The disable only takes effect once RWP clears (Linux: gic_poke_irq()); until then, a
	// still-asserted level-triggered IRQ is delivered again after the EOI that follows.
	if(irq_ < 32) {
		auto &redist = getRedistForThisCpu();
		auto sgiSpace = redist.space_.subspace(0x10000);
		arch::scalar_store_relaxed(sgiSpace, dist_reg::irqClearEnableBase + offset, 1U << bit);
		while(redist.space_.load_relaxed(redist_reg::control) & redist_control::rwp)
			;
	} else {
		arch::scalar_store_relaxed(dist->space_, dist_reg::irqClearEnableBase + offset, 1U << bit);
		while(dist->space_.load_relaxed(dist_reg::control) & dist_control::rwp)
			;
	}
}

void GicPinV3::unmask() {
	auto bit = irq_ % 32;
	auto offset = irq_ / 32 * 4;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;
	arch::scalar_store_relaxed(space, dist_reg::irqSetEnableBase + offset, 1U << bit);
}

void GicPinV3::endOfInterrupt() {
	gicV3->eoi(0, irq_);
}

void GicPinV3::setAffinity_(uint32_t affinity) {
	if(irq_ < 32)
		return;

	auto offset = (irq_ - 32) * 8;

	arch::bit_value<uint64_t> v =
		dist_router::aff0(affinity) |
		dist_router::aff1(affinity >> 8) |
		dist_router::aff2(affinity >> 16) |
		dist_router::aff3(affinity >> 24);

	arch::scalar_store_relaxed(dist->space_, dist_reg::irqRouterBase + offset, static_cast<uint64_t>(v));
}

void GicPinV3::setPriority_(uint8_t priority) {
	auto offset = irq_ / 4 * 4;
	auto bitOffset = irq_ % 4 * 8;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;

	auto value = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqPriorityBase + offset);
	value &= ~(0xFF << bitOffset);
	value |= static_cast<uint32_t>(priority) << bitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqPriorityBase + offset, value);
}

void addRedistRange(uintptr_t address, size_t size) {
	auto redistPtr = KernelVirtualMemory::global().allocate(size);
	for(size_t i = 0; i < size; i += kPageSize) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(redistPtr) + i, address + i,
				page_access::write, CachingMode::mmio);
	}

	auto redistCount = size / 0x20000;
	size_t offset = 0;

	for(size_t i = 0; i < redistCount; ++i) {
		arch::mem_space space{(void *)(VirtualAddr(redistPtr) + offset)};

		// Check if we have a redistributor.
		auto pidr2 = space.load_relaxed(redist_reg::pidr2);
		uint8_t arch = pidr2 & redist_pidr2::arch;
		if(arch != redist_pidr2::arch_gicv3 && arch != redist_pidr2::arch_gicv4) {
			warningLogger() << "No redistributor present" << frg::endlog;
			break;
		}

		auto type = space.load_relaxed(redist_reg::type);
		uint32_t redistAff = type & redist_type::affinity;
		bool vlpis = type & redist_type::vlpis;
		bool last = type & redist_type::last;

		infoLogger() << "thor: GIC redistributor at " << frg::hex_fmt{address + offset}
		             << " affinity " << frg::hex_fmt{redistAff}
		             << (vlpis ? " vlpi" : "")
		             << (last ? " last" : "") << frg::endlog;

		redists->emplace_back(space);

		if(last)
			break;

		// Skip RD_base and SGI_base.
		offset += 0x20000;

		// If VLPIS is present, skip the VLPI_base and reserved space.
		if(vlpis)
			offset += 0x20000;

		if(offset >= size)
			break;
	}
}

bool initGicV3() {
	auto root = getDeviceTreeRoot();
	if (!root)
		return false;

	DeviceTreeNode *gicNode = nullptr;
	root->forEach([&](DeviceTreeNode *node) -> bool {
		if(node->isCompatible(dtGicV3Compatible)) {
			gicNode = node;
			return true;
		}

		return false;
	});

	if(!gicNode)
		return false;

	infoLogger() << "thor: found the GIC at node \"" << gicNode->path() << "\"" << frg::endlog;
	assert(gicNode->reg().size() >= 2);

	auto reg = gicNode->reg();
	dist.initialize(reg[0].addr, reg[0].size);

	redists.initialize(*kernelAlloc);
	addRedistRange(reg[1].addr, reg[1].size);

	dist->init();

	gicV3.initialize();

	externalIrq = gicV3.get();

	gicNode->associateIrqController(gicV3.get());

	return true;
}

bool initGicV3FromAcpi(
    uintptr_t distributor,
    size_t distributorSize,
    frg::span<const GicRedistributorRange> redistributorRanges
) {
	infoLogger() << "thor: found ACPI GICv3 distributor at " << frg::hex_fmt{distributor}
	             << frg::endlog;

	dist.initialize(distributor, distributorSize);

	redists.initialize(*kernelAlloc);
	for (auto range : redistributorRanges)
		addRedistRange(range.address, range.size);

	if (!redists->size()) {
		warningLogger() << "thor: ACPI GICv3 has no redistributor ranges" << frg::endlog;
		return false;
	}

	dist->init();

	gicV3.initialize();
	externalIrq = gicV3.get();

	return true;
}

void initGicOnThisCpuV3() {
	getRedistForThisCpu().initOnThisCpu();

	// The system register interface of the EL that we run in is enabled by the
	// ICC_SRE register of that EL; ICC_SRE_EL1 is not redirected by E2H.
	arch::bit_value<uint64_t> sre{0};
	if (isKernelInEl2()) {
		asm volatile("mrs %0, icc_sre_el2" : "=r"(sre));
		sre |= cpu_sre::sre(true);
		asm volatile("msr icc_sre_el2, %0" : : "r"(sre));
	} else {
		asm volatile("mrs %0, icc_sre_el1" : "=r"(sre));
		sre |= cpu_sre::sre(true);
		asm volatile("msr icc_sre_el1, %0" : : "r"(sre));
	}
	asm volatile("isb");

	arch::bit_value<uint64_t> ctlr{0};
	asm volatile("mrs %0, icc_ctlr_el1" : "=r"(ctlr));
	ctlr |= cpu_ctlr::separateDeact(true);
	asm volatile("msr icc_ctlr_el1, %0" : : "r"(ctlr));

	uint64_t priority = 0xFF;
	asm volatile("msr icc_pmr_el1, %0" : : "r"(priority));

	// No pre-emption
	uint64_t bpr = 0b111;
	asm volatile("msr icc_bpr1_el1, %0" : : "r"(bpr));

	uint64_t igrpen1;
	asm volatile("mrs %0, icc_igrpen1_el1" : "=r"(igrpen1));
	igrpen1 |= 1;
	asm volatile("msr icc_igrpen1_el1, %0" : : "r"(igrpen1));

	auto *pins = &localPins.get();

	for(int i = 0; i < 32; ++i) {
		auto pin = createIrqPin<GicPinV3>(dist.get(), i);
		pins->pins[i] = pin;

		pin->mask();
		pin->setPriority_(defaultPrio);
		if(i < 16)
			pin->unmask();
	}
}

GicV3::GicV3() : irqPins_{*kernelAlloc} {
	auto affinity = getCpuData()->affinity;

	auto type = dist->space_.load_relaxed(dist_reg::type);
	uint32_t irqLines = type & dist_type::irqLines;
	auto securityExtensions = type & dist_type::securityExtensions;

	// TODO: there can be more extension pins
	auto pins = frg::min<uint32_t>(32 * (irqLines + 1), 1020);

	infoLogger() << "GIC Distributor has " << pins << " IRQs and "
		<< (securityExtensions ? "supports" : "doesn't support") << " security extensions" << frg::endlog;

	irqPins_.resize(pins);
	for(uint32_t i = 32; i < pins; ++i) {
		irqPins_[i] = createIrqPin<GicPinV3>(dist.get(), i);

		irqPins_[i]->mask();
		irqPins_[i]->setPriority_(defaultPrio);
		irqPins_[i]->setAffinity_(affinity);
	}
}

void GicV3::sendSgi_(uint32_t affinity, uint16_t targetList, uint8_t id) {
	uint8_t aff1 = affinity >> 8;
	uint8_t aff2 = affinity >> 16;
	uint8_t aff3 = affinity >> 24;

	arch::bit_value<uint64_t> v =
		cpu_sgi1r::targetList(targetList) |
		cpu_sgi1r::aff1(aff1) |
		cpu_sgi1r::aff2(aff2) |
		cpu_sgi1r::aff3(aff3) |
		cpu_sgi1r::intId(id);
	// Unlike memory accesses, the SGI register write is only ordered after prior stores by a DSB.
	asm volatile("dsb ishst; msr icc_sgi1r_el1, %0; isb" : : "r"(v) : "memory");
}

void GicV3::sendIpi(int cpuId, uint8_t id) {
	auto affinity = getCpuData(cpuId)->affinity;
	uint8_t aff0 = affinity;
	assert(aff0 < 16);
	sendSgi_(affinity, 1U << aff0, id);
}

size_t GicV3::sendIpi(const frg::dyn_bitset<KernelAlloc> &targets, uint8_t id) {
	// One SGI register write reaches up to 16 PEs that share affinity levels 1 to 3.
	uint32_t cluster = 0;
	uint16_t targetList = 0;
	size_t numTargets = 0;
	auto flush = [&] {
		if (!targetList)
			return;
		sendSgi_(cluster, targetList, id);
		targetList = 0;
	};
	for (auto cpu : targets.set_bits()) {
		auto *dstData = getCpuData(cpu);
		if (suppressIpiToOfflineCpu(dstData))
			continue;
		auto affinity = dstData->affinity;
		uint8_t aff0 = affinity;
		assert(aff0 < 16);
		if (targetList && (affinity & ~UINT32_C(0xFF)) != cluster)
			flush();
		cluster = affinity & ~UINT32_C(0xFF);
		targetList |= 1U << aff0;
		++numTargets;
	}
	flush();
	return numTargets;
}

size_t GicV3::sendIpiToOthers(uint8_t id) {
	// The all-excluding-self icc_sgi1r_el1 bit (IRM) is not always implemented correctly when running under a hypervisor
	// which is why it's not used here.
	// An example of this is the QCM6490 platform and probably other Qualcomm platforms where an old version of the Gunyah hypervisor is used
	// which ignores icc_sgi1r_el1 writes that have IRM set.
	size_t self = getCpuData()->cpuIndex;
	size_t numTargets = 0;
	for (size_t i = 0; i < getCpuCount(); ++i) {
		if (i == self)
			continue;
		if (suppressIpiToOfflineCpu(getCpuData(i)))
			continue;
		sendIpi(static_cast<int>(i), id);
		++numTargets;
	}
	return numTargets;
}

Gic::CpuIrq GicV3::getIrq() {
	uint64_t iar1;
	asm volatile("mrs %0, icc_iar1_el1" : "=r"(iar1));
	uint32_t irq = iar1 & 0xFFFFFF;

	if(irq < 1020)
		asm volatile("msr icc_eoir1_el1, %0" : : "r"(uint64_t {irq}));

	return {0, irq};
}

void GicV3::eoi(uint32_t, uint32_t id) {
	asm volatile("msr icc_dir_el1, %0" : : "r"(uint64_t {id}));
}

smarter::shared_ptr<Gic::Pin> GicV3::setupIrq(uint32_t irq, TriggerMode trigger) {
	auto pin = getPin(irq);
	if (!pin)
		return nullptr;

	pin->configure({trigger, Polarity::null});

	return pin;
}

smarter::shared_ptr<Gic::Pin> GicV3::getPin(uint32_t irq) {
	if (irq < 32)
		return localPins.get().pins[irq];
	if(irq >= irqPins_.size())
		return nullptr;

	return irqPins_[irq];
}

uint32_t GicV3::irqCount() {
	return irqPins_.size();
}

}
