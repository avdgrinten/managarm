#pragma once

#include <cstddef>

#include <frg/expected.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/error.hpp>
#include <initgraph.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <smarter.hpp>

namespace thor {

struct KernelIoChannel {
public:
	using IoFlags = unsigned int;

	// The following flags control what kind of I/O operations issueIo() should perform.
	// * If ioProgressOutput is given, issueIo() must make an effort to flush output;
	//   in other words, writeableSpan() must become larger.
	// * If ioProgressInput is given, issueIo() must make an effort to obtain more input;
	//   in other words, readableSpan() must become larger.
	// * If both ioProgress flags are given, issueIo() should return once progress is made
	//   in either direction (but not necessarily both).
	// * issueIo() is always allowed to also make progress in the other direction.
	static constexpr IoFlags ioProgressOutput = 1;
	static constexpr IoFlags ioProgressInput = 2;

	// Write all output.
	static constexpr IoFlags ioFlush = 4;

	KernelIoChannel(frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag)
	: tag_{std::move(tag)}, descriptiveTag_{std::move(descriptiveTag)} { }

	KernelIoChannel(const KernelIoChannel &) = delete;

protected:
	~KernelIoChannel() = default;

public:
	KernelIoChannel &operator= (const KernelIoChannel &) = delete;

	frg::string_view tag() {
		return tag_;
	}

	frg::string_view descriptiveTag() {
		return descriptiveTag_;
	}

	frg::span<std::byte> writableSpan() {
		return writable_;
	}

	frg::span<const std::byte> readableSpan() {
		return readable_;
	}

	// These two functions inform the channel that bytes have been written to
	// (or taken from) writeableSpan() (or readableSpan(), respectively).
	// The advance the spans but do not necessarily invoke I/O.
	virtual void produceOutput(size_t n) = 0;
	virtual void consumeInput(size_t n) = 0;

	virtual coroutine<frg::expected<Error>> issueIo(IoFlags flags) = 0;

	// ----------------------------------------------------------------------------------
	// High-level convenience API.
	// For users that are not performance critical; unless the compiler manages to
	// elide all coroutines, the low-level API will be considerably faster.
	// ----------------------------------------------------------------------------------

	coroutine<frg::expected<Error>> writeOutput(uint8_t b) {
		auto span = writableSpan();
		if(!span.size()) {
			FRG_CO_TRY(co_await issueIo(ioProgressOutput));
			span = writableSpan();
			assert(span.size());
		}
		*span.data() = static_cast<std::byte>(b);
		produceOutput(1);
		FRG_CO_TRY(co_await issueIo(ioProgressOutput | ioFlush));
		co_return {};
	}

	coroutine<frg::expected<Error>> postOutput(uint8_t b) {
		auto span = writableSpan();
		if(!span.size()) {
			FRG_CO_TRY(co_await issueIo(ioProgressOutput));
			span = writableSpan();
			assert(span.size());
		}
		*span.data() = static_cast<std::byte>(b);
		produceOutput(1);
		co_return {};
	}

	coroutine<frg::expected<Error>> flushOutput() {
		return issueIo(ioProgressOutput | ioFlush);
	}

	coroutine<frg::expected<Error, uint8_t>> readInput() {
		auto span = readableSpan();
		if(!span.size()) {
			FRG_CO_TRY(co_await issueIo(ioProgressInput));
			span = readableSpan();
			assert(span.size());
		}
		auto b = static_cast<uint8_t>(*span.data());
		consumeInput(1);
		co_return b;
	}

protected:
	void updateWritableSpan(frg::span<std::byte> span) {
		writable_ = span;
	}

	void updateReadableSpan(frg::span<const std::byte> span) {
		readable_ = span;
	}

private:
	frg::string<KernelAlloc> tag_;
	frg::string<KernelAlloc> descriptiveTag_;
	frg::span<std::byte> writable_;
	frg::span<const std::byte> readable_;
};

initgraph::Stage *getIoChannelsDiscoveredStage();

void publishIoChannel(smarter::shared_ptr<KernelIoChannel> channel);

// Completes once a channel with the given tag is published; parks forever if none ever is.
coroutine<smarter::shared_ptr<KernelIoChannel>> solicitIoChannel(frg::string_view tag);

// Helper function to drain a ring buffer to an I/O channel.
// Records larger than maxRecordSize are truncated.
coroutine<void> dumpRingToChannel(LogRingBuffer *ringBuffer,
		smarter::shared_ptr<KernelIoChannel> channel, size_t maxRecordSize);

} // namespace thor
