inc = include_directories('.')

unvme_sources = [
  'unvmed.c', 'unvme.c', 'unvmed-cmds.c', 'unvmed-print.c', 'unvmed-file.c',
  'unvme-cmds.c',
]

fio_lib = []
subdir('app')
subdir('ublk')

liburing_dep = dependency('liburing', version: '>=2.0', static: static)

configure_file(
    output: 'config.h',
    configuration: conf
)

executable(
  'unvme',
  unvme_sources + ublk_sources,
  dependencies: [vfn_lib, nvme_lib, jsonc_lib, unvmed_dep, liburing_dep],
  link_with: [ccan_lib, argtable3_lib],
  link_whole: [fio_lib],
  include_directories: [inc, ccan_inc, argtable3_inc, libunvmed_inc, ublk_inc],
  install: true,
  install_dir: get_option('bindir'),
)
