inc = [ 'include' ]
deps = [ libarch, hw_proto_dep, kernlet_proto_dep, libasync_dep , posix_extra_dep ]

virtio_core = shared_library('virtio_core', 'src/core.cpp',
	dependencies : deps,
	include_directories : inc,
	install : true
)

virtio_core_dep = declare_dependency(
	link_with : virtio_core,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/core/virtio/core.hpp',
	subdir : 'core/virtio'
)
