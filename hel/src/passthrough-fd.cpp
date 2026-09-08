#include <hel.h>
#include <hel-syscalls.h>
#include <helix/passthrough-fd.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

namespace helix {

HelHandle handleForFd(int fd) {
	if (fd >= 512)
		return 0;

	posix::ManagarmProcessData data;
	HEL_CHECK(helSyscall2(
		kHelCallSuper + posix::superGetProcessData,
		reinterpret_cast<HelWord>(&data),
		sizeof(posix::ManagarmProcessData)
	));

	return reinterpret_cast<HelHandle *>(data.fileTable)[fd];
}

} // namespace helix
