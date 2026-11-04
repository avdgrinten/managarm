#include "memfd.hpp"
#include "process.hpp"
#include "swap.hpp"

#include <frg/mutex.hpp>
#include <helix/ipc.hpp>

void MemoryFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>>
MemoryFile::seek(off_t delta, VfsSeek whence) {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	if(whence == VfsSeek::absolute) {
		_offset = delta;
	}else if(whence == VfsSeek::relative){
		_offset += delta;
	}else if(whence == VfsSeek::eof) {
		assert(whence == VfsSeek::eof);
		_offset = _fileSize + delta;
	}
	co_return _offset;
}

async::result<frg::expected<protocols::fs::Error>> MemoryFile::truncate(size_t size) {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	co_return (co_await _resizeFile(size)).map_error(protocols::fs::toFsProtoError);
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::allocate(int64_t offset, size_t size) {
	assert(!offset);

	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	if(_seals & F_SEAL_WRITE)
		co_return protocols::fs::Error::insufficientPermissions;
	/* check if the file size is enough */
	if(offset + size <= _fileSize)
		co_return {};

	co_return (co_await _resizeFile(offset + size)).map_error(protocols::fs::toFsProtoError);
}

FutureMaybe<helix::UniqueDescriptor>
MemoryFile::accessMemory() {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	co_return _memory.dup();
}

// Must be called with _mutex held.
async::result<frg::expected<Error>> MemoryFile::_resizeFile(size_t new_size) {
	if(new_size > _fileSize && _seals & F_SEAL_GROW)
		co_return Error::insufficientPermissions;

	if(new_size < _fileSize && _seals & F_SEAL_SHRINK)
		co_return Error::insufficientPermissions;

	// TODO: Growing the file again exposes the truncated contents. Zero them on shrink once memory
	//       objects can discard a range; writing zeros would allocate all of it.
	_fileSize = new_size;

	size_t aligned_size = (new_size + 0xFFF) & ~size_t(0xFFF);
	if(aligned_size <= _areaSize)
		co_return {};

	if(_memory) {
		auto resizeResult = co_await helix_ng::resizeMemory(_memory, aligned_size);
		HEL_CHECK(resizeResult.error());
	}else{
		HelHandle handle;
		HEL_CHECK(helAllocateSwappableMemory(sharedHierarchy(),
				getSwapSpaceHandle().getHandle(), aligned_size, 0, &handle));
		_memory = helix::UniqueDescriptor{handle};
	}

	_areaSize = aligned_size;

	co_return {};
}

async::result<frg::expected<protocols::fs::Error, int>>
MemoryFile::getSeals() {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	co_return int{_seals};
}

async::result<frg::expected<protocols::fs::Error, int>>
MemoryFile::addSeals(int seals) {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	if(_seals & F_SEAL_SEAL) {
		co_return protocols::fs::Error::insufficientPermissions;
	}

	_seals |= seals;
	co_return int{_seals};
}

async::result<frg::expected<Error, size_t>>
MemoryFile::writeAll(Process *, const void *data, size_t length) {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	if(_seals & F_SEAL_WRITE)
		co_return Error::insufficientPermissions;

	auto end_size = _offset + length;
	if(end_size > _fileSize) {
		FRG_CO_TRY(co_await _resizeFile(end_size));
	}

	if(length) {
		auto store = co_await helix_ng::writeMemory(_memory, _offset, length, data);
		if(store.error() == kHelErrNoMemory)
			co_return Error::noMemory;
		HEL_CHECK(store.error());
	}
	_offset += length;
	co_return length;
}

async::result<std::expected<size_t, Error>>
MemoryFile::readSome(Process *, void *data, size_t max_length, async::cancellation_token) {
	co_await _mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _mutex};

	if(_offset > _fileSize)
		co_return std::unexpected{Error::eof};

	auto read_len = std::min(_fileSize - _offset, max_length);
	if(read_len) {
		auto load = co_await helix_ng::readMemory(_memory, _offset, read_len, data);
		if(load.error() == kHelErrNoMemory)
			co_return std::unexpected{Error::noMemory};
		HEL_CHECK(load.error());
	}
	_offset += read_len;
	co_return read_len;
}
