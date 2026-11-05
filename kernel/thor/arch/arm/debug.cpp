#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <eir/interface.hpp>
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/main.hpp>
#include <uart/uart.hpp>

namespace thor {

constinit common::uart::AnyUart bootUart;
constinit frg::manual_box<UartLogHandler> uartLogHandler;

extern bool debugToSerial;

extern ManagarmElfNote<BootUartConfig> bootUartConfig;
THOR_DEFINE_ELF_NOTE(bootUartConfig){elf_note_type::bootUartConfig, {}};

void setupDebugging() {
	if (debugToSerial) {
		if (bootUartConfig->type == BootUartType::pl011) {
			bootUart.emplace<common::uart::PL011>(bootUartConfig->window, 0);
		} else if (bootUartConfig->type == BootUartType::samsung) {
			bootUart.emplace<common::uart::Samsung>(bootUartConfig->window);
		}

		if (!std::holds_alternative<std::monostate>(bootUart)) {
			uartLogHandler.initialize(&bootUart);
			enableLogHandler(uartLogHandler.get());
		}
	}
}

// The device is registered only here since its resources live on the kernel heap.
static initgraph::Task registerBootUartTask{&globalInitEngine, "arm.register-boot-uart",
	[] {
		if (!uartLogHandler.valid())
			return;
		uartLogHandler->addResource(
		    {KernelDeviceResourceKind::mmio, bootUartConfig->address, bootUartConfig->size}
		);
		registerKernelDevice(uartLogHandler.get());
	}
};

void UartLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	print(msg);
}

void UartLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	print("URGENT: ", false);
	print(msg);
}

void UartLogHandler::print(frg::string_view line, bool printNL) {
	// Output is lost while the UART is suspended.
	if (isSuspended())
		return;
	std::visit(
	    frg::overloaded{
	        [](std::monostate) { __builtin_trap(); },
	        [&](auto &inner) {
		        for (size_t i = 0; i < line.size(); ++i) {
			        auto c = line[i];
			        if (c == '\n') {
				        inner.write('\r');
				        inner.write('\n');
			        } else {
				        inner.write(c);
			        }
		        }
		        if (printNL) {
			        inner.write('\r');
			        inner.write('\n');
		        }
	        }
	    },
	    *uart_
	);
}

} // namespace thor
