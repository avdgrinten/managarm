#include <elf.h>
#include <hel.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include "vfs.hpp"
#include "exec.hpp"
#include <fs.bragi.hpp>

constexpr size_t kPageSize = kHelPageSize;
constexpr uintptr_t ldsoBaseAddress = 0x40000000;

// This struct is parsed before knowing the type of executable (PIE vs. non-PIE)
// and also before knowing the ELF's base address.
struct ImagePreamble {
	bool isPie = false;
};

// This struct contains the image meta data with correct base address applied.
struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	std::string interpreter;
};

async::result<frg::expected<Error, ImagePreamble>>
parseElfPreamble(SharedFilePtr file) {
	ImagePreamble preamble;

	// Read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	FRG_CO_TRY(co_await file->seek(0, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr)));

	if(!(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F'))
		co_return Error::badExecutable;
	if(ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
		co_return Error::badExecutable;

	// Right now we treat every ET_DYN object as PIE and unconditionally apply
	// a non-zero base address.
	if(ehdr.e_type == ET_DYN)
		preamble.isPie = true;

	co_return preamble;
}

async::result<frg::expected<Error, ImageInfo>>
loadElfImage(SharedFilePtr file, VmContext *vmContext, uintptr_t base) {
	assert(!(base & (kPageSize - 1))); // Callers need to ensure this.
	ImageInfo info;

	// Get a handle to the file's memory.
	auto fileMemory = co_await file->accessMemory();

	// Read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	FRG_CO_TRY(co_await file->seek(0, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr, &ehdr, sizeof(Elf64_Ehdr)));

	// Verify the ELF file again, since loadElfPreamble() is not necessarily called
	// on every object that we load.
	if(!(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F'))
		co_return Error::badExecutable;
	if(ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
		co_return Error::badExecutable;

	// Linux rejects the same program header tables in load_elf_phdrs().
	if(ehdr.e_phentsize != sizeof(Elf64_Phdr) || !ehdr.e_phnum
			|| ehdr.e_phnum * sizeof(Elf64_Phdr) > 65536)
		co_return Error::badExecutable;

	info.entryIp = (char *)base + ehdr.e_entry;
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// Read the elf program headers and load them into the address space.
	std::vector<char> phdrBuffer;
	phdrBuffer.resize(ehdr.e_phnum * ehdr.e_phentsize);
	FRG_CO_TRY(co_await file->seek(ehdr.e_phoff, VfsSeek::absolute));
	FRG_CO_TRY(co_await file->readExactly(nullptr,
			phdrBuffer.data(), ehdr.e_phnum * size_t(ehdr.e_phentsize)));

	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdrBuffer.data() + i * ehdr.e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			if(!phdr->p_memsz) // Skip empty segments.
				continue;

			// Segments are mapped page-wise, so p_offset and p_vaddr must agree within a page.
			bool properlyAligned = (phdr->p_offset & (kPageSize - 1)) == (phdr->p_vaddr & (kPageSize - 1));

			size_t misalign = phdr->p_vaddr & (kPageSize - 1);
			uintptr_t mapAddress = base + phdr->p_vaddr - misalign;
			uintptr_t fileOffset = phdr->p_offset - misalign;
			size_t mapLength = (phdr->p_memsz + misalign + kPageSize - 1) & ~(kPageSize - 1);

			if(!properlyAligned) {
				std::cout << "posix: ELF file with differently misaligned p_offset and p_vaddr."
						<< std::endl;
				co_return Error::badExecutable;
			}

			if(phdr->p_filesz > phdr->p_memsz) {
				std::cout << "posix: ELF segment with p_filesz larger than p_memsz" << std::endl;
				co_return Error::badExecutable;
			}

			if(mapLength < phdr->p_memsz) {
				std::cout << "posix: ELF segment with p_memsz that overflows" << std::endl;
				co_return Error::badExecutable;
			}

			// Check if we can share the segment.
			if(!(phdr->p_flags & PF_W)) {
				// Map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, mapLength));

					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							fileOffset, mapLength, true,
							kHelMapProtRead | kHelMapProtExecute));
				// Allow read only mappings too, ICU loves those.
				}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R)) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, mapLength));

					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							fileOffset, mapLength, true,
							kHelMapProtRead));
				}else{
					std::cout << "posix: Illegal combination of segment permissions" << std::endl;
					co_return Error::badExecutable;
				}
			}else{
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) != (PF_R | PF_W)) {
					std::cout << "posix: Illegal combination of segment permissions" << std::endl;
					co_return Error::badExecutable;
				}

				// Map the file-backed pages copy-on-write and the remaining pages as anonymous memory.
				size_t fileMapLength = 0;
				if(phdr->p_filesz)
					fileMapLength = (misalign + phdr->p_filesz + kPageSize - 1) & ~(kPageSize - 1);

				if(fileMapLength) {
					HEL_CHECK(helLoadahead(fileMemory.getHandle(), fileOffset, fileMapLength));

					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress,
							fileMemory.dup(), file,
							fileOffset, fileMapLength, true,
							kHelMapProtRead | kHelMapProtWrite));

					// Bytes past p_filesz in the last file page must read as zero.
					size_t tailOffset = misalign + phdr->p_filesz;
					if(tailOffset < fileMapLength) {
						static const char zeros[kPageSize]{};
						auto store = co_await helix_ng::writeMemory(vmContext->getSpace(),
								mapAddress + tailOffset, fileMapLength - tailOffset, zeros);
						if(store.error() == kHelErrNoMemory)
							co_return Error::noMemory;
						// The segment extends past the end of the file.
						if(store.error() == kHelErrFault)
							co_return Error::badExecutable;
						HEL_CHECK(store.error());
					}
				}

				if(fileMapLength < mapLength)
					FRG_CO_TRY(co_await vmContext->mapFile(mapAddress + fileMapLength,
							{}, nullptr,
							0, mapLength - fileMapLength, true,
							kHelMapProtRead | kHelMapProtWrite));
			}
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_INTERP) {
			if(phdr->p_filesz < 2 || phdr->p_filesz > PATH_MAX)
				co_return Error::badExecutable;
			info.interpreter.resize(phdr->p_filesz);
			FRG_CO_TRY(co_await file->seek(phdr->p_offset, VfsSeek::absolute));
			FRG_CO_TRY(co_await file->readExactly(nullptr,
					info.interpreter.data(), phdr->p_filesz));
			if(size_t n = info.interpreter.find('\0'); n != size_t(-1))
				info.interpreter.resize(n);
		}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK
				|| phdr->p_type == PT_GNU_RELRO || phdr->p_type == PT_NOTE) {
			// Ignore this PHDR here.
		}else{
			// Ignore unknown PHDRs.
			std::cout << "posix: Unexpected PHDR type " << phdr->p_type << std::endl;
		}
	}

	co_return info;
}

async::result<frg::expected<Error, ExecuteResult>>
execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vmContext, helix::BorrowedDescriptor universe,
		HelHandle mbusHandle, Process *self) {
	(void) mbusHandle;

	auto execFile = FRG_CO_TRY(co_await open(root, workdir, path, self));
	assert(execFile); // If open() succeeds, it must return a non-null file.

	int nRecursions = 0;
	while(true) {
		if(nRecursions > 8) {
			std::cout << "posix: More than 8 shebang recursions" << std::endl;
			co_return Error::badExecutable;
		}

		char shebangPrefix[2];
		if(!(co_await execFile->readExactly(nullptr, shebangPrefix, 2)))
			break;
		if(shebangPrefix[0] != '#' && shebangPrefix[1] != '!')
			break;

		std::string shebangStr;
		while(true) {
			if(shebangStr.size() > 128) {
				std::cout << "posix: Shebang line of excessive length" << std::endl;
				co_return Error::badExecutable;
			}

			char buffer[128];
			auto readResult = co_await execFile->readSome(nullptr, buffer, 128, {});
			if (!readResult.has_value()) {
				std::cout << "posix: Failed to read executable" << std::endl;
				co_return Error::badExecutable;
			}
			size_t chunk = readResult.value();
			if(!chunk) {
				std::cout << "posix: EOF in shebang line" << std::endl;
				co_return Error::badExecutable;
			}
			auto nlPtr = std::find(buffer, buffer + 128, '\n');
			shebangStr.insert(shebangStr.end(), buffer, nlPtr);
			if(nlPtr != buffer + 128)
				break;
		}

		// The path is the first whitespace-separated word of the line.
		// Trim spaces from the left and the right.
		auto beginPath = std::find_if_not(shebangStr.begin(), shebangStr.end(), isspace);
		auto endPath = std::find_if(beginPath, shebangStr.end(), isspace);

		// Trim space from the argument, too.
		auto beginArg = std::find_if_not(endPath, shebangStr.end(), isspace);
		auto endArg = std::find_if_not(shebangStr.rbegin(), shebangStr.rend(), isspace).base();

		// Linux looks up the interpreter in the current working directory.
		std::string interpreterPath{beginPath, endPath};
		auto interpreterFile = FRG_CO_TRY(co_await open(root, workdir, interpreterPath, self));
		assert(interpreterFile); // If open() succeeds, it must return a non-null file.

		if(!args.empty()) // Handle exec() without arguments.
			args.erase(args.begin());
		args.insert(args.begin(), path);
		if(beginArg != endArg)
			args.insert(args.begin(), std::string{beginArg, endArg});
		args.insert(args.begin(), interpreterPath);
		path = std::move(interpreterPath);
		execFile = std::move(interpreterFile);
		nRecursions++;
	}

	auto execPreamble = FRG_CO_TRY(co_await parseElfPreamble(execFile));
	ImageInfo execInfo;
	if(execPreamble.isPie) {
		// Unconditionally apply a non-zero base address to PIE objects.
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, vmContext.get(), 0x200000));
	}else{
		execInfo = FRG_CO_TRY(co_await loadElfImage(execFile, vmContext.get(), 0));
	}

	// TODO: Should we really look up the dynamic linker in the current working dir?
	auto ldsoFile = FRG_CO_TRY(co_await open(root, workdir, execInfo.interpreter, self));
	assert(ldsoFile); // If open() succeeds, it must return a non-null file.
	auto ldsoInfo = FRG_CO_TRY(co_await loadElfImage(ldsoFile, vmContext.get(), ldsoBaseAddress));

	auto link = execFile->associatedLink();
	if(!link) {
		co_return Error::badExecutable;
	}

	auto stats = FRG_CO_TRY(co_await link->getTarget()->getStats());
	bool hasSetuid = stats.mode & S_ISUID;
	bool hasSetgid = stats.mode & S_ISGID;
	uid_t newUid = hasSetuid ? stats.uid : self->threadGroup()->euid();
	gid_t newGid = hasSetgid ? stats.gid : self->threadGroup()->egid();

	constexpr size_t stackSize = 0x200000;

	// Map the stack into the new process.
	void *stackBase = FRG_CO_TRY(co_await vmContext->mapFile(0,
			{}, nullptr,
			0, stackSize, true, kHelMapProtRead | kHelMapProtWrite));

	// the offset at which the stack image starts.
	size_t d = stackSize;

	size_t stringsSize = path.size() + 1;
	for(const auto &str : args)
		stringsSize += str.size() + 1;
	for(const auto &str : env)
		stringsSize += str.size() + 1;
	if(stringsSize > stackSize)
		co_return Error::illegalArguments;

	// Place argument and environment strings at the top of the stack.
	auto placeString = [&] (const std::string &str) -> uintptr_t {
		d -= str.size() + 1;
		return reinterpret_cast<uintptr_t>(stackBase) + d;
	};

	auto execfn = placeString(path);
	std::vector<uintptr_t> argsPtrs;
	for(const auto &str : args)
		argsPtrs.push_back(placeString(str));

	std::vector<uintptr_t> envPtrs;
	for(const auto &str : env)
		envPtrs.push_back(placeString(str));

	// Words below the strings, in ascending address order.
	std::vector<uintptr_t> words;
	words.push_back(argsPtrs.size()); // argc.
	words.insert(words.end(), argsPtrs.begin(), argsPtrs.end());
	words.push_back(0); // End of args.
	words.insert(words.end(), envPtrs.begin(), envPtrs.end());
	words.push_back(0); // End of environment.

	size_t auxBeginIndex = words.size();
	words.insert(words.end(), {
		AT_ENTRY,
		uintptr_t(execInfo.entryIp),
		AT_PHDR,
		uintptr_t(execInfo.phdrPtr),
		AT_PHENT,
		execInfo.phdrEntrySize,
		AT_PHNUM,
		execInfo.phdrCount,
		AT_EXECFN,
		execfn,
		AT_SECURE,
		hasSetuid || hasSetgid,
		AT_BASE,
		ldsoBaseAddress,
		AT_PAGESZ,
		kPageSize,
		AT_NULL,
		0
	});
	size_t auxEndIndex = words.size();

	// Pad the words so that the stack is aligned after them.
	if(words.size() & 1)
		words.push_back(0);

	// Align the stack before pushing the words.
	d -= d & size_t(15);
	if(d < words.size() * sizeof(uintptr_t))
		co_return Error::illegalArguments;
	d -= words.size() * sizeof(uintptr_t);

	// Stack has to be aligned at entry.
	assert(!(d & size_t(15)));

	void *auxBegin = reinterpret_cast<std::byte *>(stackBase) + d + auxBeginIndex * sizeof(uintptr_t);
	void *auxEnd = reinterpret_cast<std::byte *>(stackBase) + d + auxEndIndex * sizeof(uintptr_t);

	// The stack image is built locally and written into the process in one go.
	std::vector<char> image(stackSize - d);
	memcpy(image.data(), words.data(), words.size() * sizeof(uintptr_t));

	auto copyString = [&] (uintptr_t ptr, const std::string &str) {
		auto offset = ptr - reinterpret_cast<uintptr_t>(stackBase) - d;
		memcpy(image.data() + offset, str.c_str(), str.size() + 1);
	};

	copyString(execfn, path);
	for(size_t i = 0; i < args.size(); i++)
		copyString(argsPtrs[i], args[i]);
	for(size_t i = 0; i < env.size(); i++)
		copyString(envPtrs[i], env[i]);

	auto store = co_await helix_ng::writeMemory(vmContext->getSpace(),
			reinterpret_cast<uintptr_t>(stackBase) + d, image.size(), image.data());
	if(store.error() == kHelErrNoMemory)
		co_return Error::noMemory;
	HEL_CHECK(store.error());

	HelHandle thread;
	HEL_CHECK(helCreateThread(vmContext->getHierarchy().getHandle(), universe.getHandle(),
			vmContext->getSpace().getHandle(),
			(void *)ldsoInfo.entryIp, (char *)stackBase + d,
			kHelThreadStopped, &thread));

	co_return ExecuteResult{
		.thread = helix::UniqueDescriptor{thread},
		.auxBegin = auxBegin,
		.auxEnd = auxEnd,
		.effectiveUid = newUid,
		.effectiveGid = newGid,
		.savedUid = self->threadGroup()->uid(),
		.savedGid = self->threadGroup()->gid()
	};
}
