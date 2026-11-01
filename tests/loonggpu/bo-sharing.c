/* SPDX-License-Identifier: MIT */
/*
 * A BO that is exported as a dma-buf and imported back on the same device must
 * keep its contents and its identity: the import has to return the existing
 * wrapper instead of a second one for the same object, and the data written
 * through the exporter must still be readable through the importer after the
 * exporter is released.
 *
 * Skips (exit 77) unless LOONGGPU_TEST_DEVICE names the node to use.
 */

#include "loonggpu.h"
#include "loonggpu_drm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BO_SIZE 4096

static void
fill_pattern(uint8_t *data)
{
	for (unsigned i = 0; i < BO_SIZE; i++)
		data[i] = (uint8_t)(i * 37u + 11u);
}

static int
check_pattern(const uint8_t *data)
{
	for (unsigned i = 0; i < BO_SIZE; i++) {
		if (data[i] != (uint8_t)(i * 37u + 11u))
			return -1;
	}
	return 0;
}

int
main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = BO_SIZE,
		.phys_alignment = BO_SIZE,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
		.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
	};
	struct loonggpu_bo_import_result imported = {0};
	loonggpu_device_handle device = NULL;
	loonggpu_bo_handle bo = NULL;
	uint32_t shared_handle;
	void *map = NULL;
	int dma_buf_fd = -1;
	int fd = -1;
	int ret = 1;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping BO sharing check");
		return 77;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		goto cleanup;
	}

	if (loonggpu_device_initialize(fd, NULL, NULL, &device)) {
		fprintf(stderr, "failed to initialize LoongGPU device\n");
		goto cleanup;
	}
	if (loonggpu_bo_alloc(device, &request, &bo)) {
		fprintf(stderr, "failed to allocate BO\n");
		goto cleanup;
	}
	if (loonggpu_bo_cpu_map(bo, &map)) {
		fprintf(stderr, "failed to map BO\n");
		goto cleanup;
	}
	fill_pattern(map);
	if (loonggpu_bo_cpu_unmap(bo)) {
		fprintf(stderr, "failed to unmap BO\n");
		map = NULL;
		goto cleanup;
	}
	map = NULL;

	if (loonggpu_bo_export(bo, loonggpu_bo_handle_type_dma_buf_fd,
			       &shared_handle)) {
		fprintf(stderr, "failed to export BO\n");
		goto cleanup;
	}
	dma_buf_fd = (int)shared_handle;
	if (loonggpu_bo_import(device, loonggpu_bo_handle_type_dma_buf_fd,
			       (uint32_t)dma_buf_fd, &imported)) {
		fprintf(stderr, "failed to import BO\n");
		goto cleanup;
	}
	if (imported.buf_handle != bo) {
		fprintf(stderr, "same-device import did not preserve BO identity\n");
		goto cleanup;
	}
	if (imported.alloc_size < BO_SIZE) {
		fprintf(stderr, "imported BO is smaller than the allocation\n");
		goto cleanup;
	}

	close(dma_buf_fd);
	dma_buf_fd = -1;
	loonggpu_bo_free(bo);
	bo = NULL;

	if (loonggpu_bo_cpu_map(imported.buf_handle, &map)) {
		fprintf(stderr, "failed to map imported BO after exporter release\n");
		goto cleanup;
	}
	if (check_pattern(map)) {
		fprintf(stderr, "imported BO contents mismatch\n");
		goto cleanup;
	}
	if (loonggpu_bo_cpu_unmap(imported.buf_handle)) {
		fprintf(stderr, "failed to unmap imported BO\n");
		map = NULL;
		goto cleanup;
	}
	map = NULL;

	puts("LoongGPU BO sharing check passed");
	ret = 0;

cleanup:
	if (map) {
		if (bo)
			loonggpu_bo_cpu_unmap(bo);
		else if (imported.buf_handle)
			loonggpu_bo_cpu_unmap(imported.buf_handle);
	}
	if (dma_buf_fd >= 0)
		close(dma_buf_fd);
	if (bo)
		loonggpu_bo_free(bo);
	if (imported.buf_handle)
		loonggpu_bo_free(imported.buf_handle);
	if (device)
		loonggpu_device_deinitialize(device);
	if (fd >= 0)
		close(fd);
	return ret;
}
