#pragma once

#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-device.hpp>

namespace _debug {

enum class Condition {
	execute, write, io, readwrite
};
enum class Size {
	size1, size2, size8, size4
};

template<typename T>
struct BreakOnWrite;

template<typename T> requires (sizeof(T) == 1)
struct BreakOnWrite<T> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size1);
	}
};

template<typename T> requires (sizeof(T) == 4)
struct BreakOnWrite<T> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size4);
	}
};

template<typename T> requires (sizeof(T) == 8)
struct BreakOnWrite<T> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size8);
	}
};

inline void installBreak(const void *p, Condition condition, Size size) {
	asm volatile ("mov %0, %%db0" : : "r"(p) : "memory");

	uint64_t trigger = (uint64_t(size) << 18) | (uint64_t(condition) << 16) | (1 << 1);
	asm volatile ("mov %0, %%db7" : : "r"(trigger) : "memory");
}

} // namespace _debug

template<typename T>
void breakOnWrite(const T *p) {
	_debug::BreakOnWrite<T>::invoke(p);
}

namespace thor {

struct PIOLogHandler final : LogHandler, KernelDevice {
	constexpr PIOLogHandler()
	: KernelDevice{"boot-serial"}, serialBufferIndex{0}, serialBuffer{0} {
		takesUrgentLogs = true;
	}

	void emit(frg::string_view record) override;
	void emitUrgent(frg::string_view record) override;

	void printChar(char c);
	bool setPriority(Severity severity);
	void resetPriority();

private:
	int serialBufferIndex;
	uint8_t serialBuffer[16];
};

} // namespace thor
