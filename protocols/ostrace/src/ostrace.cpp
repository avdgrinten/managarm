#include <async/oneshot-event.hpp>
#include <bragi/helpers-std.hpp>
#include <frg/std_compat.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <ostrace.bragi.hpp>

namespace protocols::ostrace {

Context::Context(Vocabulary &vocabulary)
: vocabulary_{&vocabulary}, enabled_{false} { }

async::result<void> Context::create() {
	assert(!lane_);

	// Find ostrace in mbus.
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "ostrace"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	std::cout << "ostrace: Found ostrace" << std::endl;
	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	lane_ = (co_await entity.getRemoteLane()).unwrap();

	// Perform the negotiation request.

	managarm::ostrace::NegotiateReq req;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	recvResp.reset();
	assert(maybeResp);
	auto &resp = maybeResp.value();

	if(resp.error() == managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED)
		co_return;
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);

	// Define all terms before enabling emission; otherwise, a concurrent emit could
	// observe an active context whose terms have not been announced yet.
	for (auto *term : vocabulary_->terms())
		co_await define(term);

	enabled_ = true;

	async::detach(run_());
}

async::result<ItemId> Context::announceItem_(std::string_view name) {
	managarm::ostrace::AnnounceItemReq req;
	req.set_name(std::string{name});

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	recvResp.reset();
	assert(maybeResp);
	auto &resp = maybeResp.value();
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);

	co_return ItemId{resp.id()};
}

async::result<void> Context::run_() {
	if(!enabled_)
		co_return;

	while (true) {
		auto maybeBuffer = co_await queue_.async_get();
		assert(maybeBuffer);

		managarm::ostrace::EmitReq req;

		auto [offer, sendReq, sendData, recvResp] =
			co_await helix_ng::exchangeMsgs(
				lane_,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::sendBuffer(maybeBuffer->data(), maybeBuffer->size()),
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendData.error());
		HEL_CHECK(recvResp.error());

		auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
		recvResp.reset();
		assert(maybeResp);
		auto &resp = maybeResp.value();
		assert(resp.error() == managarm::ostrace::Error::SUCCESS);
	}
}

} // namespace protocols::ostrace
