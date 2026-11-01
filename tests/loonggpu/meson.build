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
