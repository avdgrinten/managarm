inc = include_directories('include')
deps = [ libarch_base, hel_dep, frigg ]

if build_drivers
	libarch_libary = shared_library(
		'arch',
		'src/dma_pool.cpp',
		dependencies: deps + [ posix_extra_dep ],
		include_directories: inc,
		install: true
	)

	libarch = declare_dependency(
		link_with : libarch_libary,
		dependencies : deps + [ posix_extra_dep ],
		include_directories : inc
	)
endif
