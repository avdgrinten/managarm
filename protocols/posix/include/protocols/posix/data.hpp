#pragma once

#include <hel.h>

namespace posix {

struct ThreadPage {
	unsigned int globalSignalFlag;
	bool cancellationRequested;
	HelHandle queueHandle;
};

// API signatures stored in PtDescriptor::apiSignature.
enum PtApis {
	ptApiDefault = 1,
};

// One entry of the shared file descriptor table. The sequence number acts as a
// seqlock so that readers can observe a consistent descriptor while it is updated.
struct PtDescriptor {
	uint64_t sequence;
	HelHandle handle;
	uint64_t apiSignature;
	uint64_t reserved;
};

struct ManagarmProcessData {
	HelHandle posixLane;
	HelHandle mbusLane;
	ThreadPage *threadPage;
	PtDescriptor *fileTable;
	size_t fileTableSize;
	void *clockTrackerPage;
};

struct ManagarmServerData {
	HelHandle controlLane;
};

} // namespace posix
