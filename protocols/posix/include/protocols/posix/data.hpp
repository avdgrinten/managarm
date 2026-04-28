#pragma once

#include <hel.h>

namespace posix {

struct ThreadPage {
	unsigned int globalSignalFlag;
	bool cancellationRequested;
	HelHandle queueHandle;
};

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	ThreadPage *threadPage;
	HelHandle *fileTable;
	void *clockTrackerPage;
	HelHandle hierarchyHandle;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
