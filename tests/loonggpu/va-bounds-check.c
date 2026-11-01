/* SPDX-License-Identifier: MIT */
/*
 * GEM_VA range validation: a rejected request must be a complete no-op.
 *
 * The cases are a wrapping offset_in_bo, a VA end outside the device VA space,
 * a VA that the kernel's hole masking would fold into the reserved bottom, an
 * unaligned size or offset, and flags wider than the 32-bit UAPI field.  After
 * the rejections a control MAP at the original VA must still succeed, which catches
 * an orphan mapping left behind by a failed request.
 *
 * Opt-in: creating these requests on a kernel without the matching checks walks
 * the page tables with a bogus range, so the test skips unless
 * LOONGGPU_TEST_DEVICE and LOONGGPU_TEST_VA_BOUNDS=1 are set.
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

#define BO_SIZE 16384
#define VA_HOLE_END 0xffff800000000000ULL
#define VA_NEAR_TOP 0x0000fffffffff000ULL

static unsigned failures;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

static const uint64_t rwx = LOONGGPU_VM_PAGE_READABLE |
			    LOONGGPU_VM_PAGE_WRITEABLE |
			    LOONGGPU_VM_PAGE_EXECUTABLE;

int
main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	const char *bounds_env = getenv("LOONGGPU_TEST_VA_BOUNDS");
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
	int mapped = 0;
	int ret = 1;
	int r;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping VA bounds check");
		return 77;
	}
	if (!bounds_env || strcmp(bounds_env, "1") != 0) {
		puts("LOONGGPU_TEST_VA_BOUNDS != 1; skipping VA bounds check");
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

	/* Control: a well formed MAP succeeds and can be removed again. */
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, rwx, LOONGGPU_VA_OP_MAP);
	CHECK(r == 0, "control MAP succeeds");
	if (r)
		goto cleanup;
	mapped = 1;
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, 0, LOONGGPU_VA_OP_UNMAP);
	CHECK(r == 0, "control UNMAP succeeds");
	if (r)
		goto cleanup;
	mapped = 0;

	/* offset_in_bo + size wraps. */
	r = loonggpu_bo_va_op(bo, UINT64_MAX - 0xfff, 0x2000, va, rwx,
			      LOONGGPU_VA_OP_MAP);
	CHECK(r != 0, "a MAP with a wrapping offset_in_bo is refused");

	/* The exclusive VA end lies outside the device VA space. */
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, VA_NEAR_TOP, rwx,
			      LOONGGPU_VA_OP_MAP);
	CHECK(r != 0, "a MAP whose end exceeds the device VA space is refused");

	/* Hole masking folds HOLE_END into the reserved bottom. */
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, VA_HOLE_END, rwx,
			      LOONGGPU_VA_OP_MAP);
	CHECK(r != 0, "a MAP at HOLE_END is refused");

	/* Alignment. */
	r = loonggpu_bo_va_op(bo, 0, 0x1234, va, rwx, LOONGGPU_VA_OP_MAP);
	CHECK(r != 0, "a MAP with an unaligned size is refused");
	r = loonggpu_bo_va_op(bo, 1, BO_SIZE, va, rwx, LOONGGPU_VA_OP_MAP);
	CHECK(r != 0, "a MAP with an unaligned offset_in_bo is refused");

	/* Flags wider than the 32-bit UAPI field must not be truncated into a
	 * different, valid request. */
	r = loonggpu_bo_va_op_raw(device, bo, 0, BO_SIZE, va,
				  rwx | (1ULL << 32), LOONGGPU_VA_OP_MAP);
	CHECK(r == -EINVAL, "a MAP with flags beyond UINT32_MAX is refused");

	/* A context id and a BO handle are only meaningful on their own device. */
	{
		int fd2 = open(path, O_RDWR | O_CLOEXEC);

		CHECK(fd2 >= 0, "a second device fd can be opened");
		if (fd2 >= 0) {
			loonggpu_device_handle dev2 = NULL;

			int init_ret = loonggpu_device_initialize(fd2, NULL, NULL, &dev2);

			CHECK(init_ret == 0 && dev2, "a second device can be initialized");
			if (!init_ret && dev2) {
				r = loonggpu_bo_va_op_raw(dev2, bo, 0, BO_SIZE, va,
							  rwx, LOONGGPU_VA_OP_MAP);
				CHECK(r == -EINVAL,
				      "a MAP of another device's BO is refused");
				loonggpu_device_deinitialize(dev2);
			}
			close(fd2);
		}
	}

	/* None of the refusals may have left a mapping behind. */
	r = loonggpu_bo_va_op(bo, 0, BO_SIZE, va, rwx, LOONGGPU_VA_OP_MAP);
	CHECK(r == 0, "the control MAP still succeeds after the refusals");
	if (r == 0)
		mapped = 1;
	ret = failures ? 1 : 0;

cleanup:
	if (mapped && bo)
		loonggpu_bo_va_op(bo, 0, BO_SIZE, va, 0, LOONGGPU_VA_OP_UNMAP);
	if (va_handle)
		loonggpu_va_range_free(va_handle);
	if (bo)
		loonggpu_bo_free(bo);
	if (device)
		loonggpu_device_deinitialize(device);
	if (fd >= 0)
		close(fd);

	if (failures) {
		fprintf(stderr, "%u VA bounds checks failed\n", failures);
		return 1;
	}
	if (!ret)
		puts("VA bounds checks passed");
	return ret;
}
