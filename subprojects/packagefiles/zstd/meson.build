project('zstd', version : '1.5.7', license : 'BSD-3-Clause OR GPL-2.0-only')

decompress_sources = files(
	'lib/common/entropy_common.c',
	'lib/common/error_private.c',
	'lib/common/fse_decompress.c',
	'lib/common/xxhash.c',
	'lib/common/zstd_common.c',
	'lib/decompress/huf_decompress.c',
	'lib/decompress/zstd_ddict.c',
	'lib/decompress/zstd_decompress.c',
	'lib/decompress/zstd_decompress_block.c',
)

includes = include_directories('lib')
