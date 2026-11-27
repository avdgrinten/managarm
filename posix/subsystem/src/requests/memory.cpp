#include "common.hpp"
#include "../memfd.hpp"
#include "../swap.hpp"

#include <sys/mman.h>
#include <linux/memfd.h>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::VmMapRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "VM_MAP", "size={:#x}", req.size());

	// TODO: Validate req.flags().

	if(req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.rel_offset() & (kHelPageSize - 1)) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if((req.flags() & MAP_ANONYMOUS) && req.rel_offset()) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	uint32_t nativeFlags = 0;

	if(req.mode() & PROT_READ)
		nativeFlags |= kHelMapProtRead;
	if(req.mode() & PROT_WRITE)
		nativeFlags |= kHelMapProtWrite;
	if(req.mode() & PROT_EXEC)
		nativeFlags |= kHelMapProtExecute;

	if(req.flags() & MAP_FIXED_NOREPLACE)
		nativeFlags |= kHelMapFixedNoReplace;
	else if(req.flags() & MAP_FIXED)
		nativeFlags |= kHelMapFixed;

	bool copyOnWrite;
	if((req.flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
		copyOnWrite = true;
	}else if((req.flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
		copyOnWrite = false;
	}else{
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	uintptr_t hint = req.address_hint();

	smarter::shared_ptr<File, FileHandle> file;
	if(!(req.flags() & MAP_ANONYMOUS)) {
		file = self->fileContext()->getFile(req.fd());
		if(!file) {
			co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}
	}

	if(req.size() == 0) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}
	// Linux fails with ENOMEM if rounding the size up to pages wraps around.
	if(req.size() > SIZE_MAX - (kHelPageSize - 1)) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::NO_MEMORY);
		co_return {};
	}

	if(req.flags() & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
		if(hint & (kHelPageSize - 1)) {
			co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}else{
		// Like Linux, round unaligned hints down instead of failing.
		hint &= ~uintptr_t(kHelPageSize - 1);
	}

	// Files such as /dev/zero map like anonymous memory; their offset is ignored.
	frg::expected<Error, void *> result;
	if((req.flags() & MAP_ANONYMOUS) || file->mapsAnonymously()) {
		// We round up to page size
		size_t size = req.size();
		if(size & (kHelPageSize - 1)) {
			size = (req.size() + (kHelPageSize - 1)) & ~(kHelPageSize - 1);
		}

		if(copyOnWrite) {
			result = co_await self->vmContext()->mapFile(hint,
					{}, nullptr,
					0, size, true, nativeFlags);
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateSwappableMemory(sharedHierarchy(),
					getSwapSpaceHandle().getHandle(), size, 0, &handle));

			result = co_await self->vmContext()->mapFile(hint,
					helix::UniqueDescriptor{handle}, nullptr,
					0, size, false, nativeFlags);
		}
	}else{
		auto memory = co_await file->accessMemory();
		if(!memory) {
			co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
		result = co_await self->vmContext()->mapFile(hint,
				std::move(memory), std::move(file),
				req.rel_offset(), req.size(), copyOnWrite, nativeFlags);
	}

	if(!result) {
		co_await sendErrorResponse<managarm::posix::VmMapResponse>(conversation,
				result.error() | toPosixProtoError);
		co_return {};
	}

	void *address = result.unwrap();

	managarm::posix::VmMapResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_offset(reinterpret_cast<uintptr_t>(address));

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::MemFdCreateRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "MEMFD_CREATE", "'{}'", req.name());

	if(req.flags() & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
		co_await sendErrorResponse<managarm::posix::MemFdCreateResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto link = MemoryFileLink::makeMemoryFileLink(0777);
	auto memFile = smarter::make_shared<MemoryFile>(nullptr, link, (req.flags() & MFD_ALLOW_SEALING));
	link->setFile(memFile);
	MemoryFile::serve(memFile);
	memFile->setupWeakFile(memFile);
	auto file = File::constructHandle(std::move(memFile));

	int flags = 0;

	if(req.flags() & MFD_CLOEXEC) {
		flags |= managarm::posix::OpenFlags::OF_CLOEXEC;
	}

	managarm::posix::MemFdCreateResponse resp;
	auto fd = self->fileContext()->attachFile(file, flags);

	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
        conversation,
        helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
    );
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::VmRemapRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "VM_REMAP");

	auto address = co_await self->vmContext()->remapFile(
			reinterpret_cast<void *>(req.address()), req.size(), req.new_size());

	managarm::posix::VmRemapResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_offset(reinterpret_cast<uintptr_t>(address));

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::VmProtectRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "VM_PROTECT");

	if((req.address() & (kHelPageSize - 1))
			|| (req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
		co_await sendErrorResponse<managarm::posix::VmProtectResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	uint32_t native_flags = 0;
	if(req.mode() & PROT_READ)
		native_flags |= kHelMapProtRead;
	if(req.mode() & PROT_WRITE)
		native_flags |= kHelMapProtWrite;
	if(req.mode() & PROT_EXEC)
		native_flags |= kHelMapProtExecute;

	co_await self->vmContext()->protectFile(
			reinterpret_cast<void *>(req.address()), req.size(), native_flags);

	managarm::posix::VmProtectResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::VmUnmapRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "VM_UNMAP", "address={:#08x} size={:#x}", req.address(), req.size());

	size_t size = req.size();

	if(req.address() & (kHelPageSize - 1) || size == 0) {
		co_await sendErrorResponse<managarm::posix::VmUnmapResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(size & (kHelPageSize - 1)) {
		size = (size + (kHelPageSize - 1)) & ~(kHelPageSize - 1);
	}

	co_await self->vmContext()->unmapFile(reinterpret_cast<void *>(req.address()), size);

	managarm::posix::VmUnmapResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
