if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('uhci', 'src/main.cpp',
	dependencies : [ libarch, hw_proto_dep, kernlet_proto_dep, mbus_proto_dep, usb_proto_dep , posix_extra_dep ],
	install : true
)
