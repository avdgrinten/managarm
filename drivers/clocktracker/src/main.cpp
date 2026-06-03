
#include <iostream>
#include <protocols/posix/procdata.hpp>

#include <async/oneshot-event.hpp>
#include <core/dispatch.hpp>
#include <helix/memory.hpp>
#include <protocols/clock/defs.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>
#include <clock.bragi.hpp>

// ----------------------------------------------------------------------------
// RTC handling.
// ----------------------------------------------------------------------------

// Pair of (reference clock, rtc time).
using RtcTime = std::pair<int64_t, int64_t>;

helix::UniqueLane rtcLane;
async::oneshot_event foundRtc;

async::result<void> enumerateRtc() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "rtc"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	rtcLane = (co_await entity.getRemoteLane()).unwrap();
}

async::result<RtcTime> getRtcTime() {
	managarm::clock::GetRtcTimeRequest req;

	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		rtcLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = *bragi::parse_head_only<managarm::clock::SvrResponse>(recvResp);
	recvResp.reset();
	assert(resp.error() == managarm::clock::Error::SUCCESS);

	co_return RtcTime{resp.ref_nanos(), resp.rtc_nanos()};
}

// ----------------------------------------------------------------------------
// Tracker page handling.
// ----------------------------------------------------------------------------

helix::UniqueDescriptor trackerPageMemory;
helix::Mapping trackerPageMapping;

TrackerPage *accessPage() {
	return reinterpret_cast<TrackerPage *>(trackerPageMapping.get());
}

// ----------------------------------------------------------------------------
// clocktracker mbus interface.
// ----------------------------------------------------------------------------

struct HandleRequest {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::clock::AccessPageRequest &&, helix::BorrowedDescriptor conversation, bragi::preamble) {
		managarm::clock::SvrResponse resp;
		resp.set_error(managarm::clock::Error::SUCCESS);

		auto [sendResp, sendMemory] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(trackerPageMemory)
		);
		HEL_CHECK(sendResp.error());
		HEL_CHECK(sendMemory.error());
		co_return {};
	}
};

async::detached serve(helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::clock::AccessPageRequest
		>(lane, HandleRequest{});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "clocktracker: dispatch error" << std::endl;
			continue;
		}
	}
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

async::detached initializeDriver() {
	// Find an RTC on the mbus.

	// TODO
#if !defined(__aarch64__) && !defined(__riscv)
	co_await enumerateRtc();
#endif

	// Allocate and map our tracker page.
	size_t page_size = 4096;
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), page_size, 0, nullptr, &handle));
	trackerPageMemory = helix::UniqueDescriptor{handle};
	trackerPageMapping = helix::Mapping{trackerPageMemory, 0, page_size};

	// Initialize the tracker page.
	auto page = accessPage();
	memset(page, 0, page_size);

	// Read the RTC to initialize the realtime clock.
	// TODO
#if defined(__aarch64__) || defined(__riscv)
	auto result = RtcTime{0, 0};
#else
	auto result = co_await getRtcTime(); // TODO: Use the seqlock.
#endif

	std::cout << "drivers/clocktracker: Initializing time to "
			<< std::get<1>(result) << std::endl;
	accessPage()->refClock = std::get<0>(result);
	accessPage()->baseRealtime = std::get<1>(result);

	// Create an mbus object for the device.
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"clocktracker"}},
	};

	auto entity = (co_await mbus_ng::Instance::global().createEntity(
		"clocktracker", descriptor)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serve(std::move(localLane));
		}
	}(std::move(entity));
}

int main() {
	std::cout << "drivers/clocktracker: Starting driver" << std::endl;

	{
		initializeDriver();
	}

	async::run_forever(helix::currentDispatcher);

	return 0;
}


