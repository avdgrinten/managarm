#include <format>
#include <string_view>

#include <arch/mem_space.hpp>
#include <arch/dma_pool.hpp>
#include <arch/barrier.hpp>
#include <async/recurring-event.hpp>
#include <async/sequenced-event.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>
#include <protocols/hw/client.hpp>

#include "spec.hpp"
#include "context.hpp"
#include "trb.hpp"
#include "ring.hpp"

namespace proto = protocols::usb;

constexpr const char *completionCodeNames[256] = {
	"Invalid",
	"Success",
	"Data buffer error",
	"Babble detected",
	"USB transaction error",
	"TRB error",
	"Stall error",
	"Resource error",
	"Bandwidth error",
	"No slots available",
	"Invalid stream type",
	"Slot not enabled",
	"Endpoint not enabled",
	"Short packet",
	"Ring underrun",
	"Ring overrun",
	"VF event ring full",
	"Parameter error",
	"Bandwidth overrun",
	"Context state error",
	"No ping response",
	"Event ring full",
	"Incompatible device",
	"Missed service",
	"Command ring stopped",
	"Command aborted",
	"Stopped",
	"Stopped - invalid length",
	"Stopped - short packet",
	"Max exit latency too high",
	"Reserved",
	"Isoch buffer overrun",
	"Event lost",
	"Undefined error",
	"Invalid stream ID",
	"Secondary bandwidth error",
	"Split transaction error",
};

struct Controller;

// ----------------------------------------------------------------
// Interrupter
// ----------------------------------------------------------------

struct Interrupter {
	Interrupter(EventRing *ring, arch::mem_space space)
	: _ring{ring}, _space{space} { }

	async::result<void> initialize();
	async::detached handleIrqs(helix::UniqueIrq &irq);
	async::detached pollIrqs();

	void dump(Controller *controller);

private:
	bool _isBusy();
	void _clearPending();
	void _updateDequeue();

	EventRing *_ring;
	arch::mem_space _space;
};

// ----------------------------------------------------------------
// Device & {Configuration,Interface,Endpoint}State
// ----------------------------------------------------------------

inline int getEndpointIndex(int endpoint, proto::PipeType dir) {
	using proto::PipeType;

	// For control endpoints the index is:
	//  DCI = (Endpoint Number * 2) + 1.
	// For interrupt, bulk, isoch, the index is:
	//  DCI = (Endpoint Number * 2) + Direction,
	//    where Direction = '0' for OUT endpoints
	//    and '1' for IN endpoints.

	return endpoint * 2 +
		((dir == PipeType::in || dir == PipeType::control)
				? 1 : 0);
}

struct EndpointState;

struct Device final : proto::DeviceData, std::enable_shared_from_this<Device> {
	Device(Controller *controller);

	// Public API inherited from DeviceData.
	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<proto::UsbError, std::string>>
	deviceDescriptor() override;

	async::result<frg::expected<proto::UsbError, std::string>>
	configurationDescriptor(uint8_t configuration) override;

	async::result<frg::expected<proto::UsbError, proto::Configuration>>
	useConfiguration(uint8_t index, uint8_t value) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::ControlTransfer info) override;


	void submit(int endpoint);

	async::result<frg::expected<proto::UsbError>>
	enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType);

	async::result<frg::expected<proto::UsbError>>
	readDescriptor(arch::dma_buffer_view dest, uint16_t desc);

	async::result<frg::expected<proto::UsbError>>
	setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type, int interval);

	async::result<frg::expected<proto::UsbError>>
	configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed);

	async::result<frg::expected<proto::UsbError>>
	updateEp0PacketSize(size_t maxPacketSize);

	// Endpoint State field of the endpoint's context in the device context (0 = disabled, 1 = running,
	// 2 = halted, 3 = stopped, 4 = error).
	uint8_t endpointState(int endpointId);


	size_t slot() const {
		return _slotId;
	}

	Controller *controller() const {
		return _controller;
	}

	proto::DeviceSpeed speed() const {
		return _speed;
	}

	std::shared_ptr<EndpointState> endpoint(int endpointId) {
		return _endpoints[endpointId - 1];
	}

private:
	int _slotId;

	Controller *_controller;

	DeviceContext _devCtx;

	async::result<void> _initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type, int interval);

	std::array<std::shared_ptr<EndpointState>, 31> _endpoints;

	proto::DeviceSpeed _speed{};
};


struct EndpointState final : proto::EndpointData {
	friend struct Device;

	explicit EndpointState(Device *device, int endpointId, proto::EndpointType type, size_t maxPacketSize)
	: _device{device}, _endpointId{endpointId}, _type{type},
		_maxPacketSize{maxPacketSize}, _transferRing{device->controller()} { }

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::ControlTransfer info) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::InterruptTransfer info) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::BulkTransfer info) override;

	ProducerRing &transferRing() {
		return _transferRing;
	}

private:
	Device *_device;
	int _endpointId;
	proto::EndpointType _type;

	size_t _maxPacketSize;
	ProducerRing _transferRing;

	async::mutex _submissionMutex;

	async::result<frg::expected<proto::UsbError, size_t>>
	_postTd(std::vector<RawTrb> &&trbs, arch::dma_buffer_view buffer, bool toHost, std::string what);

	async::result<frg::expected<proto::UsbError>>
	_resetAfterError(RingPointer nextDequeue);
};


struct ConfigurationState final : proto::ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Device> device, uint8_t index)
	: _device{device}, _index{index} { }

	async::result<frg::expected<proto::UsbError, proto::Interface>>
	useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Device> _device;
	uint8_t _index;
};


struct InterfaceState final : proto::InterfaceData {
	explicit InterfaceState(std::shared_ptr<Device> device, int interface)
	: proto::InterfaceData{interface}, _device{device} { }

	async::result<frg::expected<proto::UsbError, proto::Endpoint>>
	getEndpoint(proto::PipeType type, int number) override {
		co_return proto::Endpoint{_device->endpoint(getEndpointIndex(number, type))};
	}

private:
	std::shared_ptr<Device> _device;
};

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

struct Controller final : proto::BaseController {
	Controller(protocols::hw::Device hw_device,
			mbus_ng::Entity entity,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio,
			helix::UniqueIrq irq,
			std::string name,
			bool iommuActive,
			helix::UniqueDescriptor dmaSpaceHandle);

	~Controller() = default;

	async::detached initialize();

	async::result<frg::expected<proto::UsbError>>
	enumerateDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) override;

	arch::contiguous_pool *memoryPool() {
		return &_memoryPool;
	}

	void processEvent(Event ev);

	// Prints controller and interrupter state, e.g., when a transfer does not complete.
	void dumpState();

	void ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t streamId = 0);

	async::result<Event> submitCommand(RawTrb trb) {
		ProducerRing::Transaction tx;
		std::vector<RawTrb> trbs{trb};

		co_await _cmdRing.pushTrbs(trbs, &tx);

		ringDoorbell(0, 0);

		co_return co_await tx.command();
	}

	bool largeCtx() const {
		return _largeCtx;
	}

	void setDeviceContext(size_t slot, DeviceContext &ctx) {
		_dcbaa[slot] = ctx.iova();
		barrier.writeback(_dcbaa.view_buffer());
	}

	std::string_view name() const {
		return _name;
	}

	arch::dma_barrier barrier{
		// TODO(qookie): This can be found out properly via device tree properties
		// (either of the PCIe RC, or device itself, depending on how it's found).
#if defined(__x86_64__)
		true
#else
		false
#endif
	};

	// Controller commands -----------------------------------------------------------

	async::result<frg::expected<proto::UsbError, uint32_t>>
	enableSlot(uint8_t slotType);

	async::result<frg::expected<proto::UsbError>>
	addressDevice(uint32_t slotId, InputContext &ctx);

	async::result<frg::expected<proto::UsbError>>
	configureEndpoint(uint32_t slotId, InputContext &ctx);

	async::result<frg::expected<proto::UsbError>>
	evaluateContext(uint32_t slotId, InputContext &ctx);

	async::result<frg::expected<proto::UsbError>>
	resetEndpoint(uint32_t slotId, uint32_t endpointId);

	async::result<frg::expected<proto::UsbError>>
	setTransferRingDequeue(uint32_t slotId, uint32_t endpointId, ProducerRing &ring, RingPointer pointer);

private:
	struct SupportedProtocol;

	struct Port {
		Port(int id, arch::mem_space space, Controller *controller, SupportedProtocol *port);
		void reset();
		void disable();
		void resetChangeBits();
		bool isConnected();
		bool isEnabled();
		bool isPowered();
		void transitionToLinkStatus(uint8_t status);
		async::detached initPort();

		template <typename T>
		async::result<void> awaitFlag(arch::field<uint32_t, T> field, T value) {
			while (true) {
				resetChangeBits();
				if ((_space.load(port::portsc) & field) == value)
					co_return;

				async::cancellation_event ev;
				helix::TimeoutCancellation tc{1'000'000'000, ev};

				co_await _doorbell.async_wait(ev);
				co_await tc.retire();
			}
		}

		void logPortsc(std::string_view what);

		async::recurring_event _doorbell;

		async::result<proto::PortState> pollState();
		async::result<frg::expected<proto::UsbError, void>> issueReset();
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> querySpeed();

	private:
		uint8_t getLinkStatus();
		uint8_t getSpeed();
		int _id;
		Controller *_controller;
		std::shared_ptr<Device> _device;
		SupportedProtocol *_proto;
		arch::mem_space _space;

		async::sequenced_event _pollEv;
		uint64_t _pollSeq = 0;
		proto::PortState _state{};
	};

	struct RootHub final : proto::Hub {
		RootHub(Controller *controller, SupportedProtocol &proto, arch::mem_space portSpace, mbus_ng::EntityManager entity);

		size_t numPorts() override;
		async::result<proto::PortState> pollState(int port) override;
		async::result<frg::expected<proto::UsbError, void>> issueReset(int port) override;
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> querySpeed(int port) override;

		SupportedProtocol *protocol() {
			return _proto;
		}

		auto entityId() {
			return _entity.id();
		}

	private:
		Controller *_controller;
		SupportedProtocol *_proto;
		std::vector<std::unique_ptr<Port>> _ports;
		mbus_ng::EntityManager _entity;
	};

	struct SupportedProtocol {
		int minor;
		int major;

		size_t compatiblePortStart;
		size_t compatiblePortCount;

		size_t slotType;
	};

	std::vector<SupportedProtocol> _supportedProtocols;

	protocols::hw::Device _hw_device;
	helix::Mapping _mapping;
	helix::UniqueDescriptor _mmio;
	helix::UniqueIrq _irq;
	arch::mem_space _space;
	arch::mem_space _doorbells;

	std::string _name;

	async::result<void> _processExtendedCapabilities();
	async::result<void> _biosHandoff(uint32_t cap);

	arch::dma_realm _dmaRealm;
	arch::contiguous_pool _memoryPool;
	helix::UniqueDescriptor _dmaSpaceHandle;
	arch::dma_space _dmaSpace;

	arch::dma_array<uint64_t> _dcbaa;
	// The scratchpad buffer array is required to be 64-byte aligned.
	arch::dma_array<uint64_t, 64> _scratchpadBufArray;
	std::vector<arch::dma_buffer> _scratchpadBufs;

	std::vector<std::unique_ptr<Interrupter>> _interrupters;
	std::vector<Port *> _ports;
	std::array<std::shared_ptr<Device>, 256> _devices;

	std::vector<std::shared_ptr<RootHub>> _rootHubs;

	ProducerRing _cmdRing;
	EventRing _eventRing;

	int _numPorts;
	int _maxDeviceSlots;

	proto::Enumerator _enumerator;

	bool _largeCtx;

	mbus_ng::Entity _entity;

public:
	arch::dma_space &dmaSpace() {
		return _dmaSpace;
	}
};

template<>
struct std::formatter<Controller *, char> {
	template<class Ctx>
	constexpr Ctx::iterator parse(Ctx &ctx) {
		return ctx.begin();
	}

	template<class Ctx>
	Ctx::iterator format(Controller *controller, Ctx &ctx) const {
		return std::format_to(ctx.out(), "xhci {}:", controller->name());
	}
};

template<>
struct std::formatter<Controller &, char> {
	template<class Ctx>
	constexpr Ctx::iterator parse(Ctx &ctx) {
		return ctx.begin();
	}

	template<class Ctx>
	Ctx::iterator format(const Controller &controller, Ctx &ctx) const {
		return std::format_to(ctx.out(), "xhci {}:", controller.name());
	}
};
