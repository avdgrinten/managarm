#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <iostream>
#include <queue>
#include <memory>

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <async/oneshot-event.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

#include <blockfs.hpp>

namespace {
	constexpr bool logIrqs = false;
	constexpr bool logRequests = false;
}

// --------------------------------------------------------
// Controller class
// --------------------------------------------------------

namespace regs {
	inline constexpr arch::scalar_register<uint16_t> ioData{0};
	inline constexpr arch::scalar_register<uint8_t> inStatus{7};

	inline constexpr arch::scalar_register<uint8_t> outSectorCount{2};
	inline constexpr arch::scalar_register<uint8_t> outLba1{3};
	inline constexpr arch::scalar_register<uint8_t> outLba2{4};
	inline constexpr arch::scalar_register<uint8_t> outLba3{5};
	inline constexpr arch::scalar_register<uint8_t> outDevice{6};
	inline constexpr arch::scalar_register<uint8_t> outCommand{7};
}

namespace alt_regs {
	inline constexpr arch::scalar_register<uint8_t> inStatus{0};
}

class Controller : public blockfs::BlockDevice {
	enum class IoResult {
		none,
		timeout,
		notReady,
		noData,
		withData
	};

public:
	Controller(int64_t parentId, uint16_t mainOffset, uint16_t altOffset,
			helix::UniqueDescriptor mainBar, helix::UniqueDescriptor altBar,
			helix::UniqueDescriptor irq);

public:
	async::detached run();

private:
	async::detached _doRequestLoop();
	async::result<IoResult> _pollForBsy();
	async::result<IoResult> _waitForBsyIrq();

public:
	async::result<std::expected<void, blockfs::Error>> flush() override;

	async::result<size_t> getSize() override;

protected:
	async::result<std::expected<void, blockfs::Error>> issueRead(unsigned int, uint64_t sector, arch::dma_buffer_view view) override;
	async::result<std::expected<void, blockfs::Error>> issueWrite(unsigned int, uint64_t sector, arch::dma_buffer_view view) override;

private:
	enum Commands {
		kCommandReadSectors = 0x20,
		kCommandReadSectorsExt = 0x24,
		kCommandWriteSectors = 0x30,
		kCommandWriteSectorsExt = 0x34,
		kCommandIdentify = 0xEC,
		kCommandFlush = 0xE7,
		kCommandFlushExt = 0xEA,
	};

	enum Flags {
		kStatusErr = 0x01,
		kStatusDrq = 0x08,
		kStatusDf = 0x20,
		kStatusRdy = 0x60,
		kStatusBsy = 0x80,

		kDeviceSlave = 0x10,
		kDeviceLba = 0x40
	};

	enum class RequestType { read, write, flush };

	struct Request {
		RequestType type;
		uint64_t sector;
		size_t numSectors;
		arch::dma_buffer_view view;
		async::oneshot_primitive event;
	};

	async::result<void> _performRequest(Request *request);

	async::result<bool> _detectDevice();

	std::queue<Request *> _requestQueue;
	async::recurring_event _doorbell;

	helix::UniqueDescriptor _irq;
	arch::io_space _ioSpace;
	arch::io_space _altSpace;

	bool _supportsLBA48;
	uint8_t _flushCommand = 0;
	bool _writeCacheKnownDisabled = false;

	uint64_t _irqSequence;
};

Controller::Controller(
    int64_t parentId,
    uint16_t mainOffset,
    uint16_t altOffset,
    helix::UniqueDescriptor mainBar,
    helix::UniqueDescriptor altBar,
    helix::UniqueDescriptor irq
)
// PIO transfers one command at a time; the sector count register holds up to 255 sectors.
: BlockDevice{512, parentId, nullptr, {.maxSectorsPerRequest = 255, .queueCapacity = 1}},
  _irq{std::move(irq)},
  _ioSpace{mainOffset},
  _altSpace{altOffset},
  _supportsLBA48{false} {
	HEL_CHECK(helEnableIo(mainBar.getHandle()));
	HEL_CHECK(helEnableIo(altBar.getHandle()));
}

async::detached Controller::run() {
	// Initialize the _irqSequence. For now, assume that this is 0.
	// TODO: if the driver restarts, we would need to get the current IRQ sequence from the kernel.
	_irqSequence = 0;

	if (!(co_await _detectDevice())) {
		std::cout << "block/ata: Could not detect drive" << std::endl;
		co_return;
	}

	_doRequestLoop();

	blockfs::runDevice(this);
}

async::detached Controller::_doRequestLoop() {
	while(true) {
		if(_requestQueue.empty()) {
			co_await _doorbell.async_wait();
			continue;
		}

		auto request = _requestQueue.front();
		_requestQueue.pop();
		co_await _performRequest(request);
		request->event.raise();
	}
}

auto Controller::_pollForBsy() -> async::result<IoResult> {
	while(true) {
		auto altStatus = _altSpace.load(alt_regs::inStatus);
		if(altStatus & kStatusBsy)
			continue; // TODO: sleep some time before continuing.
		// TODO: Report those errors to the caller.
		if(!(altStatus & kStatusRdy)) // Device was disconnected?
			co_return IoResult::notReady;
		assert(!(altStatus & kStatusErr));
		assert(!(altStatus & kStatusDf));
		co_return ((altStatus & kStatusDrq) ? IoResult::withData : IoResult::noData);
	}
}

auto Controller::_waitForBsyIrq() -> async::result<IoResult> {
	while(true) {
		if(logIrqs)
			std::cout << "block/ata: Awaiting IRQ." << std::endl;
		auto await = co_await helix_ng::awaitEvent(_irq, _irqSequence);
		HEL_CHECK(await.error());
		_irqSequence = await.sequence();
		if(logIrqs)
			std::cout << "block/ata: IRQ fired." << std::endl;

		// Since ATA has no internal ISR register, we check BSY to see if the IRQ was likely
		// caused by this controller.
		// If BSY is clear, the job of this function is done.
		// Otherwise, if BSY is set, check an external ISR (e.g., PCI confiuration space),
		// or error out below.
		auto altStatus = _altSpace.load(alt_regs::inStatus);
		if(altStatus & kStatusBsy) {
			// TODO: Check the PCI registers if the IRQ is pending.
			//       This is the only situation where we should loop.
			if(false) {
				HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, _irqSequence));
				continue;
			}
			std::cout << "\e[31m" "block/ata: Drive asserted IRQ without clearing BSY"
					"\e[39m" << std::endl;
		}

		// Clear and acknowledge the IRQ.
		auto status = _ioSpace.load(regs::inStatus);
		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, _irqSequence));
		// When BSY is still set, all other bits are meaningless.
		if(status & kStatusBsy)
			co_return IoResult::timeout; // TODO: properly implement the timeout!
		// TODO: Report those errors to the caller.
		if(!(status & kStatusRdy)) // Device was disconnected?
			co_return IoResult::notReady;
		assert(!(status & kStatusErr));
		assert(!(status & kStatusDf));
		co_return ((status & kStatusDrq) ? IoResult::withData : IoResult::noData);
	}
}

async::result<std::expected<void, blockfs::Error>> Controller::issueRead(unsigned int, uint64_t sector, arch::dma_buffer_view view) {
	auto numSectors = (view.size() + sectorSize - 1) >> sectorShift;

	Request request{};
	request.type = RequestType::read;
	request.sector = sector;
	request.numSectors = numSectors;
	request.view = view;

	_requestQueue.push(&request);
	_doorbell.raise();

	co_await request.event.wait();
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> Controller::issueWrite(unsigned int, uint64_t sector, arch::dma_buffer_view view) {
	auto numSectors = (view.size() + sectorSize - 1) >> sectorShift;

	Request request{};
	request.type = RequestType::write;
	request.sector = sector;
	request.numSectors = numSectors;
	request.view = view;

	_requestQueue.push(&request);
	_doorbell.raise();

	co_await request.event.wait();
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> Controller::flush() {
	if (_writeCacheKnownDisabled)
		co_return {};
	if (!_flushCommand) {
		std::cout << "block/ata: Device does not support cache flush" << std::endl;
		abort();
	}

	Request request{};
	request.type = RequestType::flush;
	_requestQueue.push(&request);
	_doorbell.raise();
	co_await request.event.wait();
	co_return {};
}

async::result<size_t> Controller::getSize() {
	std::cout << "ata: Controller::getSize() is a stub!" << std::endl;
	co_return 0;
}

async::result<bool> Controller::_detectDevice() {
	_ioSpace.store(regs::outDevice, kDeviceLba);
	// TODO: delay

	// TODO: Detect ATAPI drives.
	// For ATAPI drives, we do not need to wait until RDY is set.

	// Try to detect non-ATAPI drives now.
	// Virtually all non-ATAPI commands (inclduing IDENTIY) require RDY to be set
	// (this is documented on a per-command basis in the ATA specification).
	// The RDY bit is set in <= 30s after the drive spins up.

	// First, wait until RDY becomes set, then send IDENTITY.
	// In principle, we would have to wait for 30s.
	// Let us hope that 5s are enough on real hardware.
	bool isRdy = false;
	for(int i = 0; i < 5; ++i) {
		auto altStatus = _altSpace.load(alt_regs::inStatus);
		// We cannot trust RDY is BSY is set.
		if(altStatus & kStatusBsy)
			continue;
		if(altStatus & kStatusRdy) {
			isRdy = true;
			break;
		}
		co_await helix::sleepFor(1'000'000'000);
	}

	if(!isRdy)
		co_return false;

	_ioSpace.store(regs::outCommand, kCommandIdentify);

	auto ioRes = co_await _waitForBsyIrq();
	if (ioRes != IoResult::withData)
		co_return false;

	uint8_t ident_data[512];
	_ioSpace.load_iterative(regs::ioData, reinterpret_cast<uint16_t *>(ident_data), 256);

	char model[41];
	memcpy(model, ident_data + 54, 40);
	model[40] = 0;

	// model name is returned as big endian, swap each 2-byte pair to fix that
	for (int i = 0; i < 40; i += 2) {
		uint8_t tmp = model[i];
		model[i] = model[i + 1];
		model[i + 1] = tmp;
	}

	_supportsLBA48 = (ident_data[167] & (1 << 2))
			&& (ident_data[173] & (1 << 2));

	auto word = [&](size_t index) -> uint16_t {
		return ident_data[index * 2] | (uint16_t{ident_data[index * 2 + 1]} << 8);
	};
	if ((word(83) & 0xC000) == 0x4000) {
		if (word(83) & (1 << 13))
			_flushCommand = kCommandFlushExt;
		else if (word(83) & (1 << 12))
			_flushCommand = kCommandFlush;
		_writeCacheKnownDisabled = !(word(82) & (1 << 5));
	}
	if ((word(87) & 0xC000) == 0x4000 && !(word(85) & (1 << 5)))
		_writeCacheKnownDisabled = true;

	printf("block/ata: detected device, model: '%s', %s 48-bit LBA\n", model, _supportsLBA48 ? "supports" : "doesn't support");

	co_return true;
}

async::result<void> Controller::_performRequest(Request *request) {
	if(logRequests)
		std::cout << "block/ata: Reading/writing " << request->numSectors
				<< " sectors from " << request->sector << std::endl;

	assert(!(request->sector & ~((size_t(1) << 48) - 1)));
	assert(request->numSectors <= 255);

	_ioSpace.store(regs::outDevice, kDeviceLba);
	// TODO: There should be a 400ns delay after drive selection.

	if (request->type == RequestType::flush) {
		_ioSpace.store(regs::outCommand, _flushCommand);
		auto ioRes = co_await _waitForBsyIrq();
		if (ioRes != IoResult::noData) {
			std::cout << "block/ata: Cache flush failed" << std::endl;
			abort();
		}
		co_return;
	}

	if (_supportsLBA48) {
		_ioSpace.store(regs::outSectorCount, (request->numSectors >> 8) & 0xFF);
		_ioSpace.store(regs::outLba1, (request->sector >> 24) & 0xFF);
		_ioSpace.store(regs::outLba2, (request->sector >> 32) & 0xFF);
		_ioSpace.store(regs::outLba3, (request->sector >> 40) & 0xFF);
	}

	_ioSpace.store(regs::outSectorCount, request->numSectors & 0xFF);
	_ioSpace.store(regs::outLba1, request->sector & 0xFF);
	_ioSpace.store(regs::outLba2, (request->sector >> 8) & 0xFF);
	_ioSpace.store(regs::outLba3, (request->sector >> 16) & 0xFF);

	if(request->type == RequestType::read) {
		if (_supportsLBA48)
			_ioSpace.store(regs::outCommand, kCommandReadSectorsExt);
		else
			_ioSpace.store(regs::outCommand, kCommandReadSectors);

		// Receive the result for each sector.
		for(size_t k = 0; k < request->numSectors; k++) {
			auto ioRes = co_await _waitForBsyIrq();
			assert(ioRes == IoResult::withData);

			// Read the data.
			// TODO: Do we have to be careful with endianess here?
			auto chunk = reinterpret_cast<uint8_t *>(request->view.byte_data()) + k * 512;
			// TODO: The following is a hack. Lock the page into memory instead!
			*static_cast<volatile uint8_t *>(chunk); // Fault in the page.
			_ioSpace.load_iterative(regs::ioData, reinterpret_cast<uint16_t *>(chunk), 256);
		}
	}else{
		if (_supportsLBA48)
			_ioSpace.store(regs::outCommand, kCommandWriteSectorsExt);
		else
			_ioSpace.store(regs::outCommand, kCommandWriteSectors);

		// Write requests do not generate an IRQ for the first sector.
		auto ioRes = co_await _pollForBsy();
		assert(ioRes == IoResult::withData);

		// Receive the result for each sector.
		for(size_t k = 0; k < request->numSectors; k++) {
			// Read the data.
			// TODO: Do we have to be careful with endianess here?
			auto chunk = reinterpret_cast<uint8_t *>(request->view.byte_data()) + k * 512;
			// TODO: The following is a hack. Lock the page into memory instead!
			*static_cast<volatile uint8_t *>(chunk); // Fault in the page.
			_ioSpace.store_iterative(regs::ioData, reinterpret_cast<uint16_t *>(chunk), 256);

			// Wait for the device to process the sector.
			auto ioRes = co_await _waitForBsyIrq();
			if(k + 1 < request->numSectors) {
				assert(ioRes == IoResult::withData);
			}else{
				assert(ioRes == IoResult::noData);
			}
		}
	}

	if(logRequests)
		std::cout << "block/ata: Reading/writing from " << request->sector
				<< " complete" << std::endl;
}

std::vector<std::shared_ptr<Controller>> globalControllers;

// ------------------------------------------------------------------------
// Freestanding discovery functions.
// ------------------------------------------------------------------------

async::detached bindController(mbus_ng::Entity hwEntity) {
	protocols::hw::Device device((co_await hwEntity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	assert(info.barInfo[1].ioType == protocols::hw::IoType::kIoTypePort);
	auto mainBar = co_await device.accessBar(0);
	auto altBar = co_await device.accessBar(1);
	auto irq = co_await device.accessIrq();

	auto controller = std::make_shared<Controller>(hwEntity.id(),
			info.barInfo[0].address, info.barInfo[1].address,
			std::move(mainBar), std::move(altBar),
			std::move(irq));
	controller->run();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"legacy", "ata"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "block/ata: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("block/ata: Starting driver\n");

	observeControllers();
	// This driver is not thread-safe yet; run the pool single threaded.
	helix::DispatcherPool::global().setThreadCount(1);
	helix::DispatcherPool::global().blockOn(
			async::suspend_indefinitely(async::cancellation_token{}));
}
