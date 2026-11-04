#include <linux/elf.h>
#include <numeric>
#include <print>
#include <sys/procfs.h>

#include "process.hpp"

#if defined(__x86_64__)

namespace {

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> createFile(Process *proc, std::string name) {
	PathResolver resolver;
	resolver.setup(proc->fsContext()->getRoot(), proc->fsContext()->getWorkingDirectory(), name, proc);
	auto resolveResult = co_await resolver.resolve(
		resolvePrefix | resolveOpenCreate);
	if (!resolveResult)
		co_return resolveResult.error() | toPosixError;

	auto directory = resolver.currentLink();
	auto linkResult = co_await directory->getTarget()->getLinkOrCreate(directory.get(), proc, name, 0755);
	if (!linkResult)
		co_return linkResult.error();

	auto link = linkResult.value();
	auto fileResult = co_await link->getTarget()->open(proc, resolver.currentView(), std::move(link), semanticRead | semanticWrite);
	co_return fileResult;
}

size_t ntSiginfoSize() {
	return sizeof(elf_siginfo);
}

async::result<size_t> ntAuxvSize(Process *p, std::vector<std::byte> &buffer) {
	auto begin = reinterpret_cast<std::byte *>(p->clientAuxBegin());
	auto end = reinterpret_cast<std::byte *>(p->clientAuxEnd());
	for(auto it = begin; it != end; ++it) {
		// We load the memory byte for byte until we fail,
		// readMemory does not support partial reads yet.
		std::byte b;
		auto loadMemory = co_await helix_ng::readMemory(p->vmContext()->getSpace(),
			reinterpret_cast<uintptr_t>(it), 1, &b);
		if(loadMemory.error())
			break;
		buffer.push_back(b);
	}

	co_return buffer.size();
}

size_t ntFileSize(Process *p, const std::vector<VmContext::AreaSnapshot> &areas,
		std::vector<std::string> &noteFiles) {
	size_t noteSize = sizeof(long) + sizeof(long);

	for(auto &area : areas) {
		if(!area.backingFile())
			continue;
		noteSize += 3 * sizeof(long);
		ViewPath path{area.backingFile()->associatedMount(), area.backingFile()->associatedLink()};
		noteFiles.push_back(path.getPath(p->fsContext()->getRoot()));
		noteSize += noteFiles.back().size() + 1;
	}

	return noteSize;
}

size_t ntSizeAlign(size_t noteSize) {
	return (noteSize + 3) & ~3;
}

// Removes a core file that could not be written completely.
static async::result<void> removeCoreFile(FsLink *link) {
	auto [parent, name] = link->getParentAndName();
	auto res[[maybe_unused]] = co_await parent->getTarget()->unlink(std::move(name));
}

static const auto simdStateSize = [] () -> size_t {
	HelRegisterInfo regInfo;
	HEL_CHECK(helQueryRegisterInfo(kHelRegsSimd, &regInfo));
	return regInfo.setSize;
}();

bool dumpArea(const VmContext::AreaSnapshot &area) {
	return !area.backingFile() || (area.isWritable() || area.isExecutable());
}

}

#endif // defined(__x86_64__)

async::result<void> Process::coredump(TerminationState state) {
#if !defined(__x86_64__)
	(void)state;
	std::println("posix: coredump is not supported on this architecture yet");
	co_return;
#else
	if(!threadGroup()->dumpable_)
		co_return;

	std::println("posix: writing coredump for process {}", pid());
	auto fileResult = co_await createFile(this, std::format("core.{}", pid()));
	if(!fileResult)
		co_return;
	auto file = std::move(fileResult.value());

	Elf64_Half phdrCount = 1;
	size_t memoryDumpSize = 0;

	// All passes below need to agree on the set of areas.
	auto areas = co_await vmContext()->snapshotAreas();

	for(auto &area : areas) {
		phdrCount++;
		if(dumpArea(area))
			memoryDumpSize += area.size();
	}

	size_t noteSize = 0;
	std::vector<std::byte> auxv;
	std::vector<std::string> noteFiles;

	// NT_PRSTATUS
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(elf_prstatus);
	// NT_PRPSINFO
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(elf_prpsinfo);
	// NT_SIGINFO
	noteSize += sizeof(Elf64_Nhdr) + 8 + ntSiginfoSize();
	// NT_AUXV
	noteSize += sizeof(Elf64_Nhdr) + 8 + co_await ntAuxvSize(this, auxv);
	// NT_FILE
	noteSize += sizeof(Elf64_Nhdr) + 8 + ntFileSize(this, areas, noteFiles);
	noteSize = ntSizeAlign(noteSize);
	// NT_PRFPREG
	noteSize += sizeof(Elf64_Nhdr) + 8 + sizeof(user_fpregs_struct);

	size_t memoryDumpOffset = sizeof(Elf64_Ehdr) + (phdrCount * sizeof(Elf64_Phdr)) + noteSize;
	memoryDumpOffset = (memoryDumpOffset + 0x1000 - 1) & ~0xFFF;

	auto resizeResult = co_await file->truncate(memoryDumpOffset + memoryDumpSize);
	if (!resizeResult) {
		co_await removeCoreFile(file->associatedLink().get());
		co_return;
	}

	auto memory = co_await file->accessMemory();
	if (!memory) {
		co_await removeCoreFile(file->associatedLink().get());
		co_return;
	}

	// Headers and notes are built locally and written into the file's memory in one go.
	std::vector<std::byte> headers(memoryDumpOffset);

	size_t fileOffset = 0;

	auto write = [&headers, &fileOffset](const void *data, size_t len) {
		assert(fileOffset + len <= headers.size());
		memcpy(headers.data() + fileOffset, data, len);
		fileOffset += len;
	};

	Elf64_Ehdr hdr{
		.e_type = ET_CORE,
		.e_machine = EM_X86_64,
		.e_version = EV_CURRENT,
		.e_phoff = sizeof(Elf64_Ehdr),
		.e_flags = 0,
		.e_ehsize = sizeof(Elf64_Ehdr),
		.e_phentsize = sizeof(Elf64_Phdr),
		.e_phnum = phdrCount,
	};

	memcpy(hdr.e_ident, ELFMAG, SELFMAG);
	hdr.e_ident[EI_CLASS] = ELFCLASS64;
	hdr.e_ident[EI_DATA] = ELFDATA2LSB;
	hdr.e_ident[EI_VERSION] = EV_CURRENT;

	write(&hdr, sizeof(hdr));

	Elf64_Phdr notePhdr{
		.p_type = PT_NOTE,
		.p_flags = 0,
		.p_offset = sizeof(hdr) + (hdr.e_phnum * sizeof(Elf64_Phdr)),
		.p_filesz = noteSize,
		.p_align = 4,
	};

	write(&notePhdr, sizeof(notePhdr));

	size_t dumpRegionOffset = 0;
	for(auto &area : areas) {
		Elf64_Word areaFlags = 0;
		if(area.isReadable())
			areaFlags |= PF_R;
		if(area.isWritable())
			areaFlags |= PF_W;
		if(area.isExecutable())
			areaFlags |= PF_X;

		bool dump = dumpArea(area);

		Elf64_Phdr phdr{
			.p_type = PT_LOAD,
			.p_flags = areaFlags,
			.p_offset = memoryDumpOffset + dumpRegionOffset,
			.p_vaddr = area.baseAddress(),
			.p_paddr = 0,
			.p_filesz = dump ? area.size() : 0,
			.p_memsz = area.size(),
			.p_align = 0x1000,
		};

		dumpRegionOffset += phdr.p_filesz;

		write(&phdr, sizeof(phdr));
	}

	// NT_PRSTATUS
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRSTATUS,
		};
		elf_prstatus prstatus;
		memset(&prstatus, 0, sizeof(prstatus));

		noteHeader.n_descsz = sizeof(prstatus);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
			prstatus.pr_info.si_signo = bySignal->signo;
			prstatus.pr_info.si_code = SI_TKILL;
		}

		prstatus.pr_pid = pid();
		prstatus.pr_ppid = getParent() ? getParent()->pid() : 0;

		uintptr_t pcrs[2];
		uintptr_t threadrs[2];
		uintptr_t gprs[kHelNumGprs];

		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
				kHelRegsProgram, pcrs));
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
				kHelRegsGeneral, gprs));
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
			kHelRegsThread, threadrs));

		#if defined(__x86_64__)
		prstatus.pr_reg[10] = gprs[0];
		prstatus.pr_reg[5] = gprs[1];
		prstatus.pr_reg[11] = gprs[2];
		prstatus.pr_reg[12] = gprs[3];
		prstatus.pr_reg[13] = gprs[5];
		prstatus.pr_reg[14] = gprs[4];
		prstatus.pr_reg[4] = gprs[14];
		prstatus.pr_reg[19] = pcrs[1];
		prstatus.pr_reg[9] = gprs[6];
		prstatus.pr_reg[8] = gprs[7];
		prstatus.pr_reg[7] = gprs[8];
		prstatus.pr_reg[6] = gprs[9];
		prstatus.pr_reg[3] = gprs[10];
		prstatus.pr_reg[2] = gprs[11];
		prstatus.pr_reg[1] = gprs[12];
		prstatus.pr_reg[0] = gprs[13];
		prstatus.pr_reg[16] = pcrs[0];
		prstatus.pr_reg[25] = threadrs[0];
		prstatus.pr_reg[26] = threadrs[1];
		#else
		#warning "Unsupported architecture for coredump"
		#endif

		write(&prstatus, sizeof(prstatus));
	}

	// NT_PRPSINFO
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRPSINFO,
		};
		noteHeader.n_descsz = sizeof(elf_prpsinfo);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		elf_prpsinfo info;
		memset(&info, 0, sizeof(info));
		info.pr_sname = 'R';
		info.pr_pid = pid();
		info.pr_ppid = getParent() ? getParent()->pid() : 0;
		info.pr_uid = threadGroup()->uid();
		info.pr_gid = threadGroup()->gid();
		info.pr_flag = 0x600;
		strncpy(info.pr_fname, this->name().c_str(), sizeof(info.pr_fname));
		strncpy(info.pr_psargs, this->path().c_str(), sizeof(info.pr_psargs));
		write(&info, sizeof(info));
	}

	// NT_SIGINFO
	{
		elf_siginfo siginfo{};

		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_SIGINFO,
		};
		noteHeader.n_descsz = sizeof(siginfo);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
			siginfo.si_signo = bySignal->signo;
			siginfo.si_code = SI_TKILL;
		}

		write(&siginfo, sizeof(siginfo));
	}

	// NT_AUXV
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_AUXV,
		};
		noteHeader.n_descsz = auxv.size();
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));
		write(auxv.data(), auxv.size());
	}

	// NT_FILE
	{
		size_t noteFileDesc = std::accumulate(noteFiles.begin(), noteFiles.end(), 0,
			[&](size_t sum, const std::string &file) {
				return sum + file.size() + 1;
			}) + (3 * sizeof(long) * noteFiles.size());

		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_FILE,
		};
		noteHeader.n_descsz = (2 * sizeof(long)) + noteFileDesc;
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		long info[2];
		info[0] = noteFiles.size();
		info[1] = 0x1000;

		write(&info, sizeof(info));

		for(auto &area : areas) {
			if(!area.backingFile())
				continue;

			long fileInfo[3];
			fileInfo[0] = area.baseAddress();
			fileInfo[1] = area.baseAddress() + area.size();
			fileInfo[2] = area.backingFileOffset() / 0x1000;
			write(&fileInfo, sizeof(fileInfo));
		}

		for(auto path : noteFiles) {
			write(path.c_str(), path.size() + 1);
		}

		size_t disalign = (4 - (noteFileDesc % 4)) & 3;
		fileOffset += disalign;
	}

	// NT_PRFPREG
	{
		Elf64_Nhdr noteHeader{
			.n_namesz = 5,
			.n_type = NT_PRFPREG,
		};
		noteHeader.n_descsz = sizeof(user_fpregs_struct);
		write(&noteHeader, sizeof(noteHeader));

		const char name[8] = "CORE\0\0\0";
		write(reinterpret_cast<const void *>(name), sizeof(name));

		std::vector<std::byte> fpreg(simdStateSize);
		HEL_CHECK(helLoadRegisters(threadDescriptor().getHandle(),
			kHelRegsSimd, fpreg.data()));
		write(fpreg.data(), sizeof(struct user_fpregs_struct));
	}

	auto storeHeaders = co_await helix_ng::writeMemory(memory, 0, headers.size(), headers.data());
	if(storeHeaders.error()) {
		co_await removeCoreFile(file->associatedLink().get());
		co_return;
	}

	fileOffset = memoryDumpOffset;

	std::vector<std::byte> buffer(0x10000);
	for(auto &area : areas) {
		if(!dumpArea(area))
			continue;

		for(size_t progress = 0; progress < area.size(); progress += buffer.size()) {
			auto chunk = std::min(area.size() - progress, buffer.size());
			auto loadMemory = co_await helix_ng::readMemory(vmContext()->getSpace(),
				area.baseAddress() + progress, chunk, buffer.data());
			if (loadMemory.error() != kHelErrNone)
				memset(buffer.data(), 0, chunk);

			auto storeMemory = co_await helix_ng::writeMemory(memory, fileOffset + progress,
				chunk, buffer.data());
			if(storeMemory.error()) {
				co_await removeCoreFile(file->associatedLink().get());
				co_return;
			}
		}

		fileOffset += area.size();
	}

	assert(fileOffset == (memoryDumpOffset + memoryDumpSize));

	co_return;
#endif
}
