#pragma once

#include <hel.h>
#include <hel-syscalls.h>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

namespace posix {

// Returns a handle to the calling process's own hierarchy, as provided by the
// posix server via ManagarmProcessData. The result is cached for the lifetime
// of the process.
inline HelHandle getProcessHierarchy() {
	static HelHandle handle = [] {
		ManagarmProcessData pd;
		HEL_CHECK(helSyscall1(kHelCallSuper + superGetProcessData,
				reinterpret_cast<HelWord>(&pd)));
		return pd.hierarchyHandle;
	}();
	return handle;
}

} // namespace posix
