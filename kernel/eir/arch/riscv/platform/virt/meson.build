exe = executable('eir-virt', eir_sources, 'early-log.cpp',
	include_directories : eir_includes,
	cpp_args : eir_cpp_args,
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	install : true
)
