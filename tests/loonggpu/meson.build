# Copyright © 2026 LoongGPU contributors
# SPDX-License-Identifier: MIT

loonggpu_test_incs = [inc_root, inc_drm, include_directories('../../loonggpu')]

# Keep assertions enabled: some checks perform operations inside assert().
# Shared builds interpose DRM symbols from the executable.  Static builds use
# linker wrappers to avoid duplicate definitions from libdrm.a.
loonggpu_mock_tests = [
  ['contracts', 'contracts.c', []],
  ['hw-ip-availability', 'hw-ip-availability.c', ['drmCommandWriteRead']],
  ['userq-commands', 'userq-commands.c', ['drmCommandWriteRead']],
  ['bo-wait-idle', 'bo-wait-idle.c', ['drmCommandWriteRead', 'clock_gettime']],
  ['bo-import-race', 'bo-import-race.c', ['drmCommandWriteRead', 'drmPrimeFDToHandle', 'drmIoctl']],
  ['device-lifetime', 'device-lifetime.c', ['drmGetVersion', 'drmFreeVersion', 'drmCommandWriteRead', 'drmIoctl']],
  ['bo-queries', 'bo-queries.c', ['drmCommandWriteRead', 'drmIoctl']],
  ['cs-admission', 'cs-admission.c', ['drmCommandWriteRead']],
]

foreach spec : loonggpu_mock_tests
  test_c_args = ['-UNDEBUG']
  test_link_args = []
  if get_option('default_library') == 'static'
    foreach symbol : spec[2]
      test_c_args += '-D@0@=__wrap_@0@'.format(symbol)
      test_link_args += '-Wl,--wrap=@0@'.format(symbol)
    endforeach
  endif
  check = executable(
    'loonggpu-' + spec[0],
    files(spec[1]),
    include_directories : loonggpu_test_incs,
    c_args : test_c_args,
    link_args : test_link_args,
    link_with : [libdrm, libdrm_loonggpu],
    dependencies : [dep_threads, dep_atomic_ops],
    export_dynamic : true,
    install : with_install_tests,
  )
  test('loonggpu-' + spec[0], check, suite : ['loonggpu'])
endforeach

# Compile the VA manager directly to test its hidden entry points.
loonggpu_vamgr_guard = executable(
  'loonggpu-vamgr-guard',
  [files('vamgr-guard.c'), files('../../loonggpu/loonggpu_vamgr.c')],
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  dependencies : [dep_threads, dep_atomic_ops],
  install : with_install_tests,
)
test('loonggpu-vamgr-guard', loonggpu_vamgr_guard, suite : ['loonggpu'])

# Checks that need a DRM node.  They skip with exit status 77 unless
# LOONGGPU_TEST_DEVICE names the node to use, and some of them need an
# additional opt-in variable because they submit work or create a firmware
# queue; each file documents its own condition.
loonggpu_smoketest = executable(
  'loonggpu-smoketest',
  files('smoketest.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-smoketest', loonggpu_smoketest, suite : ['loonggpu-hw'])

loonggpu_bo_sharing = executable(
  'loonggpu-bo-sharing',
  files('bo-sharing.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-bo-sharing', loonggpu_bo_sharing, suite : ['loonggpu-hw'])

loonggpu_bo_import_map = executable(
  'loonggpu-bo-import-map',
  files('bo-import-map.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-bo-import-map', loonggpu_bo_import_map, suite : ['loonggpu-hw'])

loonggpu_va_bounds = executable(
  'loonggpu-va-bounds',
  files('va-bounds-check.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-va-bounds', loonggpu_va_bounds, suite : ['loonggpu-hw'])

loonggpu_display_permissions = executable(
  'loonggpu-display-permissions',
  files('display-permissions.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-display-permissions', loonggpu_display_permissions, suite : ['loonggpu-hw'])

loonggpu_userq_lease = executable(
  'loonggpu-userq-lease',
  files('userq-lease-check.c'),
  include_directories : loonggpu_test_incs,
  c_args : ['-UNDEBUG'],
  link_with : [libdrm, libdrm_loonggpu],
  install : with_install_tests,
)
test('loonggpu-userq-lease', loonggpu_userq_lease, suite : ['loonggpu-hw'])
