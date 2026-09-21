#include <algorithm>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/hash_map.hpp>
#include <frg/string.hpp>
#include <elf.h>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/servers.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/user-io.hpp>

namespace thor {

static bool debugLaunch = true;

frg::manual_box<smarter::shared_ptr<Stream, LanePolicy>> mbusClient;
static frg::manual_box<smarter::shared_ptr<Stream, LanePolicy>> futureMbusServer;

frg::ticket_spinlock globalMfsMutex;

extern MfsDirectory *mfsRoot;

constinit IrqSpinlock allServersMutex;

// Protected by allServersMutex.
static frg::manual_box<
	frg::hash_map<
		frg::string<KernelAlloc>,
		smarter::shared_ptr<Stream, LanePolicy>,
		frg::hash<frg::string<KernelAlloc>>,
		KernelAlloc
	>
> allServers;

// TODO: move this declaration to a header file
void runService(
	managarm::svrctl::Description<KernelAlloc> desc,
	smarter::shared_ptr<Stream, LanePolicy> control_lane,
	smarter::shared_ptr<Hierarchy> hierarchy,
	smarter::shared_ptr<Thread, ActiveHandle> thread
);

// ------------------------------------------------------------------------
// File management.
// ------------------------------------------------------------------------

coroutine<bool> createMfsFile(frg::string_view path, const void *buffer, size_t size,
		MfsRegular **out) {
	// Copy to the memory object before taking locks below.
	auto memoryOutcome = AllocatedMemory::create(rootHierarchy(),
			(size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
	if(!memoryOutcome)
		panicLogger() << "thor: Failed to create memory" << frg::endlog;
	auto memory = std::move(*memoryOutcome);
	auto copyOutcome = co_await memory->copyTo(0, buffer, size);
	assert(copyOutcome);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&globalMfsMutex);

	const char *begin = path.data();
	const char *end = path.data() + path.size();
	auto it = begin;

	// We have no VFS. Relative paths are absolute.
	if(it != end && *it == '/')
		++it;

	// Parse each individual component.
	MfsNode *node = mfsRoot;
	while(it != end) {
		auto slash = std::find(it, end, '/');
		if(slash == end)
			break;

		auto component = path.sub_string(it - begin, slash - it);
		if(component == "..") {
			// We resolve double-dots unless they are at the beginning of the path.
			assert(!"Fix double-dots");
		}else if(component.size() && component != ".") {
			// We discard multiple slashes and single-dots.
			assert(node->type == MfsType::directory);
			auto directory = static_cast<MfsDirectory *>(node);
			auto target = directory->getTarget(component);
			if(target) {
				node = target;
			}else{
				node = frg::construct<MfsDirectory>(*kernelAlloc);
				directory->link(frg::string<KernelAlloc>{*kernelAlloc, component}, node);
			}
		}

		// Finally we need to skip the slash we found.
		it = slash;
		if(it != end)
			++it;
	}

	// Now, insert the file into its parent directory.
	auto directory = static_cast<MfsDirectory *>(node);
	auto name = path.sub_string(it - begin, end - it);
	if(auto file = directory->getTarget(name); file) {
		assert(file->type == MfsType::regular);
		*out = static_cast<MfsRegular *>(file);
		co_return false;
	}

	auto file = frg::construct<MfsRegular>(*kernelAlloc, std::move(memory), size);
	directory->link(frg::string<KernelAlloc>{*kernelAlloc, name}, file);
	*out = file;
	co_return true;
}

MfsNode *resolveModule(frg::string_view path) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&globalMfsMutex);

	const char *begin = path.data();
	const char *end = path.data() + path.size();
	auto it = begin;

	// We have no VFS. Relative paths are absolute.
	if(it != end && *it == '/')
		++it;

	// Parse each individual component.
	MfsNode *node = mfsRoot;
	while(it != end) {
		auto slash = std::find(it, end, '/');

		auto component = path.sub_string(it - begin, slash - it);
		if(component == "..") {
			// We resolve double-dots unless they are at the beginning of the path.
			assert(!"Fix double-dots");
		}else if(component.size() && component != ".") {
			// We discard multiple slashes and single-dots.
			assert(node->type == MfsType::directory);
			auto directory = static_cast<MfsDirectory *>(node);
			auto target = directory->getTarget(component);
			if(!target)
				return nullptr;
			node = target;
		}

		// Finally we need to skip the slash we found.
		it = slash;
		if(it != end)
			++it;
	}

	return node;
}

// ------------------------------------------------------------------------
// ELF parsing and execution.
// ------------------------------------------------------------------------

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr), interpreter(*kernelAlloc) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	frg::string<KernelAlloc> interpreter;
};

coroutine<ImageInfo> loadModuleImage(
	smarter::shared_ptr<Hierarchy> hierarchy,
	smarter::shared_ptr<AddressSpace, BindableHandle> space,
	VirtualAddr base,
	smarter::shared_ptr<MemoryView> image
) {
	ImageInfo info;

	// parse the ELf file format
	Elf64_Ehdr ehdr;
	auto copyEhdrOutcome = co_await image->copyFrom(0, &ehdr, sizeof(Elf64_Ehdr));
	assert(copyEhdrOutcome);
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');

	info.entryIp = reinterpret_cast<void *>(base + ehdr.e_entry);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		auto copyPhdrOutcome = co_await image->copyFrom(ehdr.e_phoff + i * ehdr.e_phentsize,
				&phdr, sizeof(Elf64_Phdr));
		assert(copyPhdrOutcome);
		
		if(phdr.p_type == PT_LOAD) {
			assert(phdr.p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr.p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr.p_vaddr + phdr.p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memoryOutcome = AllocatedMemory::create(hierarchy, virt_length);
			if(!memoryOutcome)
				panicLogger() << "thor: Failed to create memory" << frg::endlog;
			auto memory = std::move(*memoryOutcome);
			auto copyResult = co_await copyBetweenViews(memory.get(), phdr.p_vaddr - virt_address,
					image.get(), phdr.p_offset, phdr.p_filesz);
			assert(copyResult);

			auto viewOutcome = MemorySlice::create(std::move(memory), 0, virt_length);
			if(!viewOutcome)
				panicLogger() << "thor: Failed to create memory slice" << frg::endlog;
			auto view = std::move(*viewOutcome);

			if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				auto mapResult = co_await space->map(std::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtWrite);
				assert(mapResult);
			}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				auto mapResult = co_await space->map(std::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtExecute);
				assert(mapResult);
			}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
				auto mapResult = co_await space->map(std::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead);
				assert(mapResult);
			}else{
				panicLogger() << "Illegal combination of segment permissions"
						<< frg::endlog;
			}
		}else if(phdr.p_type == PT_INTERP) {
			info.interpreter.resize(phdr.p_filesz);
			auto copyInterpOutcome = co_await image->copyFrom(phdr.p_offset,
					info.interpreter.data(), phdr.p_filesz);
			assert(copyInterpOutcome);
		}else if(phdr.p_type == PT_PHDR) {
			info.phdrPtr = reinterpret_cast<void *>(base + phdr.p_vaddr);
		}else if(phdr.p_type == PT_DYNAMIC
				|| phdr.p_type == PT_TLS
				|| phdr.p_type == PT_GNU_EH_FRAME
				|| phdr.p_type == PT_GNU_STACK
				|| phdr.p_type == PT_GNU_RELRO
				|| phdr.p_type == PT_NOTE) {
			// ignore the phdr
		}else{
			warningLogger() << "Unexpected PHDR of type 0x" << frg::hex_fmt{phdr.p_type} << frg::endlog;
		}
	}

	co_return info;
}

template<typename T>
uintptr_t copyToStack(frg::string<KernelAlloc> &stack_image, const T &data) {
	uintptr_t misalign = stack_image.size() % alignof(T);
	if(misalign)
		stack_image.resize(alignof(T) - misalign);
	uintptr_t offset = stack_image.size();
	stack_image.resize(stack_image.size() + sizeof(data));
	memcpy(&stack_image[offset], &data, sizeof(data));
	return offset;
}

coroutine<void> executeModule(managarm::svrctl::Description<KernelAlloc> &desc, MfsRegular *module,
		smarter::shared_ptr<Stream, LanePolicy> control_lane,
		smarter::shared_ptr<Stream, LanePolicy> xpipe_lane,
		Scheduler *scheduler) {
	auto tag = frg::string<KernelAlloc>{*kernelAlloc, "server:"};
	tag += desc.name();

	auto hierarchyOutcome = Hierarchy::extend(rootHierarchy(), std::move(tag));
	if(!hierarchyOutcome)
		panicLogger() << "thor: Failed to extend hierarchy for server "
				<< desc.name() << frg::endlog;

	auto spaceOutcome = AddressSpace::create();
	if(!spaceOutcome)
		panicLogger() << "thor: Failed to create address space" << frg::endlog;
	auto space = std::move(*spaceOutcome);

	ImageInfo exec_info = co_await loadModuleImage(*hierarchyOutcome, space, 0,
			module->getMemory());

	if(size_t n = frg::string_view(exec_info.interpreter).find_first('\0'); n != size_t(-1))
		exec_info.interpreter.resize(n);
	auto rtdl_module = resolveModule(exec_info.interpreter);
	assert(rtdl_module && rtdl_module->type == MfsType::regular);
	ImageInfo interp_info = co_await loadModuleImage(*hierarchyOutcome, space, 0x40000000,
			static_cast<MfsRegular *>(rtdl_module)->getMemory());

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stackMemoryOutcome = AllocatedMemory::create(*hierarchyOutcome, stack_size);
	if(!stackMemoryOutcome)
		panicLogger() << "thor: Failed to create memory" << frg::endlog;
	auto stack_memory = std::move(*stackMemoryOutcome);
	auto stackViewOutcome = MemorySlice::create(stack_memory, 0, stack_size);
	if(!stackViewOutcome)
		panicLogger() << "thor: Failed to create memory slice" << frg::endlog;
	auto stack_view = std::move(*stackViewOutcome);

	auto mapResult = co_await space->map(std::move(stack_view), 0, 0, stack_size,
			AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
				| AddressSpace::kMapProtWrite);
	assert(mapResult);

	// build the stack data area (containing program arguments,
	// environment strings and related data).
	// TODO: do we actually need this buffer?
	frg::string<KernelAlloc> data_area(*kernelAlloc);

	uintptr_t data_disp = stack_size - data_area.size();
	auto copyDataOutcome = co_await stack_memory->copyTo(data_disp,
			data_area.data(), data_area.size());
	assert(copyDataOutcome);

	// build the stack tail area (containing the aux vector).
	auto universeOutcome = Universe::create();
	if(!universeOutcome)
		panicLogger() << "thor: Failed to create universe" << frg::endlog;
	auto universe = std::move(*universeOutcome);

	Handle xpipe_handle = 0;
	if(xpipe_lane) {
		xpipe_handle = universe->attachDescriptor(
			AnyDescriptor::make<DescriptorType::lane>(xpipe_lane, kHelRightInvoke)
		);
	}

	enum {
		AT_NULL = 0,
		AT_PHDR = 3,
		AT_PHENT = 4,
		AT_PHNUM = 5,
		AT_ENTRY = 9,

		AT_XPIPE = 0x1000,
	};

	frg::string<KernelAlloc> tail_area(*kernelAlloc);

	// Setup the stack with argc, argv and environment.
	copyToStack<uintptr_t>(tail_area, 0); // argc.
	copyToStack<uintptr_t>(tail_area, 0); // End of args.
	copyToStack<uintptr_t>(tail_area, 0); // End of environment.

	// This is the auxiliary vector.
	copyToStack<uintptr_t>(tail_area, AT_ENTRY);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.entryIp);
	copyToStack<uintptr_t>(tail_area, AT_PHDR);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.phdrPtr);
	copyToStack<uintptr_t>(tail_area, AT_PHENT);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrEntrySize);
	copyToStack<uintptr_t>(tail_area, AT_PHNUM);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrCount);
	if(xpipe_lane) {
		copyToStack<uintptr_t>(tail_area, AT_XPIPE);
		copyToStack<uintptr_t>(tail_area, xpipe_handle);
	}
	copyToStack<uintptr_t>(tail_area, AT_NULL);
	copyToStack<uintptr_t>(tail_area, 0);

	// Padding to ensure the stack alignment.
	copyToStack<uintptr_t>(tail_area, 0);
	
	uintptr_t tail_disp = data_disp - tail_area.size();
	assert(!(tail_disp % 16));
	auto copyPtrsOutcome = co_await stack_memory->copyTo(tail_disp,
			tail_area.data(), tail_area.size());
	assert(copyPtrsOutcome);

	// create a thread for the module
	AbiParameters params;
	params.ip = (uintptr_t)interp_info.entryIp;
	params.sp = mapResult.value() + tail_disp;
	params.argument = 0;

	auto threadOutcome = Thread::create(std::move(universe), std::move(space), params);
	if(!threadOutcome)
		panicLogger() << "thor: Failed to create thread" << frg::endlog;
	auto thread = std::move(*threadOutcome);
	thread->flags |= Thread::kFlagServer;

	// see helCreateThread for the reasoning here
	thread.policy().increment();
	thread.policy().increment();

	LoadBalancer::singleton().connect(thread.get(), getCpuData());
	Scheduler::associate(thread.get(), scheduler);
	Scheduler::resume(thread.get());
	Thread::resumeOther(smarter::rc_policy_downcast<smarter::default_rc_policy>(thread));

	// Listen to POSIX calls from the thread.
	// Call this after resumeOther() to ensure that we do not see the initial interrupt.
	runService(desc, control_lane, std::move(*hierarchyOutcome), thread);
}

void initializeMbusStream() {
	auto mbusStreamOutcome = createStream();
	if(!mbusStreamOutcome)
		panicLogger() << "thor: Failed to create stream" << frg::endlog;
	mbusClient.initialize(std::move(mbusStreamOutcome->get<1>()));
	futureMbusServer.initialize(std::move(mbusStreamOutcome->get<0>()));
}

coroutine<void> runMbus() {
	if(debugLaunch)
		infoLogger() << "thor: Launching mbus" << frg::endlog;

	auto desc = co_await parseServerFromInitrd("usr/lib/managarm/server/mbus.bin");

	frg::tuple<smarter::shared_ptr<Stream, LanePolicy>, smarter::shared_ptr<Stream, LanePolicy>> controlStream;
	{
		auto lock = frg::guard(&allServersMutex);

		assert(!allServers->get(desc.name()));

		auto controlStreamOutcome = createStream();
		if(!controlStreamOutcome)
			panicLogger() << "thor: Failed to create stream" << frg::endlog;
		controlStream = std::move(*controlStreamOutcome);
		allServers->insert(desc.name(), controlStream.get<1>());
	}

	auto module = resolveModule(desc.exec());
	assert(module && module->type == MfsType::regular);
	co_await executeModule(desc, static_cast<MfsRegular *>(module),
			controlStream.get<0>(),
			std::move(*futureMbusServer), &localScheduler.get());
}

coroutine<smarter::shared_ptr<Stream, LanePolicy>> runServer(
	managarm::svrctl::Description<KernelAlloc> &desc
) {
	if(debugLaunch)
		infoLogger() << "thor: Launching server " << desc.name() << frg::endlog;

	frg::tuple<smarter::shared_ptr<Stream, LanePolicy>, smarter::shared_ptr<Stream, LanePolicy>> controlStream;
	{
		auto lock = frg::guard(&allServersMutex);

		if(auto server = allServers->get(desc.name()); server) {
			if(debugLaunch)
				infoLogger() << "thor: Server "
						<< desc.name() << " is already running" << frg::endlog;
			co_return *server;
		}

		auto controlStreamOutcome = createStream();
		if(!controlStreamOutcome)
			panicLogger() << "thor: Failed to create stream" << frg::endlog;
		controlStream = std::move(*controlStreamOutcome);
		allServers->insert(desc.name(), controlStream.get<1>());
	}

	auto module = resolveModule(desc.exec());
	if(!module)
		panicLogger() << "thor: Could not find module " << desc.exec() << frg::endlog;
	assert(module->type == MfsType::regular);

	co_await executeModule(desc, static_cast<MfsRegular *>(module),
			controlStream.get<0>(),
			smarter::shared_ptr<Stream, LanePolicy>{}, &localScheduler.get());

	co_return controlStream.get<1>();
}

coroutine<managarm::svrctl::Description<KernelAlloc>> parseServerFromInitrd(frg::string_view path) {
	auto module = resolveModule(path);
	if(!module)
		panicLogger() << "thor: Could not find server description " << path << frg::endlog;
	assert(module->type == MfsType::regular);
	auto file = static_cast<MfsRegular *>(module);

	frg::unique_memory<KernelAlloc> buffer{*kernelAlloc, file->size()};
	auto copyOutcome = co_await file->getMemory()->copyFrom(0, buffer.data(), file->size());
	assert(copyOutcome);

	managarm::svrctl::Description<KernelAlloc> desc(*kernelAlloc);
	bragi::limited_reader rd{buffer.data(), buffer.size()};
	bragi::deserializer de;
	if(!desc.decode_body(rd, de))
		panicLogger() << "thor: Failed to parse server description " << path << frg::endlog;

	co_return desc;
}

coroutine<smarter::shared_ptr<Stream, LanePolicy>> runServerFromInitrd(frg::string_view path) {
	auto desc = co_await parseServerFromInitrd(path);
	co_return co_await runServer(desc);
}

// ------------------------------------------------------------------------
// svrctl interface to user space.
// mbus object creation and management.
// ------------------------------------------------------------------------


struct SvrctlBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "svrctl"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("svrctl", std::move(properties))).unwrap();
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
		if(preamble.id() == bragi::message_id<managarm::svrctl::FileUploadRequest>) {
			auto req = bragi::parse_head_only<managarm::svrctl::FileUploadRequest>(reqBuffer, *kernelAlloc);
			if(!req)
				co_return Error::protocolViolation;

			managarm::svrctl::FileUploadResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::svrctl::Errors::SUCCESS);

			if(req->with_data()) {
				auto [dataError, dataBuffer] = co_await recvBuffer(lane);
				if(dataError != Error::success)
					co_return dataError;

				MfsRegular *file;
				if(!(co_await createMfsFile(req->name(), dataBuffer.data(), dataBuffer.size(), &file))) {
					// TODO: Verify that the file data matches. This is somewhat expensive because
					//       we would have to map the file's memory. Hence, we do not implement
					//       it for now.
					if(file->size() != dataBuffer.size())
						resp.set_error(managarm::svrctl::Errors::DATA_MISMATCH);
				}
			}else{
				auto file = resolveModule(req->name());
				if(!file)
					resp.set_error(managarm::svrctl::Errors::DATA_REQUIRED);
			}

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
			bragi::write_head_only(resp, respBuffer);

			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
		}else if(preamble.id() == bragi::message_id<managarm::svrctl::RunServerRequest>) {
			auto req = bragi::parse_head_only<managarm::svrctl::RunServerRequest>(reqBuffer, *kernelAlloc);
			if(!req)
				co_return Error::protocolViolation;

			auto controlLane = co_await runServer(req->description());

			managarm::svrctl::RunServerResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::svrctl::Errors::SUCCESS);

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
			bragi::write_head_only(resp, respBuffer);

			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
			auto controlError = co_await pushDescriptor(lane,
				AnyDescriptor::make<DescriptorType::lane>(controlLane, kHelRightInvoke)
			);
			if(controlError != Error::success)
				co_return controlError;
		}else if(preamble.id() == bragi::message_id<managarm::svrctl::ProvideIoChannelRequest>) {
			auto req = bragi::parse_head_only<managarm::svrctl::ProvideIoChannelRequest>(reqBuffer, *kernelAlloc);
			if(!req)
				co_return Error::protocolViolation;

			auto channelOutcome = UserIoChannel::create(
					frg::string<KernelAlloc>{*kernelAlloc, req->tag()},
					frg::string<KernelAlloc>{*kernelAlloc, req->descriptive_tag()},
					req->out_ring_size(), req->in_ring_size());

			managarm::svrctl::ProvideIoChannelResponse<KernelAlloc> resp(*kernelAlloc);
			if(channelOutcome) {
				resp.set_error(managarm::svrctl::Errors::SUCCESS);
			}else if(channelOutcome.error() == Error::illegalArgs) {
				resp.set_error(managarm::svrctl::Errors::ILLEGAL_REQUEST);
			}else{
				co_return channelOutcome.error();
			}

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
			bragi::write_head_only(resp, respBuffer);

			auto respError = co_await sendBuffer(lane, std::move(respBuffer));
			if(respError != Error::success)
				co_return respError;
			if(!channelOutcome)
				co_return frg::success;

			auto channel = std::move(*channelOutcome);
			auto provideOutcome = co_await channel->provide(lane);
			if(!provideOutcome)
				co_return provideOutcome.error();

			infoLogger() << "thor: Registered user I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
			publishIoChannel(std::move(channel));
		}else{
			warningLogger() << "thor: Illegal message ID " << preamble.id()
					<< " for SvrctlBusObject" << frg::endlog;
			auto dismissError = co_await dismiss(boundLane);
			if(dismissError != Error::success)
				co_return Error::protocolViolation;
		}

		co_return frg::success;
	}
};

void initializeSvrctl() {
	allServers.initialize(frg::hash<frg::string<KernelAlloc>>{}, *kernelAlloc);

	// Create a fiber to manage requests to the svrctl mbus object.
	KernelFiber::run([=] {
		auto svrctl = frg::construct<SvrctlBusObject>(*kernelAlloc);
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), svrctl->run());
	});
}

} // namespace thor
