
#include <assert.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <unordered_map>
#include <optional>
#include <print>

#include <core/process-data.hpp>
#include <core/virtio/core.hpp>
#include <fafnir/dsl.hpp>
#include <protocols/kernlet/compiler.hpp>

namespace virtio_core {

struct Mapping {
	static constexpr size_t pageSize = 0x1000;

	friend void swap(Mapping &x, Mapping &y) {
		using std::swap;
		swap(x._memory, y._memory);
		swap(x._window, y._window);
		swap(x._offset, y._offset);
		swap(x._size, y._size);
	}

	Mapping()
	: _window{nullptr}, _offset{0}, _size{0} { }

	Mapping(helix::UniqueDescriptor memory, ptrdiff_t offset, size_t size)
	: _memory{std::move(memory)}, _offset{offset}, _size{size} {
		HEL_CHECK(helMapMemory(_memory.getHandle(), kHelNullHandle,
				nullptr, _offset & ~(pageSize - 1),
				((_offset & (pageSize - 1)) + _size + (pageSize - 1)) & ~(pageSize - 1),
				kHelMapProtRead | kHelMapProtWrite, &_window));
	}

	Mapping(const Mapping &) = delete;

	Mapping(Mapping &&other)
	: Mapping() {
		swap(*this, other);
	}

	~Mapping() {
		if(_window)
			assert(!"Unmap memory here!");
	}

	Mapping &operator= (Mapping other) {
		swap(*this, other);
		return *this;
	}

	helix::BorrowedDescriptor memory() {
		return _memory;
	}
	ptrdiff_t offset() {
		return _offset;
	}

	void *get() {
		return reinterpret_cast<char *>(_window) + (_offset & (pageSize - 1));
	}

private:
	helix::UniqueDescriptor _memory;
	void *_window;
	ptrdiff_t _offset;
	size_t _size;
};

// --------------------------------------------------------
// LegacyPciTransport
// --------------------------------------------------------

#ifdef __x86_64__

namespace {

struct LegacyPciQueue;

struct LegacyPciTransport : Transport {
	friend struct LegacyPciQueue;

	LegacyPciTransport(protocols::hw::Device hw_device,
			arch::io_space legacy_space, helix::UniqueDescriptor bar,
			helix::UniqueDescriptor irq, helix::UniqueDescriptor dmaSpace);

	bool isLegacy() override {
		return true;
	}

	protocols::hw::Device &hwDevice() override {
		return _hwDevice;
	}

	unsigned int maxIrqsAvailableToQueues() override {
		return 1;
	}

	uint8_t loadConfig8(size_t offset) override;
	uint16_t loadConfig16(size_t offset) override;
	uint32_t loadConfig32(size_t offset) override;

	bool checkDeviceFeature(unsigned int feature) override;
	void acknowledgeDriverFeature(unsigned int feature) override;
	void finalizeFeatures() override;

	using Transport::claimQueues;
	void claimQueues(std::span<const QueueIrq> irqs) override;
	async::result<Queue *> setupQueue(unsigned int index) override;

	void runDevice() override;

private:
	// Returns the I/O space, after enabling it on the current thread if necessary.
	arch::io_space _io();

	async::detached _processIrqs();

	protocols::hw::Device _hwDevice;
	arch::io_space _legacySpace;
	helix::UniqueDescriptor _bar;
	helix::UniqueDescriptor _irq;
	// Identifies this device within the per-thread table of enabled devices.
	unsigned int _ioIndex;

	std::vector<std::unique_ptr<LegacyPciQueue>> _queues;
};

struct LegacyPciQueue final : Queue {
	LegacyPciQueue(LegacyPciTransport *transport,
			unsigned int queue_index, size_t queue_size,
			spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used);

	// The legacy transport has no MSIs, so every queue is serviced by the IRQ loop.
	async::result<void> serviceQueue() override {
		co_return;
	}

protected:
	void notifyTransport() override;

	arch::dma_space &dmaSpace() override {
		return _transport->dmaSpace_;
	}

private:
	LegacyPciTransport *_transport;
};

std::atomic<unsigned int> nextIoIndex{0};

LegacyPciTransport::LegacyPciTransport(
    protocols::hw::Device hw_device, arch::io_space legacy_space, helix::UniqueDescriptor bar,
    helix::UniqueDescriptor irq, helix::UniqueDescriptor dmaSpace
)
: Transport(std::move(dmaSpace), false),
  _hwDevice{std::move(hw_device)},
  _legacySpace{legacy_space},
  _bar{std::move(bar)},
  _irq{std::move(irq)},
  _ioIndex{nextIoIndex.fetch_add(1, std::memory_order_relaxed)} {}

// Thor enables port I/O per-thread, hence every thread has to call helEnableIo() itself.
arch::io_space LegacyPciTransport::_io() {
	thread_local std::vector<bool> enabled;

	if(enabled.size() <= _ioIndex)
		enabled.resize(_ioIndex + 1);
	if(!enabled[_ioIndex]) {
		HEL_CHECK(helEnableIo(_bar.getHandle()));
		enabled[_ioIndex] = true;
	}
	return _legacySpace;
}

uint8_t LegacyPciTransport::loadConfig8(size_t offset) {
	return _io().subspace(20).load(arch::scalar_register<uint8_t>(offset));
}

uint16_t LegacyPciTransport::loadConfig16(size_t offset) {
	return _io().subspace(20).load(arch::scalar_register<uint16_t>(offset));
}

uint32_t LegacyPciTransport::loadConfig32(size_t offset) {
	return _io().subspace(20).load(arch::scalar_register<uint32_t>(offset));
}

bool LegacyPciTransport::checkDeviceFeature(unsigned int feature) {
	if(feature >= 32) {
		std::cout << "core-virtio: Feature index " << feature << " cannot be queried"
				" on legacy device" << std::endl;
		return false;
	}
	return _io().load(PCI_L_DEVICE_FEATURES) & (1 << feature);
}

void LegacyPciTransport::acknowledgeDriverFeature(unsigned int feature) {
	assert(feature < 32);
	auto current = _io().load(PCI_L_DRIVER_FEATURES);
	_io().store(PCI_L_DRIVER_FEATURES, current | (1 << feature));
}

void LegacyPciTransport::finalizeFeatures() {
	// Does nothing for now.
}

void LegacyPciTransport::claimQueues(std::span<const QueueIrq> irqs) {
	_queues.resize(irqs.size());
}

async::result<Queue *> LegacyPciTransport::setupQueue(unsigned int queue_index) {
	assert(queue_index < _queues.size());
	assert(!_queues[queue_index]);

	_io().store(PCI_L_QUEUE_SELECT, queue_index);
	auto queue_size = _io().load(PCI_L_QUEUE_SIZE);
	assert(queue_size);

	// TODO: Ensure that the queue size is indeed a power of 2.

	// Determine the queue size in bytes.
	constexpr size_t available_align = 2;
	constexpr size_t used_align = 4096;

	auto available_offset = (queue_size * sizeof(spec::Descriptor)
				+ (available_align - 1))
			& ~size_t(available_align - 1);
	auto used_offset = (available_offset + sizeof(spec::AvailableRing)
				+ queue_size * sizeof(spec::AvailableRing::Element)
				+ sizeof(spec::AvailableExtra) + (used_align - 1))
			& ~size_t(used_align - 1);

	auto region_size = used_offset + sizeof(spec::UsedRing)
				+ queue_size * sizeof(spec::UsedRing::Element)
				+ sizeof(spec::UsedExtra);

	// Allocate physical memory for the virtq structs.
	assert(region_size < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 0x4000, kHelAllocContinuous, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));

	// Setup the memory region.
	auto table = reinterpret_cast<spec::Descriptor *>((char *)window);
	auto available = reinterpret_cast<spec::AvailableRing *>((char *)window + available_offset);
	auto used = reinterpret_cast<spec::UsedRing *>((char *)window + used_offset);
	_queues[queue_index] = std::make_unique<LegacyPciQueue>(this, queue_index, queue_size,
			table, available, used);

	// Hand the queue to the device.
	uintptr_t table_physical;
	HEL_CHECK(helPointerPhysical(kHelNullHandle, table, &table_physical));
	_io().store(PCI_L_QUEUE_ADDRESS, table_physical >> 12);

	co_return _queues[queue_index].get();
}

void LegacyPciTransport::runDevice() {
	// Set the DRIVER_OK bit to finish the configuration.
	_io().store(PCI_L_DEVICE_STATUS, _io().load(PCI_L_DEVICE_STATUS) | DRIVER_OK);

	_processIrqs();
}

async::detached LegacyPciTransport::_processIrqs() {
	co_await _hwDevice.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto isr = _io().load(PCI_L_ISR_STATUS);

		if(!(isr & 3)) {
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		if(isr & 2) {
			std::cout << "core-virtio: Configuration change" << std::endl;
			auto status = _io().load(PCI_L_DEVICE_STATUS);
			assert(!(status & DEVICE_NEEDS_RESET));
		}
		if(isr & 1)
			for(auto &queue : _queues)
				queue->processInterrupt();
	}
}

LegacyPciQueue::LegacyPciQueue(LegacyPciTransport *transport,
		unsigned int queue_index, size_t queue_size,
		spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used)
: Queue{queue_index, queue_size, {}, table, available, used}, _transport{transport} { }

void LegacyPciQueue::notifyTransport() {
	_transport->_io().store(PCI_L_QUEUE_NOTIFY, queueIndex());
}

} // anonymous namespace

#endif

// --------------------------------------------------------
// StandardPciTransport
// --------------------------------------------------------

namespace {

// Reserve the first vector for configMsiVector.
// The MSI vector allocator only allocates MSI vectors above numReservedMsis.
constexpr unsigned int numReservedMsis = 1;

// MSI vector that the device raises when its configuration changes.
constexpr unsigned int configMsiVector = 0;

struct StandardPciQueue;

struct StandardPciTransport : Transport {
	friend struct StandardPciQueue;

	StandardPciTransport(protocols::hw::Device hw_device,
			unsigned int numMsis,
			Mapping common_mapping, Mapping notify_mapping,
			Mapping isr_mapping, Mapping device_mapping,
			unsigned int notify_multiplier, helix::UniqueDescriptor irq,
			helix::UniqueDescriptor configMsi, helix::UniqueDescriptor dmaSpace,
			bool iommuActive);

	bool isLegacy() override {
		return false;
	}

	protocols::hw::Device &hwDevice() override {
		return _hwDevice;
	}

	unsigned int maxIrqsAvailableToQueues() override {
		if(!_numMsis)
			return 1;
		return _numMsis - numReservedMsis;
	}

	uint8_t loadConfig8(size_t offset) override;
	uint16_t loadConfig16(size_t offset) override;
	uint32_t loadConfig32(size_t offset) override;

	bool checkDeviceFeature(unsigned int feature) override;
	void acknowledgeDriverFeature(unsigned int feature) override;
	void finalizeFeatures() override;

	using Transport::claimQueues;
	void claimQueues(std::span<const QueueIrq> irqs) override;
	async::result<Queue *> setupQueue(unsigned int index) override;

	void runDevice() override;

	// Backs StandardPciQueue::serviceQueue().
	async::result<void> serviceVector(unsigned int queue_index);

private:
	arch::mem_space _commonSpace() { return arch::mem_space{_commonMapping.get()}; }
	arch::mem_space _notifySpace() { return arch::mem_space{_notifyMapping.get()}; }
	arch::mem_space _isrSpace() { return arch::mem_space{_isrMapping.get()}; }
	arch::mem_space _deviceSpace() { return arch::mem_space{_deviceMapping.get()}; }

	async::detached _processIrqs();
	async::detached _processConfigMsi();

	protocols::hw::Device _hwDevice;
	// Number of MSI vectors that the device offers. Zero if MSIs are unavailable.
	unsigned int _numMsis;
	Mapping _commonMapping;
	Mapping _notifyMapping;
	Mapping _isrMapping;
	Mapping _deviceMapping;
	unsigned int _notifyMultiplier;
	helix::UniqueDescriptor _irq;
	helix::UniqueDescriptor _configMsi;

	// MSI vectors that the device raises for virtq completions, indexed by vector.
	std::vector<helix::UniqueDescriptor> _msis;
	// Maps MSI vectors to one or more queues.
	std::vector<std::vector<StandardPciQueue *>> _msiQueues;
	// Whether a servicer was already started for each vector.
	std::unique_ptr<std::atomic_flag[]> _msiServiced;
	// MSI vector of each virtq, or VIRTIO_MSI_NO_VECTOR.
	std::vector<unsigned int> _queueVectors;

	std::vector<std::unique_ptr<StandardPciQueue>> _queues;
};

struct StandardPciQueue final : Queue {
	StandardPciQueue(
	    StandardPciTransport *transport,
	    unsigned int queue_index,
	    size_t queue_size,
	    arch::dma_buffer virtq,
	    arch::dma_object_view<spec::Descriptor> table,
	    arch::dma_object_view<spec::AvailableRing> available,
	    arch::dma_object_view<spec::UsedRing> used,
	    arch::scalar_register<uint16_t> notify_register
	);

	arch::dma_space &dmaSpace() override {
		return _transport->dmaSpace_;
	}

	async::result<void> serviceQueue() override {
		return _transport->serviceVector(queueIndex());
	}

protected:
	void notifyTransport() override;

private:
	StandardPciTransport *_transport;
	arch::scalar_register<uint16_t> _notifyRegister;
};

StandardPciTransport::StandardPciTransport(
    protocols::hw::Device hw_device,
    unsigned int numMsis,
    Mapping common_mapping,
    Mapping notify_mapping,
    Mapping isr_mapping,
    Mapping device_mapping,
    unsigned int notify_multiplier,
    helix::UniqueDescriptor irq,
    helix::UniqueDescriptor configMsi,
    helix::UniqueDescriptor dmaSpace,
    bool iommuActive
)
: Transport(std::move(dmaSpace), iommuActive),
  _hwDevice{std::move(hw_device)},
  _numMsis{numMsis},
  _commonMapping{std::move(common_mapping)},
  _notifyMapping{std::move(notify_mapping)},
  _isrMapping{std::move(isr_mapping)},
  _deviceMapping{std::move(device_mapping)},
  _notifyMultiplier{notify_multiplier},
  _irq{std::move(irq)},
  _configMsi{std::move(configMsi)} {}

uint8_t StandardPciTransport::loadConfig8(size_t offset) {
	return _deviceSpace().load(arch::scalar_register<uint8_t>(offset));
}

uint16_t StandardPciTransport::loadConfig16(size_t offset) {
	return _deviceSpace().load(arch::scalar_register<uint16_t>(offset));
}

uint32_t StandardPciTransport::loadConfig32(size_t offset) {
	return _deviceSpace().load(arch::scalar_register<uint32_t>(offset));
}

bool StandardPciTransport::checkDeviceFeature(unsigned int feature) {
	_commonSpace().store(PCI_DEVICE_FEATURE_SELECT, feature >> 5);
	return _commonSpace().load(PCI_DEVICE_FEATURE_WINDOW) & (uint32_t(1) << (feature & 31));
}

void StandardPciTransport::acknowledgeDriverFeature(unsigned int feature) {
	auto bit = uint32_t(1) << (feature & 31);
	_commonSpace().store(PCI_DRIVER_FEATURE_SELECT, feature >> 5);
	auto current = _commonSpace().load(PCI_DRIVER_FEATURE_WINDOW);
	_commonSpace().store(PCI_DRIVER_FEATURE_WINDOW, current | bit);
}

void StandardPciTransport::finalizeFeatures() {
	if (dmaSpace_.iommuActive()) {
		auto iommuCap = checkDeviceFeature(33);
		if (!iommuCap)
			throw std::runtime_error("virtio device does not support IOMMU");
		acknowledgeDriverFeature(33);
	}

	assert(checkDeviceFeature(32));
	acknowledgeDriverFeature(32);

	_commonSpace().store(PCI_DEVICE_STATUS, _commonSpace().load(PCI_DEVICE_STATUS) | FEATURES_OK);
	auto confirm = _commonSpace().load(PCI_DEVICE_STATUS);
	assert(confirm & FEATURES_OK);
}

void StandardPciTransport::claimQueues(std::span<const QueueIrq> irqs) {
	_queues.resize(irqs.size());
	_queueVectors.assign(irqs.size(), VIRTIO_MSI_NO_VECTOR);
	if(!_numMsis)
		return;

	// Assign all MSI vectors.
	// MSI-X suppresses the device's pin, so every queue needs a vector of some kind.
	auto budget = _numMsis - numReservedMsis;
	size_t numOwn = std::ranges::count(irqs, QueueIrq::own);
	bool needShared = std::ranges::contains(irqs, QueueIrq::shared) || numOwn > budget;
	numOwn = std::min<size_t>(numOwn, budget - (needShared ? 1 : 0));

	unsigned int v = numReservedMsis; // Next vector to allocate.
	auto sharedVector = needShared ? v++ : VIRTIO_MSI_NO_VECTOR;
	for(size_t i = 0; i < irqs.size(); ++i) {
		if(irqs[i] == QueueIrq::own && numOwn) {
			_queueVectors[i] = v++;
			--numOwn;
		}else{
			_queueVectors[i] = sharedVector;
		}
	}
	assert(v <= _numMsis);

	_msis.resize(v);
	_msiQueues.resize(v);
	_msiServiced = std::make_unique<std::atomic_flag[]>(v);
}

async::result<Queue *> StandardPciTransport::setupQueue(unsigned int queue_index) {
	assert(queue_index < _queues.size());
	assert(!_queues[queue_index]);

	_commonSpace().store(PCI_QUEUE_SELECT, queue_index);
	auto queue_size = _commonSpace().load(PCI_QUEUE_SIZE);
	auto notify_index = _commonSpace().load(PCI_QUEUE_NOTIFY);
	assert(queue_size);
	assert(std::has_single_bit(queue_size));

	// Determine the queue size in bytes.
	constexpr size_t available_align = 2;
	constexpr size_t used_align = 4;

	auto available_offset = (queue_size * sizeof(spec::Descriptor)
				+ (available_align - 1))
			& ~size_t(available_align - 1);
	auto used_offset = (available_offset + sizeof(spec::AvailableRing)
				+ queue_size * sizeof(spec::AvailableRing::Element)
				+ sizeof(spec::AvailableExtra) + (used_align - 1))
			& ~size_t(used_align - 1);

	auto region_size = used_offset + sizeof(spec::UsedRing)
				+ queue_size * sizeof(spec::UsedRing::Element)
				+ sizeof(spec::UsedExtra);
	region_size = (region_size + 0xFFF) & ~0xFFF;

	arch::dma_buffer virtq{&contiguousPool_, region_size};
	arch::dma_object_view<spec::Descriptor> table{virtq.get_dma_ptr()};
	arch::dma_object_view<spec::AvailableRing> available{virtq.get_dma_ptr().offset_by(available_offset)};
	arch::dma_object_view<spec::UsedRing> used{virtq.get_dma_ptr().offset_by(used_offset)};

	_queues[queue_index] = std::make_unique<StandardPciQueue>(
	    this,
	    queue_index,
	    queue_size,
	    std::move(virtq),
	    table,
	    available,
	    used,
	    arch::scalar_register<uint16_t>{_notifyMultiplier * notify_index}
	);

	// Hand the queue to the device.
	co_await dmaSpace_.ensure_mapped(table);
	auto table_physical = dmaSpace_.iova_of(table);
	auto available_physical = dmaSpace_.iova_of(available);
	auto used_physical = dmaSpace_.iova_of(used);
	_commonSpace().store(PCI_QUEUE_TABLE[0], table_physical);
	_commonSpace().store(PCI_QUEUE_TABLE[1], table_physical >> 32);
	_commonSpace().store(PCI_QUEUE_AVAILABLE[0], available_physical);
	_commonSpace().store(PCI_QUEUE_AVAILABLE[1], available_physical >> 32);
	_commonSpace().store(PCI_QUEUE_USED[0], used_physical);
	_commonSpace().store(PCI_QUEUE_USED[1], used_physical >> 32);

	auto vector = _queueVectors[queue_index];
	if(vector != VIRTIO_MSI_NO_VECTOR) {
		if(!_msis[vector])
			_msis[vector] = co_await _hwDevice.installMsi(vector);
		_msiQueues[vector].push_back(_queues[queue_index].get());
	}

	// Devices without MSI-X do not implement the vector register.
	if(_numMsis) {
		_commonSpace().store(PCI_QUEUE_MSIX_VECTOR, vector);
		if(_commonSpace().load(PCI_QUEUE_MSIX_VECTOR) != vector)
			throw std::runtime_error("Device failed to allocate MSI-X interrupt");
	}

	_commonSpace().store(PCI_QUEUE_ENABLE, 1);

	co_return _queues[queue_index].get();
}

void StandardPciTransport::runDevice() {
	if(_numMsis) {
		_commonSpace().store(PCI_CONFIG_MSIX_VECTOR, configMsiVector);
		if(_commonSpace().load(PCI_CONFIG_MSIX_VECTOR) != configMsiVector)
			throw std::runtime_error("Device failed to allocate MSI-X interrupt");
	}

	// Finally set the DRIVER_OK bit to finish the configuration.
	_commonSpace().store(PCI_DEVICE_STATUS, _commonSpace().load(PCI_DEVICE_STATUS) | DRIVER_OK);

	// The pin can be shared with other devices, so it always needs to be acked or nacked.
	// MSI-X only suppresses this device's own use of it: the ISR register then stays clear
	// and the IRQ loop does nothing but nack.
	_processIrqs();
	if(_numMsis)
		_processConfigMsi();
}

async::detached StandardPciTransport::_processIrqs() {
#ifdef __x86_64__ // TODO: implement kernlet compilation for aarch64
	co_await connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		fnr::let(
			// Load the PCI_ISR register.
			kernlet_intrin::mmio_read8 (
				fnr::binding<kernlet_types::memory_view>{0}, // IRQ space MMIO region (bound to slot 0).
				fnr::binding<kernlet_types::offset>{1} // IRQ space MMIO offset (bound to slot 1).
					 + fnr::literal{PCI_ISR.offset()} // Offset of USBSTS.
			) & fnr::literal{3}, // Progress and configuration change bits.
			[&] (auto isr) {
				// Ack the IRQ iff one of the bits was set.
				return fnr::ite(
					isr,
					fnr::seq(
						// Trigger the bitset event (bound to slot 2).
						kernlet_intrin::trigger_bitset (
							fnr::binding<kernlet_types::bitset_event>{2},
							isr
						),
						fnr::literal{1}
					),
					fnr::literal{2}
				);
			}
		)
	);

	auto kernlet_object = co_await compile(kernlet_program.data(),
			kernlet_program.size(), {BindType::memoryView, BindType::offset,
			BindType::bitsetEvent});

	HelHandle event_handle;
	HEL_CHECK(helCreateBitsetEvent(&event_handle));
	helix::UniqueDescriptor event{event_handle};

	HelKernletData data[3];
	data[0].handle = _isrMapping.memory().getHandle();
	data[1].handle = _isrMapping.offset();
	data[2].handle = event.getHandle();
	HelHandle bound_handle;
	HEL_CHECK(helBindKernlet(kernlet_object.getHandle(), data, 3, &bound_handle));
	HEL_CHECK(helAutomateIrq(_irq.getHandle(), 0, bound_handle));

	co_await _hwDevice.enableBusIrq();

	// Clear the IRQ in case it was pending while we attached the kernlet.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick | kHelAckClear, 0));

	//std::cout << "core-virtio " << getpid() << ": Starting IRQ loop" << std::endl;
	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(event, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		assert(!(await.bitset() & ~3U));

		if(await.bitset() & 2) {
			std::cout << "core-virtio: Configuration change" << std::endl;
			auto status = _commonSpace().load(PCI_DEVICE_STATUS);
			assert(!(status & DEVICE_NEEDS_RESET));
		}

		if(await.bitset() & 1)
			for(auto &queue : _queues)
				queue->processInterrupt();
	}
#else
	co_await _hwDevice.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto isr = _isrSpace().load(PCI_ISR);
		assert(!(isr & ~3U));

		if(!(isr & 3)) {
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		if(isr & 2) {
			std::cout << "core-virtio: Configuration change" << std::endl;
			auto status = _commonSpace().load(PCI_DEVICE_STATUS);
			assert(!(status & DEVICE_NEEDS_RESET));
		}

		if(isr & 1)
			for(auto &queue : _queues)
				queue->processInterrupt();
	}
#endif
}

async::detached StandardPciTransport::_processConfigMsi() {
	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_configMsi, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		HEL_CHECK(helAcknowledgeIrq(_configMsi.getHandle(), kHelAckAcknowledge, sequence));

		std::cout << "core-virtio: Configuration change" << std::endl;
		auto status = _commonSpace().load(PCI_DEVICE_STATUS);
		assert(!(status & DEVICE_NEEDS_RESET));
	}
}

async::result<void> StandardPciTransport::serviceVector(unsigned int queue_index) {
	// setupQueue() appends to _msiQueues, so all queues must be set up before servicing starts.
	assert(std::ranges::all_of(_queues, [] (auto &queue) { return static_cast<bool>(queue); }));

	auto msiVector = _queueVectors[queue_index];
	// Queues without a vector of their own are serviced by the device-wide IRQ loop.
	if(msiVector == VIRTIO_MSI_NO_VECTOR)
		co_return;
	// Only the first caller services a vector. Queues sharing it are covered by it.
	if(_msiServiced[msiVector].test_and_set(std::memory_order_acq_rel))
		co_return;

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_msis[msiVector], sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		HEL_CHECK(helAcknowledgeIrq(_msis[msiVector].getHandle(),
				kHelAckAcknowledge, sequence));

		for(auto queue : _msiQueues[msiVector])
			queue->processInterrupt();
	}
}

StandardPciQueue::StandardPciQueue(
    StandardPciTransport *transport,
    unsigned int queue_index,
    size_t queue_size,
    arch::dma_buffer virtq,
    arch::dma_object_view<spec::Descriptor> table,
    arch::dma_object_view<spec::AvailableRing> available,
    arch::dma_object_view<spec::UsedRing> used,
    arch::scalar_register<uint16_t> notify_register
)
: Queue{queue_index, queue_size, std::move(virtq), table.data(), available.data(), used.data()},
  _transport{transport},
  _notifyRegister{notify_register} {}

void StandardPciQueue::notifyTransport() {
	_transport->_notifySpace().store(_notifyRegister, queueIndex());
}

static const std::unordered_map<uint8_t, std::string> cap_names{
	{1, "VIRTIO_PCI_CAP_COMMON_CFG"},
	{2, "VIRTIO_PCI_CAP_NOTIFY_CFG"},
	{3, "VIRTIO_PCI_CAP_ISR_CFG"},
	{4, "VIRTIO_PCI_CAP_DEVICE_CFG"},
	{5, "VIRTIO_PCI_CAP_PCI_CFG"},
	{8, "VIRTIO_PCI_CAP_SHARED_MEMORY_CFG"},
};

std::optional<std::string> capName(uint8_t type) {
	if(cap_names.contains(type)) {
		return cap_names.at(type);
	}

	return std::nullopt;
}

} // anonymous namespace

// --------------------------------------------------------
// The discover() function.
// --------------------------------------------------------

async::result<std::unique_ptr<Transport>>
discover(protocols::hw::Device hw_device, DiscoverMode mode) {
	auto info = co_await hw_device.getPciInfo();
	auto irq = co_await hw_device.accessIrq();
	co_await hw_device.enableBusmaster();
	co_await hw_device.enableDma(false);
	auto [iommuActive, dmaSpace, _] = co_await hw_device.getDmaSpace();

	if(mode == DiscoverMode::transitional || mode == DiscoverMode::modernOnly) {
		std::optional<Mapping> common_mapping;
		std::optional<Mapping> notify_mapping;
		std::optional<Mapping> isr_mapping;
		std::optional<Mapping> device_mapping;
		unsigned int notify_multiplier = 0;

		for(size_t i = 0; i < info.caps.size(); i++) {
			if(info.caps[i].type != 0x09)
				continue;

			auto subtype = co_await hw_device.loadPciCapability(i, 3, 1);
			if(subtype != 1 && subtype != 2 && subtype != 3 && subtype != 4)
				continue;

			auto bir = co_await hw_device.loadPciCapability(i, 4, 1);
			auto offset = co_await hw_device.loadPciCapability(i, 8, 4);
			auto length = co_await hw_device.loadPciCapability(i, 12, 4);
			std::cout << "virtio: Subtype: " << capName(subtype).value_or("<invalid>")
					<< " (" << subtype << "), BAR index: " << bir << ", offset: " << offset
					<< ", length: " << length << std::endl;

			assert(info.barInfo[bir].ioType == protocols::hw::IoType::kIoTypeMemory);
			auto bar = co_await hw_device.accessBar(bir);
			Mapping mapping{std::move(bar), info.barInfo[bir].offset + offset, length};

			if(subtype == 1) {
				common_mapping = std::move(mapping);
			}else if(subtype == 2) {
				notify_mapping = std::move(mapping);
				notify_multiplier = co_await hw_device.loadPciCapability(i, 16, 4);
			}else if(subtype == 3) {
				isr_mapping = std::move(mapping);
			}else if(subtype == 4) {
				device_mapping = std::move(mapping);
			}
		}

		if(common_mapping && notify_mapping && isr_mapping && device_mapping) {
			// Reset the device.
			arch::mem_space common_space{common_mapping->get()};
			common_space.store(PCI_DEVICE_STATUS, 0);
			assert(!common_space.load(PCI_DEVICE_STATUS));

			// Enable MSI-X.
			// One vector is reserved for configuration changes, hence we need at least two vectors.
			helix::UniqueDescriptor configMsi;
			auto numMsis = info.numMsis >= (numReservedMsis + 1) ? info.numMsis : 0;
			if (numMsis) {
				co_await hw_device.enableMsi();
				configMsi = co_await hw_device.installMsi(configMsiVector);
			}

			// Set the ACKNOWLEDGE and DRIVER bits.
			// The specification says this should be done in two steps
			common_space.store(PCI_DEVICE_STATUS,
					common_space.load(PCI_DEVICE_STATUS) | ACKNOWLEDGE);
			common_space.store(PCI_DEVICE_STATUS,
					common_space.load(PCI_DEVICE_STATUS) | DRIVER);

			std::cout << "virtio: Using standard PCI transport" << std::endl;
			co_return std::make_unique<StandardPciTransport>(
			    std::move(hw_device),
			    numMsis,
			    std::move(*common_mapping),
			    std::move(*notify_mapping),
			    std::move(*isr_mapping),
			    std::move(*device_mapping),
			    notify_multiplier,
			    std::move(irq),
			    std::move(configMsi),
			    std::move(dmaSpace),
			    iommuActive
			);
		}
	}

	if(mode == DiscoverMode::legacyOnly || mode == DiscoverMode::transitional) {
#ifdef __x86_64__
		if (iommuActive)
			std::println("virtio: using virtio devices with legacy PCI transport bypasses IOMMU");

		if(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort) {
			auto bar = co_await hw_device.accessBar(0);
			HEL_CHECK(helEnableIo(bar.getHandle()));

			// Reset the device.
			arch::io_space legacy_space{static_cast<uint16_t>(info.barInfo[0].address)};
			legacy_space.store(PCI_L_DEVICE_STATUS, 0);
			assert(!legacy_space.load(PCI_L_DEVICE_STATUS));

			// Set the ACKNOWLEDGE and DRIVER bits.
			// The specification says this should be done in two steps
			legacy_space.store(PCI_L_DEVICE_STATUS,
					legacy_space.load(PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
			legacy_space.store(PCI_L_DEVICE_STATUS,
					legacy_space.load(PCI_L_DEVICE_STATUS) | DRIVER);

			std::cout << "virtio: Using legacy PCI transport" << std::endl;
			co_return std::make_unique<LegacyPciTransport>(std::move(hw_device),
					legacy_space, std::move(bar), std::move(irq), std::move(dmaSpace));
		}
#else
		throw std::runtime_error("Legacy transports are unsupported on this architecture");
#endif
	}

	throw std::runtime_error("Cannot construct a suitable virtio::Transport");
}

// --------------------------------------------------------
// Handle
// --------------------------------------------------------

Handle::Handle(Queue *queue, size_t table_index)
: _queue{queue}, _tableIndex{table_index} { }

void Handle::setupBuffer(HostToDeviceType, arch::dma_buffer_view view) {
	assert(view.size());

	uintptr_t physical = this->_queue->dmaSpace().iova_of(view);

	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
}

void Handle::setupBuffer(DeviceToHostType, arch::dma_buffer_view view) {
	assert(view.size());

	uintptr_t physical = this->_queue->dmaSpace().iova_of(view);

	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_WRITE);
}

void Handle::setupBuffer(HostToDeviceType, DmaChunk chunk) {
	assert(chunk.view.size());

	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(chunk.address);
	descriptor->length.store(chunk.view.size());
}

void Handle::setupBuffer(DeviceToHostType, DmaChunk chunk) {
	assert(chunk.view.size());

	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(chunk.address);
	descriptor->length.store(chunk.view.size());
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_WRITE);
}

void Handle::setupLink(Handle other) {
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->next.store(other._tableIndex);
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_NEXT);
}

async::result<void> scatterGather(HostToDeviceType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view) {
	co_await queue->dmaSpace().ensure_mapped(view);
	auto chunks = queue->splitContiguous(view);
	for(auto &chunk : chunks) {
		chain.append(co_await queue->obtainDescriptor());
		chain.setupBuffer(hostToDevice, chunk);
	}
}

async::result<void> scatterGather(DeviceToHostType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view) {
	co_await queue->dmaSpace().ensure_mapped(view);
	auto chunks = queue->splitContiguous(view);
	for(auto &chunk : chunks) {
		chain.append(co_await queue->obtainDescriptor());
		chain.setupBuffer(deviceToHost, chunk);
	}
}

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

Queue::Queue(
    unsigned int queue_index,
    size_t queue_size,
    arch::dma_buffer virtq,
    spec::Descriptor *table,
    spec::AvailableRing *available,
    spec::UsedRing *used
)
: _queueIndex{queue_index},
  _queueSize{queue_size},
  virtq_{std::move(virtq)},
  _progressHead{0} {
	// Construct the hardware state.
	_table = new (table) spec::Descriptor[_queueSize];
	_availableRing = new (available) spec::AvailableRing;
	_usedRing = new (used) spec::UsedRing;
	_availableExtra = new (spec::AvailableExtra::get(available, _queueSize)) spec::AvailableExtra;
	_usedExtra = new (spec::UsedExtra::get(used, _queueSize)) spec::UsedExtra;

	// Initializing the table as 0xFFFF helps debugging
	// as qemu complains if it encounters illegal values.

	_availableRing->flags.store(0);
	_availableRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_availableRing->elements[i].tableIndex.store(0xFFFF);
	_availableExtra->eventIndex.store(0);

	_usedRing->flags.store(0);
	_usedRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_usedRing->elements[i].tableIndex.store(0xFFFF);
	_usedExtra->eventIndex.store(0);

	// Construct the software state.
	_descriptorStack.reserve(_queueSize);
	for(size_t i = 0; i < _queueSize; i++)
		_descriptorStack.push_back(i);
	_descriptorSemaphore.release(_queueSize);
	_activeRequests.resize(_queueSize);
}

std::vector<DmaChunk> Queue::splitContiguous(arch::dma_buffer_view view) {
	// The descriptor length field is only 32 bits wide.
	constexpr size_t maxChunkSize = 0xFFFFF000;

	std::vector<DmaChunk> chunks;

	size_t offset = 0;
	dmaSpace().for_iova_ranges_of(view, [&] (uintptr_t address, size_t size) {
		while(size) {
			auto piece = std::min(size, maxChunkSize);
			chunks.push_back({view.subview(offset, piece), address});
			address += piece;
			offset += piece;
			size -= piece;
		}
	});
	return chunks;
}

async::result<Handle> Queue::obtainDescriptor() {
	Handle handle;
	co_await obtainDescriptors({&handle, 1});
	co_return handle;
}

async::result<void> Queue::obtainDescriptors(std::span<Handle> handles) {
	assert(!handles.empty());
	assert(handles.size() <= _queueSize);

	co_await _descriptorSemaphore.async_acquire(handles.size());

	{
		std::lock_guard lock{_mutex};

		// The semaphore guarantees that enough descriptors are available.
		assert(_descriptorStack.size() >= handles.size());
		for(auto &handle : handles) {
			size_t table_index = _descriptorStack.back();
			_descriptorStack.pop_back();
			handle = Handle{this, table_index};
		}
	}

	// We own the descriptors exclusively now; no need to hold the lock.
	for(auto &handle : handles) {
		auto descriptor = _table + handle.tableIndex();
		descriptor->address.store(0);
		descriptor->length.store(0);
		descriptor->flags.store(0);
	}
}

void Queue::postDescriptor(Handle handle, Request *request,
		void (*complete)(Request *)) {
	assert(request);
	request->complete = complete;

	{
		std::lock_guard lock{_mutex};
		assert(!_activeRequests[handle.tableIndex()]);
		_activeRequests[handle.tableIndex()] = request;

		auto enqueue_head = _availableRing->headIndex.load();
		auto ring_index = enqueue_head & (_queueSize - 1);
		_availableRing->elements[ring_index].tableIndex.store(handle.tableIndex());
		_availableRing->headIndex.store(enqueue_head + 1);
	}
}

void Queue::notify() {
	if(!(_usedRing->flags.load() & VIRTQ_USED_F_NO_NOTIFY))
		notifyTransport();
}

void Queue::processInterrupt() {
	while(true) {
		Request *request;
		size_t freed = 0;
		{
			std::lock_guard lock{_mutex};

			auto used_head = _usedRing->headIndex.load();

			if((_progressHead & 0xFFFF) == used_head)
				break;

			auto ring_index = _progressHead & (_queueSize - 1);
			auto table_index = _usedRing->elements[ring_index].tableIndex.load();
			assert(table_index < _queueSize);

			// Dequeue the Request object.
			request = _activeRequests[table_index];
			assert(request);
			request->len = _usedRing->elements[ring_index].written.load();
			_activeRequests[table_index] = nullptr;

			// Free all descriptors in the descriptor chain.
			auto chain_index = table_index;
			while(_table[chain_index].flags.load() & VIRTQ_DESC_F_NEXT) {
				auto successor = _table[chain_index].next.load();
				_descriptorStack.push_back(chain_index);
				++freed;
				chain_index = successor;
			}
			_descriptorStack.push_back(chain_index);
			++freed;

			_progressHead++;
		}

		// Release the semaphore and run the completion handler outside of the lock.
		_descriptorSemaphore.release(freed);
		request->complete(request);
	}
}

} // namespace virtio_core

