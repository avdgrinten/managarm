executable('storage', 'src/main.cpp',
	dependencies : [ core_dep, mbus_proto_dep, usb_proto_dep, libblockfs_dep ],
	install : true
)

custom_target('usb-storage-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'usb-storage.bin',
	input : 'usb-storage.yml',
	install : true,
	install_dir : server
)
