#include "common.hpp"
#include "../memfd.hpp"
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
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.rel_offset() & 0xFFF) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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
		throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
	}

	uintptr_t hint = req.address_hint();

	frg::expected<Error, void *> result;
	if(req.flags() & MAP_ANONYMOUS) {
		assert(!req.rel_offset());

		if(req.size() == 0) {
			std::cout << "posix: VM_MAP with size 0 is not allowed" << std::endl;
			co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		// We round up to page size
		size_t size = req.size();
		if(size & 0xFFF) {
			size = (req.size() + 0xFFF) & ~0xFFF;
		}

		if(copyOnWrite) {
			result = co_await self->vmContext()->mapFile(hint,
					{}, nullptr,
					0, size, true, nativeFlags);
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(self->vmContext()->hierarchyHandle(), size, 0, nullptr,
					&handle));

			result = co_await self->vmContext()->mapFile(hint,
					helix::UniqueDescriptor{handle}, nullptr,
					0, size, false, nativeFlags);
		}
	}else{
		auto file = self->fileContext()->getFile(req.fd());
		assert(file && "Illegal FD for VM_MAP");
		auto memory = co_await file->accessMemory();
		if(!memory) {
			co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
		result = co_await self->vmContext()->mapFile(hint,
				std::move(memory), std::move(file),
				req.rel_offset(), req.size(), copyOnWrite, nativeFlags);
	}

	if(!result) {
		assert(result.error() == Error::alreadyExists || result.error() == Error::noMemory);
		if(result.error() == Error::alreadyExists)
			co_await sendErrorResponse(conversation, managarm::posix::Errors::ALREADY_EXISTS);
		else if(result.error() == Error::noMemory)
			co_await sendErrorResponse(conversation, managarm::posix::Errors::NO_MEMORY);
		co_return {};
	}

	void *address = result.unwrap();

	managarm::posix::SvrResponse resp;
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

	managarm::posix::SvrResponse resp;

	logRequest(logRequests, self, "MEMFD_CREATE", "'{}'", req.name());

	if(req.flags() & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	auto link = SpecialLink::makeSpecialLink(VfsType::regular, 0777);
	auto memFile = smarter::make_shared<MemoryFile>(nullptr, link, (req.flags() & MFD_ALLOW_SEALING) == true);
	MemoryFile::serve(memFile);
	memFile->setupWeakFile(memFile);
	auto file = File::constructHandle(std::move(memFile));

	int flags = 0;

	if(req.flags() & MFD_CLOEXEC) {
		flags |= managarm::posix::OpenFlags::OF_CLOEXEC;
	}

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

	managarm::posix::SvrResponse resp;
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

	if(req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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

	managarm::posix::SvrResponse resp;
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

	if(req.address() & 0xFFF || size == 0) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(size & 0xFFF) {
		size = (size + 0xFFF) & ~0xFFF;
	}

	self->vmContext()->unmapFile(reinterpret_cast<void *>(req.address()), size);

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
