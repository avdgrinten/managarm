#pragma once

#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-device.hpp>
#include <uart/uart.hpp>

namespace thor {

struct UartLogHandler : LogHandler, KernelDevice {
	constexpr UartLogHandler(common::uart::AnyUart *uart)
	: KernelDevice{"boot-uart"}, uart_{uart} {
		takesUrgentLogs = true;
	}

	void emit(frg::string_view record) override;
	void emitUrgent(frg::string_view record) override;

	void print(frg::string_view line, bool printNL = true);

private:
	common::uart::AnyUart *uart_;
};

} // namespace thor
