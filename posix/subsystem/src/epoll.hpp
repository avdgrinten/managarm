
#include <sys/epoll.h>

#include <async/cancellation.hpp>
#include "file.hpp"

namespace epoll {

smarter::shared_ptr<File, FileHandle> createFile();

Error addItem(File *epfile, Process *process, smarter::shared_ptr<File> file, int fd,
		int flags, uint64_t cookie);
Error modifyItem(File *epfile, File *file, int fd, int flags, uint64_t cookie);
Error deleteItem(File *epfile, File *file, int fd);

async::result<size_t> wait(File *epfile, struct epoll_event *events,
		size_t max_events, async::cancellation_token cancellation = {});

} // namespace epoll

