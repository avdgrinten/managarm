#include <hel.h>
#include <hel-syscalls.h>
#include <helix/passthrough-fd.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

namespace helix {

HelHandle handleForFd(int fd) {
	posix::ManagarmProcessData data;
	HEL_CHECK(helSyscall1(kHelCallSuper + posix::superGetProcessData, reinterpret_cast<HelWord>(&data)));

	if (static_cast<size_t>(fd) >= data.fileTableSize)
		return 0;

	return reinterpret_cast<HelHandle *>(data.fileTable)[fd];
}

} // namespace helix
