conf.set('UNVME_UBLK', '')

liburing_dep = dependency('liburing', version: '>=2.4')

ublk_lib = static_library(
  'unvme-ublk',
  ['unvme-ublk.c'],
  dependencies: [vfn_lib, nvme_lib, unvmed_dep, liburing_dep],
  include_directories: [libunvmed_inc],
)

ublk_dep = declare_dependency(
  link_with: ublk_lib,
  include_directories: [include_directories('.')],
  dependencies: [liburing_dep],
)
