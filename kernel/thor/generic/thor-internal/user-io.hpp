#pragma once

#include <frg/optional.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/ringbuf.hpp>

namespace thor {

// KernelIoChannel whose transport is a userspace server. Output and input travel through
// rings of the ringbuf protocol; the server connects to them via svrctl's ProvideIoChannel.
struct UserIoChannel final : KernelIoChannel {
private:
	struct CtorToken {};

public:
	static constexpr size_t maxRingSize = size_t{16} << 20;

	// Ring sizes must be page-aligned powers of two; inRingSize may be zero.
	static std::expected<smarter::shared_ptr<UserIoChannel>, Error> create(
			frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag,
			size_t outRingSize, size_t inRingSize);

	UserIoChannel(CtorToken, frg::string<KernelAlloc> tag,
			frg::string<KernelAlloc> descriptiveTag);

	// Pushes the rings to the server; must complete before the channel is used.
	coroutine<frg::expected<Error>> provide(smarter::shared_ptr<Stream, LanePolicy> conversation);

	void produceOutput(size_t n) override;
	void consumeInput(size_t n) override;
	coroutine<frg::expected<Error>> issueIo(IoFlags flags) override;

private:
	void updateSpans_();

	// Output is produced by the kernel and consumed by the server, input vice versa.
	KernelRing outRing_;
	frg::optional<KernelRing> inRing_;
	protocols::ringbuf::ProducerCore outCore_;
	protocols::ringbuf::ConsumerCore inCore_;
	RingKickLanes lanes_;
};

} // namespace thor
