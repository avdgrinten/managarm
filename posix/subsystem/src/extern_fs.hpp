
#ifndef POSIX_SUBSYSTEM_EXTERN_FS_HPP
#define POSIX_SUBSYSTEM_EXTERN_FS_HPP

#include "vfs.hpp"

namespace extern_fs {

struct MountPoint;

struct OpenFile : public VfsOpenFile {
	OpenFile(MountPoint &connection, int extern_fd);

	// inherited from VfsOpenFile
	void fstat(frigg::CallbackPtr<void(FileStats)> callback) override;
	void write(const void *buffer, size_t length,
			frigg::CallbackPtr<void()> callback) override;
	void read(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(VfsError, size_t)> callback) override;

	void seek(int64_t rel_offset, frigg::CallbackPtr<void()> callback) override;
	
	MountPoint &connection;
	int externFd;
};

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

class MountPoint : public VfsMountPoint {
public:
	MountPoint(helx::Pipe pipe);
	
	// inherited from VfsMountPoint
	void openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;

	helx::Pipe &getPipe();

private:
	helx::Pipe p_pipe;
};

// --------------------------------------------------------
// Closures
// --------------------------------------------------------

struct StatClosure {
	StatClosure(MountPoint &connection, int extern_fd,
			frigg::CallbackPtr<void(FileStats)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	frigg::CallbackPtr<void(FileStats)> callback;
	uint8_t buffer[128];
};

struct OpenClosure {
	OpenClosure(MountPoint &connection, frigg::StringView path,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	frigg::StringView path;
	frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback;
	uint8_t buffer[128];
};

struct ReadClosure {
	ReadClosure(MountPoint &connection, int extern_fd, void *read_buffer, size_t max_size,
			frigg::CallbackPtr<void(VfsError, size_t)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvData(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	void *readBuffer;
	size_t maxSize;
	frigg::CallbackPtr<void(VfsError, size_t)> callback;
	uint8_t buffer[128];
};

struct SeekClosure {
	SeekClosure(MountPoint &connection, int extern_fd, int64_t rel_offset,
			frigg::CallbackPtr<void()> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	int64_t relOffset;
	frigg::CallbackPtr<void()> callback;
	uint8_t buffer[128];
};


} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP
