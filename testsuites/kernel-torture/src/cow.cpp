#include <hel.h>
#include <protocols/posix/procdata.hpp>
#include <hel-syscalls.h>
#include <async/basic.hpp>
#include <helix/ipc.hpp>

#include "testsuite.hpp"

DEFINE_TEST(cows, ([] {
	HelHandle handle;
	HEL_CHECK(helCopyOnWrite(posix::getProcessHierarchy(), kHelZeroMemory, 0, 0x1000, &handle));

	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	*reinterpret_cast<volatile uintptr_t *>(window) = 0xDEADBEEF;
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));

	auto forkResult = async::run(helix_ng::forkMemory(helix::BorrowedDescriptor{handle}),
			helix::currentDispatcher);
	HEL_CHECK(forkResult.error());
	auto forkHandle = forkResult.descriptor().getHandle();
	HEL_CHECK(helMapMemory(forkHandle, kHelNullHandle, nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	*reinterpret_cast<volatile uintptr_t *>(window) = 0xC0FFEE;
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));

	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, handle));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, forkHandle));
}))
