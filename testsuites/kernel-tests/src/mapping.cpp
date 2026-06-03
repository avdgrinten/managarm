#include <cassert>
#include <protocols/posix/procdata.hpp>
#include <cstddef>
#include <iostream>

#include <hel.h>
#include <hel-syscalls.h>

#include "testsuite.hpp"

DEFINE_TEST(unmapPartialPfs, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), 0x3000, 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x3000,
			kHelMapProtRead | kHelMapProtWrite, &window));

	// Do the partial unmap.
	auto p = reinterpret_cast<std::byte *>(window);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + 0x1000, 0x1000));

	// Touch the page to make sure that the new mappings work.
	p[0] = static_cast<std::byte>(0);
	p[0x2000] = static_cast<std::byte>(0);

	// Clean up.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, 0x1000));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + 0x2000, 0x1000));
}))

DEFINE_TEST(unmapPartialPreserve, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), 0x3000, 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x3000,
			kHelMapProtRead | kHelMapProtWrite, &window));

	auto p = reinterpret_cast<std::byte *>(window);
	p[0] = static_cast<std::byte>(42);
	p[0x2000] = static_cast<std::byte>(21);

	// Do the partial unmap.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + 0x1000, 0x1000));

	// Check that the values are preserved after partially unmapping.
	assert(p[0] == static_cast<std::byte>(42));
	assert(p[0x2000] == static_cast<std::byte>(21));

	// Clean up.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, 0x1000));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + 0x2000, 0x1000));
}))
