src = [
	'src/libblockfs.cpp',
	'src/io-dispatcher.cpp',
	'src/io-scheduler.cpp',
	'src/metadata-cache.cpp',
	'src/service-budget.cpp',
	'src/gpt.cpp',
	'src/raw.cpp',
	'src/scsi.cpp',
	'src/swap.cpp',
	'src/ext2/block-map-cache.cpp',
	'src/ext2/ext2fs.cpp',
	'src/ext2/ops.cpp',
	'src/btrfs/btrfs.cpp',
	'src/btrfs/ops.cpp',
]
inc = [ 'include' ]
deps = [ libarch, core_dep, fs_proto_dep, mbus_proto_dep, ostrace_proto_dep, swap_proto_dep ]

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
