#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/main.hpp>

namespace thor {

constinit frg::manual_box<PIOLogHandler> pioLogHandler;

inline constexpr arch::scalar_register<uint8_t> data(0);
inline constexpr arch::scalar_register<uint8_t> baudLow(0);
inline constexpr arch::scalar_register<uint8_t> interruptEnable(1);
inline constexpr arch::scalar_register<uint8_t> baudHigh(1);
inline constexpr arch::bit_register<uint8_t> fifoControl(2);
inline constexpr arch::bit_register<uint8_t> lineControl(3);
inline constexpr arch::bit_register<uint8_t> modemControl(4);
inline constexpr arch::bit_register<uint8_t> lineStatus(5);

inline constexpr arch::field<uint8_t, bool> txReady(5, 1);

inline constexpr arch::field<uint8_t, int> dataBits(0, 2);
inline constexpr arch::field<uint8_t, bool> stopBit(2, 1);
inline constexpr arch::field<uint8_t, int> parityBits(3, 3);
inline constexpr arch::field<uint8_t, bool> dlab(7, 1);

inline constexpr arch::field<uint8_t, bool> enableFifos(0, 1);
inline constexpr arch::field<uint8_t, bool> clearRxFifo(1, 1);
inline constexpr arch::field<uint8_t, bool> clearTxFifo(2, 1);

inline constexpr arch::field<uint8_t, bool> dtr(0, 1);
inline constexpr arch::field<uint8_t, bool> rts(1, 1);

extern bool debugToSerial;
extern bool debugToBochs;

void setupDebugging() {
	pioLogHandler.initialize();

	if(debugToSerial) {
		auto base = arch::global_io.subspace(0x3F8);

		// disable all interrupts
		base.store(interruptEnable, 0);

		// Set the baud rate.
		base.store(lineControl, dlab(true));
		base.store(baudLow, 0x01);
		base.store(baudHigh, 0x00);

		// Configure: 8 data bits, 1 stop bit, no parity.
		base.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));

		// clear and enable FIFOs
		base.store(fifoControl, enableFifos(true) | clearRxFifo(true) | clearTxFifo(true));

		// set DTR + RTS
		base.store(modemControl, dtr(true) | rts(true));
	}

	enableLogHandler(pioLogHandler.get());
}

// The device is registered only here since its resources live on the kernel heap.
static initgraph::Task registerBootSerialTask{&globalInitEngine, "x86.register-boot-serial",
	[] {
		if(!debugToSerial)
			return;
		pioLogHandler->addResource({KernelDeviceResourceKind::io, 0x3F8, 8});
		registerKernelDevice(pioLogHandler.get());
	}
};

void PIOLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	auto csiSent = setPriority(md.severity);
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	if (csiSent)
		resetPriority();
	printChar('\n');
}

void PIOLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	auto csiSent = setPriority(md.severity);
	const char *prefix = "URGENT: ";
	while(*prefix)
		printChar(*(prefix++));
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	if (csiSent)
		resetPriority();
	printChar('\n');
}

void PIOLogHandler::printChar(char c) {
	auto sendByteSerial = [this](uint8_t val) {
		auto base = arch::global_io.subspace(0x3F8);

		serialBuffer[serialBufferIndex++] = val;
		if (serialBufferIndex == 16) {
			serialBufferIndex = 0;
			// Output is lost while the UART is suspended.
			if (isSuspended())
				return;
			while(!(base.load(lineStatus) & txReady)) {
				// do nothing until the UART is ready to transmit.
			}
			base.store_iterative(data, serialBuffer, 16);
		}
	};

	if(debugToSerial) {
		if(c == '\n') {
			sendByteSerial('\r');
		}

		sendByteSerial(c);
	}

	if(debugToBochs) {
		auto base = arch::global_io.subspace(0xE9);
		base.store(data, c);
	}
}

bool PIOLogHandler::setPriority(Severity prio) {
	int c;
	switch(prio) {
		case Severity::emergency:
		case Severity::alert:
		case Severity::critical:
		case Severity::error:
			c = 1;
			break;
		case Severity::warning:
			c = 3;
			break;
		case Severity::debug:
			c = 5;
			break;
		default:
			// No CSI necessary.
			return false;
	}

	printChar('\e');
	printChar('[');
	printChar('3');
	printChar('0' + c);
	printChar('m');
	return true;
}

void PIOLogHandler::resetPriority() {
	printChar('\e');
	printChar('[');
	printChar('3');
	printChar('9');
	printChar('m');
}

} // namespace thor
