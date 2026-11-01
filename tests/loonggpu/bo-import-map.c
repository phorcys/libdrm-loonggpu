/* SPDX-License-Identifier: MIT */
/*
 * Create a GEM handle outside the library, then borrow it without a cached
 * size.  Mapping and querying must discover the kernel size.  A second mapping
 * through the raw handle checks that both mappings refer to the same storage.
 * No command buffers are submitted.
 */

#include "loonggpu.h"
#include "loonggpu_drm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#define BO_SIZE 65536

int main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	union drm_loonggpu_gem_create create = {0};
	union drm_loonggpu_gem_mmap mmap_args = {0};
	struct drm_gem_close close_args = {0};
	struct loonggpu_bo_import_result imported = {0};
	struct loonggpu_bo_info info;
	loonggpu_device_handle device = NULL;
	uint8_t *map = NULL, *raw_map = MAP_FAILED;
	uint32_t handle = 0;
	int fd = -1, ret = 1;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping BO import/map check");
		return 77;
	}
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (loonggpu_device_initialize(fd, NULL, NULL, &device)) {
		fprintf(stderr, "device initialization failed\n");
		goto cleanup;
	}
	create.in.bo_size = BO_SIZE;
	create.in.domains = LOONGGPU_GEM_DOMAIN_GTT;
	create.in.domain_flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	if (drmCommandWriteRead(fd, DRM_LOONGGPU_GEM_CREATE, &create, sizeof(create))) {
		fprintf(stderr, "GEM_CREATE failed\n");
		goto cleanup;
	}
	handle = create.out.handle;
	if (loonggpu_bo_import(device, loonggpu_bo_handle_type_kms, handle, &imported) ||
	    imported.alloc_size != 0) {
		fprintf(stderr, "a new borrowed handle must start without a cached size\n");
		goto cleanup;
	}
	/* Map before querying: cpu_map must obtain the size itself. */
	if (loonggpu_bo_cpu_map(imported.buf_handle, (void **)&map) || !map) {
		fprintf(stderr, "mapping the borrowed handle failed\n");
		goto cleanup;
	}
	if (loonggpu_bo_query_info(imported.buf_handle, &info) || info.alloc_size != BO_SIZE) {
		fprintf(stderr, "query_info did not report the kernel size\n");
		goto cleanup;
	}
	mmap_args.in.handle = handle;
	if (drmCommandWriteRead(fd, DRM_LOONGGPU_GEM_MMAP, &mmap_args, sizeof(mmap_args))) {
		fprintf(stderr, "GEM_MMAP failed\n");
		goto cleanup;
	}
	raw_map = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, mmap_args.out.addr_ptr);
	if (raw_map == MAP_FAILED) {
		perror("mmap");
		goto cleanup;
	}
	map[0] = 0x5a;
	map[BO_SIZE - 1] = 0xa5;
	raw_map[0x1234] = 0x3c;
	if (raw_map[0] != 0x5a || raw_map[BO_SIZE - 1] != 0xa5 || map[0x1234] != 0x3c) {
		fprintf(stderr, "borrowed and raw mappings do not alias\n");
		goto cleanup;
	}
	if (loonggpu_bo_cpu_unmap(imported.buf_handle)) {
		fprintf(stderr, "unmapping the borrowed handle failed\n");
		goto cleanup;
	}
	map = NULL;
	loonggpu_bo_free(imported.buf_handle);
	imported.buf_handle = NULL;
	/* Releasing the wrapper must not close the caller's GEM handle. */
	mmap_args.in.handle = handle;
	if (drmCommandWriteRead(fd, DRM_LOONGGPU_GEM_MMAP, &mmap_args, sizeof(mmap_args))) {
		fprintf(stderr, "the borrowed wrapper closed the caller's handle\n");
		goto cleanup;
	}
	ret = 0;

cleanup:
	if (raw_map != MAP_FAILED)
		munmap(raw_map, BO_SIZE);
	if (map)
		loonggpu_bo_cpu_unmap(imported.buf_handle);
	if (imported.buf_handle)
		loonggpu_bo_free(imported.buf_handle);
	if (handle) {
		close_args.handle = handle;
		drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args);
	}
	if (device)
		loonggpu_device_deinitialize(device);
	if (fd >= 0)
		close(fd);
	if (!ret)
		puts("BO import/map check passed");
	return ret;
}
