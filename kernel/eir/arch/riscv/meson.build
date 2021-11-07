# TODO: we need to add eir_generic_sources here.
eir_sources = [
	eir_generic_main,
	files(
		'cpu.cpp',
		'entry64.S',
		'paging.cpp'
	),
	eir_generic_sources
]
eir_includes += include_directories('.')

subdir('platform/allwinner-d1')
subdir('platform/virt')
