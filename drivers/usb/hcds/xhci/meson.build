executable('xhci', 'src/main.cpp', 'src/ring.cpp',
	dependencies : [ libarch, core_dep, hw_proto_dep, mbus_proto_dep, usb_proto_dep, kernlet_proto_dep ],
	install : true
)

custom_target('xhci-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'xhci.bin',
	input : 'xhci.yml',
	install : true,
	install_dir : server
)
