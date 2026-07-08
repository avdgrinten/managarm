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
	size_t fileTableSize;
	void *clockTrackerPage;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
