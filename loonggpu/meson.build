# Copyright © 2026 LoongGPU contributors
# SPDX-License-Identifier: MIT

datadir_loonggpu = join_paths(get_option('prefix'), get_option('datadir'), 'libdrm')

libdrm_loonggpu = library(
  'drm_loonggpu',
  [
    files(
      'loonggpu_bo.c',
      'loonggpu_cs.c',
      'loonggpu_device.c',
      'loonggpu_gpu_info.c',
      'loonggpu_userq.c',
      'loonggpu_vamgr.c',
    ),
    config_file,
  ],
  c_args : [
    libdrm_c_args,
    '-DLOONGGPU_ASIC_ID_TABLE="@0@"'.format(join_paths(datadir_loonggpu, 'loonggpu.ids')),
  ],
  include_directories : [inc_root, inc_drm],
  link_with : libdrm,
  dependencies : [dep_threads, dep_atomic_ops, dep_rt],
  version : '1.@0@.0'.format(patch_ver),
  install : true,
)

install_headers('loonggpu.h', subdir : 'libdrm')

pkg.generate(
  libdrm_loonggpu,
  name : 'libdrm_loonggpu',
  subdirs : ['.', 'libdrm'],
  description : 'Userspace interface to kernel DRM services for LoongGPU',
)

ext_libdrm_loonggpu = declare_dependency(
  link_with : [libdrm, libdrm_loonggpu],
  include_directories : [inc_drm, include_directories('.')],
)

meson.override_dependency('libdrm_loonggpu', ext_libdrm_loonggpu)
