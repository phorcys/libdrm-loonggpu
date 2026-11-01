# libdrm_loonggpu

Userspace DRM helpers for the Linux LoongGPU driver.  Device and engine
availability are obtained from the kernel.

The library wraps the kernel UAPI in `include/drm/loonggpu_drm.h` and covers
device and queue queries, GEM buffer objects including export/import and CPU
mapping, a userspace virtual address manager, command submission with contexts,
sync objects and semaphores, and the user-queue (compute) doorbell interface.

## Build

```sh
meson setup build -Dloonggpu=enabled
ninja -C build
```

## Tests

`tests/loonggpu/` holds two kinds of checks:

- device independent checks (contracts, handle table, VA manager, device and
  child-object lifetime, BO queries, submission chunks, HW-IP availability and
  user-queue argument validation).
  They interpose the DRM entry points and need neither a device nor root.
- hardware checks (smoketest, BO sharing and CPU mapping, VA bounds, display
  ioctl permissions and user-queue leases).  They skip unless
  `LOONGGPU_TEST_DEVICE` names a node, and some of them need an additional
  opt-in variable for invalid VA requests or firmware queue creation.

```sh
meson test -C build --suite loonggpu                 # device independent
LOONGGPU_TEST_DEVICE=/dev/dri/renderD128 \
  meson test -C build --suite loonggpu-hw            # needs hardware
```

The VA bounds test additionally requires `LOONGGPU_TEST_VA_BOUNDS=1`.  The
user-queue lease test requires `LOONGGPU_TEST_USERQ=1` and creates a firmware
queue.  The display permissions test requires a render node.  The remaining
hardware tests allocate and map buffers or query device state; they do not
submit command buffers.

## Ownership and synchronization

Device initialization duplicates the caller's fd.  Reinitializing the same
open file description shares its handle table and VA allocator.  A separate
open has its own handles and VA space.  Comparing file descriptions requires
`F_DUPFD_QUERY` or permission to use `kcmp`; initialization returns
`-EOPNOTSUPP` if an existing session cannot be distinguished safely.

BOs, contexts, BO lists and VA ranges retain the device.  The fd returned by
`loonggpu_device_get_fd()` is borrowed and must not be closed by the caller.
A KMS import borrows an existing GEM handle on that same DRM file; its owner
must keep it open until the wrapper is released.  A failed BO-list destroy or
VA-range free leaves the handle valid for retry.

Semaphores retain their recorded context, including its kernel handle.  They
record structured submissions and wait on the calling CPU thread; they do not
insert GPU dependencies.  Access to a semaphore must be serialized by the
caller.  Other handles must remain referenced for the duration of each API
call, including concurrent calls.

BO and fence wait durations are relative nanoseconds, except when
`LOONGGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE` is supplied.  Sync-object waits take
absolute `CLOCK_MONOTONIC` deadlines, following the generic DRM API.  A fence
query describes one submission; it does not establish CPU cache visibility
or replace synchronization with other users of a shared buffer.

## Layout

| File | Role |
|------|------|
| `loonggpu.h` | public API |
| `loonggpu_internal.h` | internal objects shared by the sources |
| `handle_table.h` | GEM handle to object map |
| `loonggpu_device.c` | device registry, init, marketing names |
| `loonggpu_gpu_info.c` | DRM_IOCTL_LOONGGPU_INFO helpers |
| `loonggpu_bo.c` | GEM BO, BO list and VM operations |
| `loonggpu_vamgr.c` | userspace VA manager |
| `loonggpu_cs.c` | contexts, submission, fences, sync objects, semaphores |
| `loonggpu_userq.c` | user-queue doorbell interface |
| `../include/drm/loonggpu_drm.h` | kernel UAPI |
| `../data/loonggpu.ids` | marketing names |
