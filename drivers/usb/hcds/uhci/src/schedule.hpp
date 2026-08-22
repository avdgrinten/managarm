#pragma once

#include <arch/dma_pool.hpp>
#include <arch/io_space.hpp>
#include <async/mutex.hpp>
#include <async/promise.hpp>
#include <async/recurring-event.hpp>
#include <frg/list.hpp>
#include <frg/std_compat.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>
#include <queue>

#include "uhci.hpp"

namespace proto = protocols::usb;

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller final : std::enable_shared_from_this<Controller>, proto::BaseController {
	friend struct DeviceState;
	friend struct ConfigurationState;

	struct RootHub final : proto::Hub {
		RootHub(Controller *controller)
		: Hub{nullptr}, _controller{controller} { }

		size_t numPorts() override;
		async::result<proto::PortState> pollState(int port) override;
		async::result<frg::expected<proto::UsbError, void>> issueReset(int port) override;
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> querySpeed(int port) override;

		mbus_ng::EntityId mbusEntityId() override {
			return _controller->_entity.id();
		}

	private:
		Controller *_controller;
	};

	Controller(protocols::hw::Device hw_device, mbus_ng::EntityManager entity,
			uintptr_t base, arch::io_space space, helix::UniqueIrq irq,
			bool iommuActive, helix::UniqueDescriptor dmaSpaceHandle);

	async::result<void> initialize();
	async::detached _handleIrqs();
	async::detached _refreshFrame();

	std::shared_ptr<proto::DeviceServerData>
	createDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) override;


private:
	protocols::hw::Device _hwDevice;
	uintptr_t _ioBase;
	arch::io_space _ioSpace;
	helix::UniqueIrq _irq;

	uint16_t _lastFrame;
	int64_t _frameCounter;
	proto::PortState _portState[2];
	async::recurring_event _portDoorbell;

	mbus_ng::EntityManager _entity;

	arch::dma_realm dmaRealm_{{.dmaMapFlags = kHelMapPreferBottom}};
	arch::contiguous_pool pool_{&dmaRealm_, {.addressBits = 32, .allocateContigous = true}};
	helix::UniqueDescriptor dmaSpaceHandle_;
	arch::dma_space dmaSpace_;
	arch::dma_object<FrameList> _frameListObj{memoryPool()};

	void _updateFrame();

	// ------------------------------------------------------------------------
	// Schedule classes.
	// ------------------------------------------------------------------------

	// Base class for classes that represent elements of the UHCI schedule.
	// All those classes are linked into a list that represents a part of the schedule.
	// They need to be freed through the reclaim mechansim.
	struct ScheduleItem {
		ScheduleItem()
		: reclaimFrame(-1) { }

		virtual ~ScheduleItem() {
			assert(reclaimFrame != -1);
		}

		int64_t reclaimFrame;
		frg::default_list_hook<ScheduleItem> reclaimHook_;
	};

	struct Transaction : ScheduleItem {
		explicit Transaction(arch::dma_array<TransferDescriptor> transfers,
				bool allow_short_packets = false)
		: transfers{std::move(transfers)}, autoToggle{false},
				numComplete{0}, lengthComplete{0},
				allowShortPackets{allow_short_packets} { }

		arch::dma_array<TransferDescriptor> transfers;
		bool autoToggle;
		size_t numComplete;
		size_t lengthComplete;
		bool allowShortPackets;
		async::promise<frg::expected<proto::UsbError, size_t>, frg::stl_allocator> promise;
		uintptr_t cachedTransfersIova = 0;
		frg::default_list_hook<Transaction> hook_;
	};

	struct QueueEntity : ScheduleItem {
		QueueEntity(arch::dma_object<QueueHead> the_head)
		: head{std::move(the_head)}, toggleState{false} {
			head->_linkPointer = QueueHead::LinkPointer();
			head->_elementPointer = QueueHead::ElementPointer();
		}

		async::result<void> initialize(arch::dma_space &space);
		uintptr_t headIova() const { return cachedHeadIova_; }

		arch::dma_object<QueueHead> head;
		bool toggleState;
		frg::default_list_hook<QueueEntity> hook_;
		frg::intrusive_list<
			Transaction,
			frg::locate_member<Transaction, frg::default_list_hook<Transaction>, &Transaction::hook_>
		> transactions;

	private:
		uintptr_t cachedHeadIova_ = 0;
	};

	// ------------------------------------------------------------------------
	// Device management.
	// ------------------------------------------------------------------------

	struct EndpointSlot {
		size_t maxPacketSize;
		QueueEntity *queueEntity;
		async::mutex submissionMutex;
	};

	struct DeviceSlot {
		EndpointSlot controlStates[16];
		EndpointSlot outStates[16];
		EndpointSlot inStates[16];
		bool lowSpeed;
	};

	proto::Enumerator _enumerator;
	std::queue<int> _addressStack;
	DeviceSlot _activeDevices[128];

public:
	async::result<frg::expected<proto::UsbError, std::string>> deviceDescriptor(int address);
	async::result<frg::expected<proto::UsbError, std::string>> configurationDescriptor(int address, uint8_t configuration);

	async::result<frg::expected<proto::UsbError>>
	useConfiguration(int address, int configuration);

	async::result<frg::expected<proto::UsbError>>
	useInterface(int address, uint8_t configIndex, uint8_t configValue, int interface, int alternative);

	arch::contiguous_pool *memoryPool() {
		return &pool_;
	}

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------

	async::result<Transaction *> _buildControl(int address, int pipe, proto::XferFlags dir,
			arch::dma_object_view<proto::SetupPacket> setup, arch::dma_buffer_view buffer,
			bool low_speed, size_t max_packet_size);
	async::result<Transaction *> _buildInterruptOrBulk(int address, int pipe, proto::XferFlags dir,
			arch::dma_buffer_view buffer,
			bool low_speed, size_t max_packet_size,
			bool allow_short_packets);

public:
	async::result<frg::expected<proto::UsbError, size_t>> transfer(int address, int pipe, proto::ControlTransfer info);

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(int address, proto::PipeType type, int pipe, proto::InterruptTransfer info);

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(int address, proto::PipeType type, int pipe, proto::BulkTransfer info);

private:
	async::result<frg::expected<proto::UsbError, size_t>>
	_directTransfer(int address, int pipe, proto::ControlTransfer info,
			QueueEntity *queue, bool low_speed, size_t max_packet_size);

private:
	// ------------------------------------------------------------------------
	// Schedule management.
	// ------------------------------------------------------------------------

	void _linkInterrupt(QueueEntity *entity, int order, int index);
	void _linkAsync(QueueEntity *entity);
	void _linkIntoScheduleTree(int order, int index, QueueEntity *entity);
	void _linkTransaction(QueueEntity *queue, Transaction *transaction);

	void _progressSchedule();
	void _progressQueue(QueueEntity *entity);

	void _reclaim(ScheduleItem *item);

	frg::intrusive_list<
		QueueEntity,
		frg::locate_member<QueueEntity, frg::default_list_hook<QueueEntity>, &QueueEntity::hook_>
	> _interruptSchedule[2 * 1024 - 1];
	frg::intrusive_list<
		QueueEntity,
		frg::locate_member<QueueEntity, frg::default_list_hook<QueueEntity>, &QueueEntity::hook_>
	> _asyncSchedule;
	std::vector<QueueEntity *> _activeEntities;

	// This queue holds all schedule structs that are currently
	// being garbage collected.
	frg::intrusive_list<
		ScheduleItem,
		frg::locate_member<ScheduleItem, frg::default_list_hook<ScheduleItem>, &ScheduleItem::reclaimHook_>
	> _reclaimQueue;

	FrameList *_frameList;

	// ----------------------------------------------------------------------------
	// Debugging functions.
	// ----------------------------------------------------------------------------

	static void _dump(Transaction *transaction);
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState final : proto::DeviceServerData {
	explicit DeviceState(std::shared_ptr<Controller> controller, int device,
			std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed);

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<proto::UsbError, std::string>> deviceDescriptor() override;
	async::result<frg::expected<proto::UsbError, std::string>> configurationDescriptor(uint8_t configuration) override;
	async::result<frg::expected<proto::UsbError, proto::Configuration>> useConfiguration(uint8_t index, uint8_t value) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::ControlTransfer info) override;

	async::result<frg::expected<proto::UsbError>>
	initialize() override;

	async::result<frg::expected<proto::UsbError>>
	updateEp0MaxPacketSize(size_t maxPacketSize) override;

	async::result<frg::expected<proto::UsbError>>
	configureAsHub(std::shared_ptr<proto::Hub> hub) override {
		(void)hub;
		co_return frg::success;
	}

	int address() override {
		return _device;
	}

private:
	std::shared_ptr<Controller> _controller;
	int _device;
};

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

struct ConfigurationState final : proto::ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Controller> controller,
			int device, uint8_t index, uint8_t value);

	async::result<frg::expected<proto::UsbError, proto::Interface>>
	useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	uint8_t _index;
	uint8_t _value;
};

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

struct InterfaceState final : proto::InterfaceData {
	explicit InterfaceState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<frg::expected<proto::UsbError, proto::Endpoint>>
	getEndpoint(proto::PipeType type, int number) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _interface [[maybe_unused]];
};

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

struct EndpointState final : proto::EndpointData {
	explicit EndpointState(std::shared_ptr<Controller> controller,
			int device, proto::PipeType type, int endpoint);

	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::ControlTransfer info) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::InterruptTransfer info) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	proto::PipeType _type;
	int _endpoint;
};

