src = [
	'src/card.cpp',
	'src/device.cpp',
]
inc = [ 'include' ]
deps = [ core_dep, fs_proto_dep, mbus_proto_dep , posix_extra_dep ]

libsound_driver = shared_library('sound', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

libsound_dep = declare_dependency(
	link_with : libsound_driver,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/sound.hpp',
	subdir : 'drivers/libsound'
)
