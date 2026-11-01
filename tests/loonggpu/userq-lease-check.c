/* SPDX-License-Identifier: MIT */
/*
 * A live user queue owns its ring mapping in the device VM.
 *
 * While a compute queue exists the driver must not let anything disturb the
 * memory the firmware is reading or the VMID the queue runs under:
 *
 *   GEM_VA UNMAP of the ring mapping   -EBUSY
 *   GEM_VA CLEAR over the ring range   -EBUSY
 *   VM UNRESERVE_VMID                  -EBUSY
 *
 * After FREE the mapping is removable again.  Creating the queue needs a real
 * firmware queue, so the test is opt-in: it runs only with LOONGGPU_TEST_DEVICE
 * and LOONGGPU_TEST_USERQ=1.
 */

#include "loonggpu.h"
#include "loonggpu_drm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BO_SIZE 65536
#define RING_OFFSET 4096
#define RING_SIZE 4096

static unsigned failures;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

int
main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	const char *userq_env = getenv("LOONGGPU_TEST_USERQ");
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = BO_SIZE,
		.phys_alignment = BO_SIZE,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
		.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
	};
	loonggpu_device_handle device = NULL;
	loonggpu_bo_handle bo = NULL;
	loonggpu_va_handle va_handle = NULL;
	uint64_t va = 0;
	int fd = -1;
	uint32_t queue_id = 0;
	bool queue_live = false;
	void *cpu = NULL;
	int mapped = 0;
	int ret = 1;
	int r;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping user-queue lease check");
		return 77;
	}
	if (!userq_env || strcmp(userq_env, "1") != 0) {
		puts("LOONGGPU_TEST_USERQ != 1; skipping user-queue lease check "
		     "(it creates a firmware queue)");
		return 77;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		goto cleanup;
	}

	uint32_t drm_major = 0, drm_minor = 0;
	if (loonggpu_device_initialize(fd, &drm_major, &drm_minor, &device) ||
	    !device) {
		fprintf(stderr, "device init failed\n");
		goto cleanup;
	}
	if (loonggpu_bo_alloc(device, &request, &bo) || !bo) {
		fprintf(stderr, "BO alloc failed\n");
		goto cleanup;
	}
	if (loonggpu_va_range_alloc(device, loonggpu_gpu_va_range_general,
				    BO_SIZE, BO_SIZE, 0, &va, &va_handle, 0) ||
	    !va) {
		fprintf(stderr, "VA alloc failed\n");
		goto cleanup;
	}
	if (loonggpu_bo_va_op(bo, 0, BO_SIZE, va,
			      LOONGGPU_VM_PAGE_READABLE |
			      LOONGGPU_VM_PAGE_WRITEABLE |
			      LOONGGPU_VM_PAGE_EXECUTABLE,
			      LOONGGPU_VA_OP_MAP)) {
		fprintf(stderr, "VA map failed\n");
		goto cleanup;
	}
	mapped = 1;

	if (loonggpu_bo_cpu_map(bo, &cpu)) {
		fprintf(stderr, "CPU map failed\n");
		goto cleanup;
	}
	memset(cpu, 0, BO_SIZE);
	__sync_synchronize();

	/* The ring starts after a separate 64-byte index block. */
	r = loonggpu_userq_create(device, LOONGGPU_HW_IP_COMPUTE,
				  va + RING_OFFSET, RING_SIZE,
				  &queue_id);
	if (r) {
		fprintf(stderr, "user-queue CREATE returned %d\n", r);
		if (r == -ENODEV || r == -ENOTTY || r == -EOPNOTSUPP)
			ret = 77;
		goto cleanup;
	}
	queue_live = true;
	printf("user-queue created: id=%u ring_va=0x%llx size=%d\n",
	       queue_id, (unsigned long long)(va + RING_OFFSET), RING_SIZE);

	/* The live queue pins the ring mapping and the VMID it runs under. */
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, 0, LOONGGPU_VA_OP_UNMAP);
	CHECK(r == -EBUSY, "UNMAP of a live ring mapping is refused");
	if (r != -EBUSY)
		goto cleanup;
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, 0, LOONGGPU_VA_OP_CLEAR);
	CHECK(r == -EBUSY, "CLEAR over a live ring range is refused");
	if (r != -EBUSY)
		goto cleanup;
	r = loonggpu_vm_unreserve_vmid(device, 0);
	CHECK(r == -EBUSY, "UNRESERVE_VMID with a live queue is refused");

	ret = failures ? 1 : 0;

cleanup:
	if (queue_live) {
		r = loonggpu_userq_free(device, queue_id);
		CHECK(r == 0, "user-queue FREE succeeds");
		if (r)
			/* Leave resources to file teardown if firmware ownership could
			 * not be released.  Do not retry FREE or recycle the mapping. */
			return 1;
	}
	if (mapped && bo) {
		r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, 0, LOONGGPU_VA_OP_UNMAP);
		CHECK(r == 0, "UNMAP succeeds after the queue lease is released");
		if (r)
			return 1;
	}
	if (cpu)
		loonggpu_bo_cpu_unmap(bo);
	if (va_handle)
		loonggpu_va_range_free(va_handle);
	if (bo)
		loonggpu_bo_free(bo);
	if (device)
		loonggpu_device_deinitialize(device);
	if (fd >= 0)
		close(fd);

	if (failures) {
		fprintf(stderr, "%u user-queue lease checks failed\n", failures);
		return 1;
	}
	if (!ret)
		puts("user-queue lease checks passed");
	return ret;
}
