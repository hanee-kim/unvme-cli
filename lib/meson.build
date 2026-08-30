libunvmed_inc = include_directories('.')

# ublk server support requires Linux UAPI kernel headers.
# Install them with:
#   Ubuntu/Debian : apt install linux-libc-dev
#   RHEL/Fedora   : dnf install kernel-headers
c = meson.get_compiler('c')
foreach hdr : ['linux/ublk_cmd.h', 'linux/io_uring.h']
  if not c.has_header(hdr)
    error('Missing kernel UAPI header: ' + hdr + '\n' +
          'Install kernel headers:\n' +
          '  Ubuntu/Debian : apt install linux-libc-dev\n' +
          '  RHEL/Fedora   : dnf install kernel-headers')
  endif
endforeach

libunvmed_src = [
  'libunvmed.c',
  'libunvmed-vcq.c',
  'libunvmed-cmds.c',
  'libunvmed-logs.c',
  'libunvmed-ublk.c',
]
libunvmed_hdr = [
  'libunvmed.h',
  'libunvmed-logs.h',
  'libunvmed-ublk.h',
]

ver = '1.0.0'
libunvmed_so = shared_library(
  'unvmed',
  libunvmed_src,
  version: ver,
  soversion: '1',
  dependencies: [vfn_lib, nvme_lib, jsonc_lib],
  link_with: [ccan_lib],
  include_directories: [ccan_inc, libunvmed_inc],
  install: true,
  install_dir: get_option('libdir'),
)

conf.set('LIBUNVMED_VERSION', '"' + ver + '"')

install_headers(libunvmed_hdr)

unvmed_dep = declare_dependency(
  include_directories:  [ccan_inc, libunvmed_inc],
  link_with: libunvmed_so
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  libunvmed_so,
  version: ver,
  description: 'unvme-cli daemon process library',
)
