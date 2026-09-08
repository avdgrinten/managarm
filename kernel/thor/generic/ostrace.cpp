#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/cmdline.hpp>
#include <frg/span.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-ostrace.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/mbus.hpp>

// --------------------------------------------------------------------------------------
// Core ostrace implementation.
// --------------------------------------------------------------------------------------

namespace thor {

bool wantOsTrace = false;

constinit std::atomic<bool> osTraceInUse{false};

initgraph::Stage *getOsTraceAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.ostrace-available"};
	return &s;
}

namespace {

// Upper bound on the size of pages that sources submit.
constexpr size_t maxPageSize = 64 * 1024;
constexpr size_t frameHeadSize = managarm::ostrace::Frame<KernelAlloc>::head_size;
constexpr size_t maxFrameSize = frameHeadSize + maxPageSize;

// Source 0 is the kernel itself.
std::atomic<uint64_t> nextSource{1};
std::atomic<uint64_t> nextFrameSeq{0};
frg::manual_box<LogRingBuffer> globalOsTraceRing;

initgraph::Task initOsTraceCore{&globalInitEngine, "generic.init-ostrace-core",
	// ostrace::setup() launches a fiber.
	initgraph::Requires{getFibersAvailableStage()},
	initgraph::Entails{getOsTraceAvailableStage()},
	[] {
		frg::array args = {
			frg::option{"ostrace", frg::store_true(wantOsTrace)},
			frg::option{"ostrace.metrics-interval", frg::as_number(ostrace::metricsInterval)},
		};
		frg::parse_arguments(getKernelCmdline(), args);

		infoLogger() << "thor: ostrace is " << (wantOsTrace ? "enabled" : "disabled") << frg::endlog;
		if(!wantOsTrace)
			return;

		// Sources flush whole pages, so the ring needs to absorb bursts of pages.
		void *osTraceMemory = kernelAlloc->allocate(1 << 22);
		globalOsTraceRing.initialize(reinterpret_cast<uintptr_t>(osTraceMemory), 1 << 22);

		osTraceInUse.store(true);

		ostrace::setup();
	}
};

} // anonymous namespace

// Writes a page of the given source to the ring, preceded by a kernel-owned Frame.
// Frames are not assembled in a heap buffer since allocations of their size bypass the slabs of
// the kernel heap.
void commitFrame(uint64_t source, uint64_t writer, uint64_t firstEvent, uint64_t numEvents,
		frg::span<const char> page) {
	if(!osTraceInUse.load(std::memory_order_relaxed))
		return;

	size_t size = page.size();
	assert(size <= maxPageSize);

	managarm::ostrace::Frame<KernelAlloc> frame{*kernelAlloc};
	frame.set_source(source);
	frame.set_seq(nextFrameSeq.fetch_add(1, std::memory_order_relaxed));
	frame.set_size(size);
	frame.set_writer(writer);
	frame.set_first_event(firstEvent);
	frame.set_num_events(numEvents);

	char head[frameHeadSize];
	bool encodeSuccess = bragi::write_head_only(frame, frg::span<char>(head, frameHeadSize));
	assert(encodeSuccess);

	frg::span<const char> pieces[2] = {{head, frameHeadSize}, page};

	// We want to be able to call this function from any context, but we cannot wake the waiters
	// in all contexts. For now, only wake waiters if IRQs are enabled.
	globalOsTraceRing->enqueue({pieces, 2}, !intsAreEnabled());
}

LogRingBuffer *getGlobalOsTraceRing() {
	return globalOsTraceRing.get();
}

// --------------------------------------------------------------------------------------
// mbus object handling.
// --------------------------------------------------------------------------------------

namespace {

struct OstraceBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "ostrace"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("ostrace", std::move(properties))).unwrap();
	}

private:
	// Each client is a separate source. Since requests of a client are handled sequentially,
	// the pages of a source reach the ring in the order in which they were submitted.
	coroutine<void> serveClient(smarter::shared_ptr<Stream, LanePolicy> lane) override {
		auto source = nextSource.fetch_add(1, std::memory_order_relaxed);
		while(true) {
			auto result = co_await handleSourceRequest(lane, source);

			if (!result && result.error() == Error::endOfLane)
				break;

			if(!result)
				infoLogger() << "thor: failed to handle ostrace request with error "
						<< static_cast<int>(result.error()) << frg::endlog;
		}
	}

	coroutine<frg::expected<Error>> handleSourceRequest(smarter::shared_ptr<Stream, LanePolicy> boundLane,
			uint64_t source) {
		auto [acceptError, lane] = co_await accept(boundLane);
		if(acceptError == Error::endOfLane)
			co_return Error::endOfLane;
		if(acceptError != Error::success) {
			assert(isRemoteIpcError(acceptError));
			co_return Error::protocolViolation;
		}

		auto [reqError, reqBuffer] = co_await recvBuffer(lane);
		if(reqError != Error::success) {
			assert(isRemoteIpcError(reqError));
			co_return Error::protocolViolation;
		}
		frg::span<const char> reqSpan{reinterpret_cast<const char *>(reqBuffer.data()),
				reqBuffer.size()};

		auto preamble = bragi::read_preamble(reqSpan);
		if(preamble.error())
			co_return Error::protocolViolation;

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::NegotiateReq>: {
			auto maybeReq = bragi::parse_head_only<managarm::ostrace::NegotiateReq>(
					reqSpan, *kernelAlloc);
			if(!maybeReq)
				co_return Error::protocolViolation;

			managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
			if(wantOsTrace) {
				resp.set_error(managarm::ostrace::Error::SUCCESS);
			}else{
				resp.set_error(managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED);
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success) {
				assert(isRemoteIpcError(respError));
				co_return Error::protocolViolation;
			}
		} break;
		case bragi::message_id<managarm::ostrace::EmitReq>: {
			auto maybeReq = bragi::parse_head_only<managarm::ostrace::EmitReq>(
					reqSpan, *kernelAlloc);
			if(!maybeReq)
				co_return Error::protocolViolation;
			auto &req = maybeReq.value();

			auto [dataError, dataBuffer] = co_await recvBuffer(lane);
			if(dataError != Error::success) {
				assert(isRemoteIpcError(dataError));
				co_return Error::protocolViolation;
			}

			managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
			if (!wantOsTrace) {
				resp.set_error(managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED);
			}else if(dataBuffer.size() > maxPageSize) {
				resp.set_error(managarm::ostrace::Error::ILLEGAL_REQUEST);
			}else{
				commitFrame(source, req.writer(), req.first_event(), req.num_events(),
						{reinterpret_cast<char *>(dataBuffer.data()), dataBuffer.size()});
				resp.set_error(managarm::ostrace::Error::SUCCESS);
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success) {
				assert(isRemoteIpcError(respError));
				co_return Error::protocolViolation;
			}
		} break;
		default:
			managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::ostrace::Error::ILLEGAL_REQUEST);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success) {
				assert(isRemoteIpcError(respError));
				co_return Error::protocolViolation;
			}
		}

		co_return frg::success;
	}
};

initgraph::Task initOsTraceMbus{&globalInitEngine, "generic.init-ostrace-sinks",
	initgraph::Requires{&initOsTraceCore,
		getFibersAvailableStage(),
		getIoChannelsDiscoveredStage()},
	[] {
		// Create a fiber to manage requests to the ostrace mbus object.
		KernelFiber::run([=] {
			// We unconditionally create the mbus object since userspace might use it.
			auto ostrace = frg::construct<OstraceBusObject>(*kernelAlloc);
			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ostrace->run());

			// Only dump to an I/O channel if ostrace is supported (otherwise, the ring buffer
			// does not even exist).
			if(wantOsTrace) {
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(),
						[] () -> coroutine<void> {
					auto channel = co_await solicitIoChannel("ostrace");
					infoLogger() << "thor: Connecting ostrace to I/O channel" << frg::endlog;
					co_await dumpRingToChannel(globalOsTraceRing.get(), std::move(channel), maxFrameSize);
				}());
			}
		});
	}
};

} // anonymous namespace

} // namespace thor
