test_inc = []
test_inc += libunvmed_inc

test_src = [
  'test_cid_conflict.c',
]

executable(
  'test_cid_conflict',
  test_src,
  dependencies: [vfn_lib, nvme_lib, unvmed_dep],
  include_directories: test_inc,
  install: false,
)
