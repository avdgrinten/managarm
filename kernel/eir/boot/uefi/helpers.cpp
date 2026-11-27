#include "helpers.hpp"
#include "efi.hpp"

void EFI_CHECK(efi_status s, std::source_location loc) {
	if (s == EFI_SUCCESS)
		return;

	eir::panicLogger() << "eir: unexpected EFI error 0x" << frg::hex_fmt{s} << " in "
	                   << loc.function_name() << " at " << loc.file_name() << ":" << loc.line()
	                   << frg::endlog;
}

namespace eir {

efi_status fsOpen(efi_file_protocol **file, char16_t *path) {
	efi_guid loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	efi_guid simpleFsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

	efi_loaded_image_protocol *loadedImage = nullptr;
	EFI_CHECK(
	    bs->handle_protocol(handle, &loadedImageGuid, reinterpret_cast<void **>(&loadedImage))
	);

	efi_simple_file_system_protocol *fileSystem = nullptr;
	EFI_CHECK(bs->handle_protocol(
	    loadedImage->device_handle, &simpleFsGuid, reinterpret_cast<void **>(&fileSystem)
	));

	efi_file_protocol *root = nullptr;
	EFI_CHECK(fileSystem->open_volume(fileSystem, &root));

	// Callers may fall back to other files, so a missing file is not fatal here.
	return root->open(root, file, path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
}

efi_status fsRead(efi_file_protocol *file, size_t len, size_t offset, efi_physical_addr buf) {
	EFI_CHECK(file->set_position(file, offset));
	EFI_CHECK(file->read(file, &len, reinterpret_cast<void *>(buf)));
	EFI_CHECK(file->set_position(file, 0));

	return EFI_SUCCESS;
}

size_t fsGetSize(efi_file_protocol *file) {
	efi_file_info fileInfo;
	efi_guid guid = EFI_FILE_INFO_GUID;
	uintptr_t infoLen = 0x1;

	efi_status stat = file->get_info(file, &guid, &infoLen, &fileInfo);
	assert(stat == EFI_SUCCESS || stat == EFI_BUFFER_TOO_SMALL);
	efi_file_info *fileInfoPtr = nullptr;
	EFI_CHECK(bs->allocate_pool(EfiLoaderData, infoLen, reinterpret_cast<void **>(&fileInfoPtr)));
	EFI_CHECK(file->get_info(file, &guid, &infoLen, fileInfoPtr));

	return fileInfoPtr->file_size;
}

char16_t *asciiToUcs2(frg::string_view &s) {
	assert(bs);

	char16_t *ucs2 = nullptr;
	EFI_CHECK(
	    bs->allocate_pool(EfiLoaderData, (s.size() + 1) * 2, reinterpret_cast<void **>(&ucs2))
	);

	for (size_t i = 0; i < s.size(); i++) {
		ucs2[i] = static_cast<char16_t>(s[i]);
	}

	ucs2[s.size()] = '\0';

	return ucs2;
}

frg::string<UefiAllocator> ucs2ToAscii(const char16_t *s, size_t n) {
	frg::string<UefiAllocator> out{n};
	for (size_t i = 0; i < n; ++i) {
		auto c = s[i];
		// We only use printable ASCII characters, everything else gets replaced.
		if (c >= 0x20 && c <= 0x7E) {
			out[i] = static_cast<char>(c);
		} else {
			out[i] = '?';
		}
	}
	return out;
}

} // namespace eir
