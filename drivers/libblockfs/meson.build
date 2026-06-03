src = [
	'src/libblockfs.cpp',
	'src/gpt.cpp',
	'src/raw.cpp',
	'src/scsi.cpp',
	'src/ext2/ext2fs.cpp',
	'src/ext2/ops.cpp',
	'src/btrfs/btrfs.cpp',
	'src/btrfs/ops.cpp',
]
inc = [ 'include' ]
deps = [ libarch, core_dep, fs_proto_dep, mbus_proto_dep, ostrace_proto_dep , posix_extra_dep ]

libblockfs_driver = shared_library('blockfs', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

libblockfs_dep = declare_dependency(
	link_with : libblockfs_driver,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/blockfs.hpp',
	subdir : 'drivers/libblockfs'
)
