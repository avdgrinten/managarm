#include <frg/string.hpp>

#include "descriptor.hpp"
#include "execution/coroutine.hpp"
#include "fiber.hpp"
#include "kerncfg.hpp"
#include "service_helpers.hpp"

#include "kerncfg.frigg_pb.hpp"
#include "mbus.frigg_pb.hpp"

#include <generic/ring-buffer.hpp>

namespace thor {

extern frigg::LazyInitializer<LaneHandle> mbusClient;
extern frigg::LazyInitializer<frg::string<KernelAlloc>> kernelCommandLine;
extern frigg::LazyInitializer<LogRingBuffer> allocLog;

namespace {

coroutine<Error> handleReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(!reqError && "Unexpected mbus transaction");
	managarm::kerncfg::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::kerncfg::CntReqType::GET_CMDLINE) {
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::SUCCESS);
		resp.set_size(kernelCommandLine->size());

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(!respError && "Unexpected mbus transaction");
		frigg::UniqueMemory<KernelAlloc> cmdlineBuffer{*kernelAlloc, kernelCommandLine->size()};
		memcpy(cmdlineBuffer.data(), kernelCommandLine->data(), kernelCommandLine->size());
		auto cmdlineError = co_await SendBufferSender{lane, std::move(cmdlineBuffer)};
		assert(!cmdlineError && "Unexpected mbus transaction");
	}else{
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(!respError && "Unexpected mbus transaction");
	}

	co_return kErrSuccess;
}

coroutine<Error> handleByteRingReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(!reqError && "Unexpected mbus transaction");
	managarm::kerncfg::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::kerncfg::CntReqType::GET_BUFFER_CONTENTS) {
		size_t oldDequeue = req.dequeue();
		size_t wantedSize = allocLog->wantedSize(oldDequeue, req.size());;

		constexpr uint64_t nanos = 100000000;
		constexpr size_t minSize = 1024 * 1024; // 1M
		while (!allocLog->hasEnoughBytes(oldDequeue, minSize))
			co_await generalTimerEngine()->sleep(systemClockSource()->currentNanos() + nanos);

		frigg::UniqueMemory<KernelAlloc> dataBuffer{*kernelAlloc, wantedSize};
		auto [newDequeue, actualSize] =
			allocLog->dequeueInto(dataBuffer.data(), oldDequeue, wantedSize);

		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::SUCCESS);
		resp.set_size(actualSize);
		resp.set_new_dequeue(newDequeue);
		resp.set_enqueue(allocLog->enqueueIndex());

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(!respError && "Unexpected mbus transaction");
		auto cmdlineError = co_await SendBufferSender{lane, std::move(dataBuffer)};
		assert(!cmdlineError && "Unexpected mbus transaction");
	}else{
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(!respError && "Unexpected mbus transaction");
	}

	co_return kErrSuccess;
}


} // anonymous namespace

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

coroutine<void> handleBind(LaneHandle objectLane);
coroutine<void> handleByteRingBind(LaneHandle objectLane);

coroutine<void> createObject(LaneHandle mbusLane) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(!offerError && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "kerncfg"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(!reqError && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(!respError && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(!objectError && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleBind(objectLane);
}

coroutine<void> createByteRingObject(LaneHandle mbusLane, const char *purpose) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(!offerError && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "kerncfg-byte-ring"));

	managarm::mbus::Property<KernelAlloc> purpose_prop(*kernelAlloc);
	purpose_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "purpose"));
	auto &purpose_item = purpose_prop.mutable_item().mutable_string_item();
	purpose_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, purpose));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));
	req.add_properties(std::move(purpose_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(!reqError && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(!respError && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(!objectError && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleByteRingBind(objectLane);
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(!acceptError && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(!reqError && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(!respError && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(!boundError && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleReq(boundLane);
			if(error == kErrEndOfLane)
				break;
			if(isRemoteIpcError(error))
				frigg::infoLogger() << "thor: Aborting kerncfg request"
						" after remote violated the protocol" << frigg::endLog;
			assert(!error);
		}
	})(boundLane));
}

// TODO: maybe don't completely duplicate this function twice?
coroutine<void> handleByteRingBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(!acceptError && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(!reqError && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(!respError && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(!boundError && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleByteRingReq(boundLane);
			if(error == kErrEndOfLane)
				break;
			if(isRemoteIpcError(error))
				frigg::infoLogger() << "thor: Aborting kerncfg request"
						" after remote violated the protocol" << frigg::endLog;
			assert(!error);
		}
	})(boundLane));
}

} // anonymous namespace

void initializeKerncfg() {
	// Create a fiber to manage requests to the kerncfg mbus object.
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, createObject(*mbusClient));
	});

	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, createByteRingObject(*mbusClient, "heap-trace"));
	});
}

} // namespace thor
