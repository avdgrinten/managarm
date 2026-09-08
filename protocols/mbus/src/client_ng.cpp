
#include <iostream>

#include <protocols/mbus/client.hpp>
#include <protocols/posix/supercalls.hpp>
#include <protocols/posix/data.hpp>
#include <bragi/helpers-std.hpp>
#include <bragi/helpers-all.hpp>
#include "helix/ipc-structs.hpp"
#include "helix/ipc.hpp"
#include "mbus.bragi.hpp"

#include <span>

namespace {
	HelHandle getMbusClientLane() {
		posix::ManagarmProcessData data;

		HEL_CHECK(helSyscall2(
			kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&data),
			sizeof(posix::ManagarmProcessData)
		));

		return data.mbusLane;
	}

	bool recreateInstance = false;

	static mbus_ng::Instance makeGlobal() {
		return mbus_ng::Instance(helix::BorrowedLane(getMbusClientLane()).dup());
	}
} // namespace anonymous

namespace mbus_ng {

void recreateInstance() {
	::recreateInstance = true;
}

Instance Instance::global() {
	static Instance instance{makeGlobal()};

	if (::recreateInstance)
		instance = makeGlobal();

	return instance;
}

async::result<Entity> Instance::getEntity(int64_t id) {
	co_return Entity{connection_, id};
}

managarm::mbus::AnyItem encodeItem(mbus_ng::AnyItem item) {
	managarm::mbus::AnyItem ret{};

	if(std::holds_alternative<StringItem>(item)) {
		ret.set_type(managarm::mbus::ItemType::STRING);
		ret.set_string_item({std::get<StringItem>(item).value});
	} else if(std::holds_alternative<ArrayItem>(item)) {
		ret.set_type(managarm::mbus::ItemType::ARRAY);
		auto arr = std::get<ArrayItem>(item);
		for(auto &arr_item : arr.items) {
			ret.add_items(encodeItem(arr_item));
		}
	} else {
		assert(!"unimplemented mbus item type!");
	}

	return ret;
}

mbus_ng::AnyItem decodeItem(managarm::mbus::AnyItem item) {
	mbus_ng::AnyItem ret{};

	switch(item.type()) {
		case managarm::mbus::ItemType::STRING: {
			return StringItem{item.string_item()};
		}
		case managarm::mbus::ItemType::ARRAY: {
			ArrayItem ret;

			for(auto &arr_item : item.items()) {
				ret.items.push_back(decodeItem(arr_item));
			}

			return ret;
		}
		default:
			assert(!"unhandled item type in decode");
	}
}

async::result<Result<EntityManager>>
Instance::createEntity(std::string_view name, const Properties &properties) {
	managarm::mbus::CreateObjectRequest req;
	req.set_name(std::string{name});

	for(auto &[name, value] : properties) {
		managarm::mbus::Property prop;
		prop.set_name(name);
		prop.set_item(encodeItem(value));
		req.add_properties(prop);
	}

	auto [offer, sendHead, sendTail, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullLane.error());

	auto maybeResp = bragi::parse_head_only<managarm::mbus::CreateObjectResponse>(recvResp);
	recvResp.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return EntityManager{resp.id(), pullLane.descriptor()};
}

// ------------------------------------------------------------------------
// mbus Entity class.
// ------------------------------------------------------------------------

async::result<Result<Properties>> Entity::getProperties() const {
	managarm::mbus::GetPropertiesRequest req;
	req.set_id(id_);

	auto [offer, sendReq, recvHead] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	auto conversation = offer.descriptor();

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::GetPropertiesResponse>)
		co_return Error::protocolViolation;

	auto maybeResp = bragi::parse_head_only<managarm::mbus::GetPropertiesResponse>(recvHead);
	recvHead.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvTail.error());

	bragi::limited_reader reader{tail.data(), tail.size()};
	if (!resp.decode_tail(reader))
		co_return Error::protocolViolation;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	Properties properties;
	for(auto &kv : resp.properties())
		properties.insert({ kv.name(), decodeItem(kv.item()) });

	co_return properties;
}

async::result<Result<helix::UniqueLane>> Entity::getRemoteLane() const {
	managarm::mbus::GetRemoteLaneRequest req;
	req.set_id(id_);

	auto [offer, sendReq, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullLane.error());

	auto maybeResp = bragi::parse_head_only<managarm::mbus::GetRemoteLaneResponse>(recvResp);
	recvResp.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return pullLane.descriptor();
}

// ------------------------------------------------------------------------
// mbus EntityManager class.
// ------------------------------------------------------------------------

async::result<Result<void>> EntityManager::serveRemoteLane(helix::UniqueLane lane) const {
	managarm::mbus::ServeRemoteLaneRequest req;

	auto [offer, sendReq, pushLane, recvResp] =
		co_await helix_ng::exchangeMsgs(
			mgmtLane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::pushDescriptor(lane, kHelRightInvoke | kHelRightManage),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(pushLane.error());
	HEL_CHECK(recvResp.error());

	auto preamble = bragi::read_preamble(recvResp);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::ServeRemoteLaneResponse>)
		co_return Error::protocolViolation;

	auto maybeResp = bragi::parse_head_only<managarm::mbus::ServeRemoteLaneResponse>(recvResp);
	recvResp.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return frg::success;
}

async::result<Error> EntityManager::updateProperties(Properties properties) {
	managarm::mbus::UpdatePropertiesRequest req;
	for(auto &[name, value] : properties) {
		managarm::mbus::Property prop;
		prop.set_name(name);
		prop.set_item(encodeItem(value));
		req.add_properties(prop);
	}
	assert(req.properties_size());

	auto [offer, sendHead, sendTail, recvResp] =
		co_await helix_ng::exchangeMsgs(
			mgmtLane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::mbus::UpdatePropertiesResponse>(recvResp);
	recvResp.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return Error::success;
}

// ------------------------------------------------------------------------
// mbus Enumerator class.
// ------------------------------------------------------------------------

static const managarm::mbus::AnyFilter encodeFilter(const AnyFilter &filter) {
	managarm::mbus::AnyFilter flt;

	if(auto alt = std::get_if<EqualsFilter>(&filter); alt) {
		flt.set_type(managarm::mbus::FilterType::EQUALS);
		flt.set_path(alt->path());
		flt.set_value(alt->value());
	}else if(auto alt = std::get_if<Conjunction>(&filter); alt) {
		flt.set_type(managarm::mbus::FilterType::CONJUNCTION);
		for(auto &operand : alt->operands()) {
			flt.add_operands(encodeFilter(operand));
		}
	}else if(auto alt = std::get_if<Disjunction>(&filter); alt) {
		flt.set_type(managarm::mbus::FilterType::DISJUNCTION);
		for(auto &operand : alt->operands()) {
			flt.add_operands(encodeFilter(operand));
		}
	}else{
		throw std::runtime_error("Unexpected filter type");
	}

	return flt;
}

async::result<Result<EnumerationResult>> Enumerator::nextEvents() {
	managarm::mbus::EnumerateRequest req;
	req.set_seq(curSeq_);
	req.set_filter(encodeFilter(filter_));

	auto [offer, sendHead, sendTail, recvRespHead] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvRespHead.error());

	auto conversation = offer.descriptor();

	auto preamble = bragi::read_preamble(recvRespHead);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::EnumerateResponse>)
		co_return Error::protocolViolation;

	auto maybeResp = bragi::parse_head_only<managarm::mbus::EnumerateResponse>(recvRespHead);
	recvRespHead.reset();
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvRespTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvRespTail.error());

	bragi::limited_reader reader{tail.data(), tail.size()};
	if (!resp.decode_tail(reader))
		co_return Error::protocolViolation;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	EnumerationResult result{};
	result.paginated = resp.out_seq() != resp.actual_seq();

	for (auto &entity : resp.entities()) {
		using enum EnumerationEvent::Type;

		auto [_, unseen] = seenIds_.insert(entity.id());

		EnumerationEvent event{};
		event.id = entity.id();
		event.name = entity.name();
		event.type = unseen ? created : propertiesChanged;
		for(auto &kv : entity.properties())
			event.properties.insert({ kv.name(), decodeItem(kv.item()) });

		result.events.push_back(std::move(event));
	}

	curSeq_ = resp.out_seq();

	co_return result;
}

// ------------------------------------------------------------------------
// mbus Conjunction class.
// ------------------------------------------------------------------------

Conjunction::Conjunction(std::vector<AnyFilter> &&operands)
	: operands_{std::move(operands)} {

}

std::vector<AnyFilter> &Conjunction::operands() & {
	return operands_;
}

const std::vector<AnyFilter> &Conjunction::operands() const & {
	return operands_;
}

// ------------------------------------------------------------------------
// mbus Disjunction class.
// ------------------------------------------------------------------------

Disjunction::Disjunction(std::vector<AnyFilter> &&operands)
	: operands_{std::move(operands)} {

}

std::vector<AnyFilter> &Disjunction::operands() & {
	return operands_;
}

const std::vector<AnyFilter> &Disjunction::operands() const & {
	return operands_;
}

}  // namespace mbus_ng
