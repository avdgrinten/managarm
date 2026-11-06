#include <frg/string.hpp>

#include <thor-internal/universe.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kerncfg.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/kernel-device.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/user-io.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/elf-notes.hpp>
#include <eir/interface.hpp>

#include <bragi/helpers-frigg.hpp>
#include <bragi/helpers-all.hpp>
#include "kerncfg.frigg_bragi.hpp"

#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern frg::manual_box<LogRingBuffer> allocLog;
extern ManagarmElfNote<AcpiData> acpiRsdpNote;
#ifdef THOR_HAS_DTB_SUPPORT
extern ManagarmElfNote<DtData> dtDataNote;
#endif

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

coroutine<frg::expected<Error>> sendSvrResponse(
		smarter::shared_ptr<Stream, LanePolicy> &lane, managarm::kerncfg::Error error) {
	managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(error);

	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
	bragi::write_head_only(resp, respBuffer);
	auto respError = co_await sendBuffer(lane, std::move(respBuffer));
	if(respError != Error::success)
		co_return respError;
	co_return frg::success;
}

struct KerncfgBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "kerncfg"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("kerncfg", std::move(properties))).unwrap();
	}

private:
	coroutine<frg::expected<Error>> handleRequest(smarter::shared_ptr<Stream, LanePolicy> boundLane) override {
		auto [acceptError, lane] = co_await accept(boundLane);
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await recvBuffer(lane);
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if (preamble.error())
			co_return Error::protocolViolation;

		if(preamble.id() == bragi::message_id<managarm::kerncfg::GetCmdlineRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetCmdlineRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);
			auto cmdline = getKernelCmdline();
			resp.set_size(cmdline.size());

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;

			frg::unique_memory<KernelAlloc> cmdlineBuffer{*kernelAlloc, cmdline.size()};
			memcpy(cmdlineBuffer.data(), cmdline.data(), cmdline.size());
			auto cmdlineError = co_await sendBuffer(lane, std::move(cmdlineBuffer));
			if(cmdlineError != Error::success)
				co_return cmdlineError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::GetMemoryInformationRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetMemoryInformationRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::GetMemoryInformationResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);
			resp.set_total_usable_memory(physicalAllocator->numTotalPages());
			resp.set_available_memory(physicalAllocator->numFreePages());
			resp.set_memory_unit(kPageSize);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::GetNumCpuRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetNumCpuRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				co_return Error::protocolViolation;
			}

			managarm::kerncfg::GetNumCpuResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);
			resp.set_num_cpu(getCpuCount());

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success) {
				co_return respError;
			}
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::GetAcpiRsdpRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetAcpiRsdpRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::GetAcpiRsdpResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);
			resp.set_rsdp(acpiRsdpNote->rsdp);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::ListKernelDevicesRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::ListKernelDevicesRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::ListKernelDevicesResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);

			for (auto device : allKernelDevices()) {
				managarm::kerncfg::KernelDevice<KernelAlloc> msg{*kernelAlloc};
				msg.set_id(device->id());
				msg.set_name(frg::string<KernelAlloc>{*kernelAlloc, device->name()});
				resp.add_devices(std::move(msg));

				for (auto &resource : device->resources()) {
					managarm::kerncfg::KernelDeviceResource<KernelAlloc> msg{*kernelAlloc};
					msg.set_device(device->id());
					msg.set_kind(resource.kind == KernelDeviceResourceKind::io
							? managarm::kerncfg::KernelDeviceResourceKind::IO
							: managarm::kerncfg::KernelDeviceResourceKind::MMIO);
					msg.set_base(resource.base);
					msg.set_length(resource.length);
					resp.add_resources(std::move(msg));
				}

				if (auto &fb = device->framebuffer()) {
					managarm::kerncfg::KernelFramebuffer<KernelAlloc> msg{*kernelAlloc};
					msg.set_device(device->id());
					msg.set_pitch(fb->pitch);
					msg.set_width(fb->width);
					msg.set_height(fb->height);
					msg.set_bpp(fb->bpp);
					msg.set_fb_type(fb->type);
					resp.add_framebuffers(std::move(msg));
				}
			}

			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc, resp.size_of_head()};
			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc, resp.size_of_tail()};
			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);
			auto respHeadError = co_await sendBuffer(lane, std::move(respHeadBuffer));
			if(respHeadError != Error::success)
				co_return respHeadError;
			auto respTailError = co_await sendBuffer(lane, std::move(respTailBuffer));
			if(respTailError != Error::success)
				co_return respTailError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::SuspendKernelDeviceRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::SuspendKernelDeviceRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			auto error = managarm::kerncfg::Error::NO_SUCH_DEVICE;
			if (auto device = findKernelDevice(req->id())) {
				co_await suspendKernelDevice(device);
				error = managarm::kerncfg::Error::SUCCESS;
			}
			FRG_CO_TRY(co_await sendSvrResponse(lane, error));
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::ResumeKernelDeviceRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::ResumeKernelDeviceRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			auto error = managarm::kerncfg::Error::NO_SUCH_DEVICE;
			if (auto device = findKernelDevice(req->id())) {
				error = resumeKernelDevice(device)
						? managarm::kerncfg::Error::SUCCESS
						: managarm::kerncfg::Error::ILLEGAL_REQUEST;
			}
			FRG_CO_TRY(co_await sendSvrResponse(lane, error));
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::ClaimKernelDeviceRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::ClaimKernelDeviceRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			auto error = managarm::kerncfg::Error::NO_SUCH_DEVICE;
			if (auto device = findKernelDevice(req->id())) {
				co_await claimKernelDevice(device);
				error = managarm::kerncfg::Error::SUCCESS;
			}
			FRG_CO_TRY(co_await sendSvrResponse(lane, error));
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::GetDeviceTreeRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetDeviceTreeRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::GetDeviceTreeResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);
#ifdef THOR_HAS_DTB_SUPPORT
			resp.set_address(dtDataNote->address);
			resp.set_size(dtDataNote->size);
#else
			resp.set_address(0);
			resp.set_size(0);
#endif

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::GetHierarchyRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::GetHierarchyRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::GetHierarchyResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);

			for(auto &node : snapshotHierarchies()) {
				managarm::kerncfg::HierarchyNode<KernelAlloc> item(*kernelAlloc);
				item.set_id(node.id);
				item.set_parent_id(node.parentId);
				item.set_charged_memory(node.chargedMemory);
				item.set_charged_swap(node.chargedSwap);
				item.set_charged_kernel(node.chargedKernel);
				item.set_tag(std::move(node.tag));
				resp.add_nodes(std::move(item));
			}

			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc, resp.head_size};
			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc, resp.size_of_tail()};
			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await sendBuffer(lane, std::move(respHeadBuffer));
			if(respHeadError != Error::success)
				co_return respHeadError;
			auto respTailError = co_await sendBuffer(lane, std::move(respTailBuffer));
			if(respTailError != Error::success)
				co_return respTailError;
		}else if(preamble.id() == bragi::message_id<managarm::kerncfg::ProvideIoChannelRequest>) {
			auto req = bragi::parse_head_only<managarm::kerncfg::ProvideIoChannelRequest>(reqBuffer, *kernelAlloc);

			if (!req)
				co_return Error::protocolViolation;

			managarm::kerncfg::ProvideIoChannelResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;

			FRG_CO_TRY(co_await UserIoChannel::provide(lane,
					frg::string<KernelAlloc>{*kernelAlloc, req->tag()},
					frg::string<KernelAlloc>{*kernelAlloc, req->descriptive_tag()},
					req->has_input() != 0));
		}else{
			managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}

		co_return frg::success;
	}
};

struct ByteRingBusObject : private KernelBusObject {
	ByteRingBusObject(LogRingBuffer *buffer, frg::string_view purpose)
	: buffer_{buffer}, purpose_{purpose} { }

	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "kerncfg-byte-ring"));
		properties.stringProperty("purpose", frg::string<KernelAlloc>(*kernelAlloc, purpose_));

		// TODO(qookie): Better error handling here.
		(co_await createObject("kerncfg-byte-ring", std::move(properties))).unwrap();
	}

private:
	LogRingBuffer *buffer_;
	frg::string_view purpose_;

	coroutine<frg::expected<Error>> handleRequest(smarter::shared_ptr<Stream, LanePolicy> boundLane) override {
		auto [acceptError, lane] = co_await accept(boundLane);
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await recvBuffer(lane);
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if (preamble.error())
			co_return Error::protocolViolation;

		if(preamble.id() == bragi::message_id<managarm::kerncfg::GetBufferContentsRequest>) {
			auto maybeReq = bragi::parse_head_only<managarm::kerncfg::GetBufferContentsRequest>(reqBuffer, *kernelAlloc);

			if (!maybeReq)
				co_return Error::protocolViolation;

			auto &req = *maybeReq;
			managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::SUCCESS);

			frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc, req.size()};

			size_t progress = 0;

			// Extract the first record. We stop on success.
			uint64_t effectivePtr;
			uint64_t currentPtr;
			while(true) {
				auto [success, recordPtr, nextPtr, actualSize] = buffer_->dequeueAt(
						req.dequeue(), dataBuffer.data(), req.size());
				if(success) {
					assert(actualSize); // For now, we do not support size zero records.
					if(actualSize == req.size())
						infoLogger() << "thor: kerncfg truncates a ring buffer record" << frg::endlog;
					effectivePtr = recordPtr;
					currentPtr = nextPtr;
					progress += actualSize;
					break;
				}

				if(!(req.flags() & managarm::kerncfg::GetBufferContentsFlags::NO_WAIT))
					co_await buffer_->wait(nextPtr);
				else {
					resp.set_error(managarm::kerncfg::Error::WOULD_BLOCK);
					break;
				}
			}

			if(!(req.flags() & managarm::kerncfg::GetBufferContentsFlags::ONE_RECORD)) {
				// Extract further records. We stop on failure, or if we miss records.
				while(true) {
					auto [success, recordPtr, nextPtr, actualSize] = buffer_->dequeueAt(
							currentPtr, static_cast<std::byte *>(dataBuffer.data()) + progress,
							req.size() - progress);
					if(recordPtr != currentPtr)
						break;
					if(success) {
						assert(actualSize); // For now, we do not support size zero records.
						if(actualSize == req.size() - progress)
							break;
						currentPtr = nextPtr;
						progress += actualSize;
						continue;
					}

					if(progress >= req.watermark() || req.flags() & managarm::kerncfg::GetBufferContentsFlags::NO_WAIT)
						break;

					co_await buffer_->wait(nextPtr);
				}
			}

			resp.set_size(progress);
			resp.set_effective_dequeue(effectivePtr);
			resp.set_new_dequeue(currentPtr);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;

			auto dataError = co_await sendBuffer(lane, std::move(dataBuffer));

			if(dataError != Error::success)
				co_return dataError;
		}else{
			managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
			bragi::write_head_only(resp, respBuffer);
			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}

		co_return frg::success;
	}
};

} // anonymous namespace

void initializeKerncfg() {
	auto kerncfg = frg::construct<KerncfgBusObject>(*kernelAlloc);
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), kerncfg->run());

	{
		auto ring = frg::construct<ByteRingBusObject>(*kernelAlloc, getGlobalKmsgRing(), "kernel-log");
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ring->run());
	}

#ifdef KERNEL_LOG_ALLOCATIONS
	auto ring = frg::construct<ByteRingBusObject>(*kernelAlloc, allocLog.get(), "heap-trace");
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ring->run());
#endif

	if (wantKernelProfile) {
		if(auto *profileRing = getGlobalProfileRing()) {
			auto ring = frg::construct<ByteRingBusObject>(*kernelAlloc, profileRing, "kernel-profile");
			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ring->run());
		}
	}

	if (wantOsTrace) {
		auto ring = frg::construct<ByteRingBusObject>(*kernelAlloc, getGlobalOsTraceRing(), "os-trace");
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ring->run());
	}
}

} // namespace thor
