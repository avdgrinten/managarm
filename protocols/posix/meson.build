inc = [ 'include' ]
headers = [ 'include/protocols/posix/data.hpp', 'include/protocols/posix/procdata.hpp', 'include/protocols/posix/supercalls.hpp' ]

posix_bragi_files = files('posix.bragi')

posix_extra_dep = declare_dependency(
	include_directories : inc
)

if not provide_deps
	install_headers(headers, subdir : 'protocols/posix')
endif
