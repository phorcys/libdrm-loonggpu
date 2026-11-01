/* SPDX-License-Identifier: MIT */
/*
 * The three user-queue wrappers, with DRM_IOCTL_LOONGGPU_USERQ interposed: the
 * request they build has to name the engine and the queue for every op, and the
 * arguments the kernel would reject are refused before a queue id exists.
 */

#include "loonggpu_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>

static unsigned failures;
static unsigned calls;
static int ioctl_result;
static union drm_loonggpu_userq seen;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

int drmCommandWriteRead(int fd, unsigned long cmd, void *data,
			unsigned long size)
{
	CHECK(fd == 23, "the request uses the device fd");
	CHECK(cmd == DRM_LOONGGPU_USERQ, "the request uses DRM_LOONGGPU_USERQ");
	CHECK(size == sizeof(seen), "the request has the expected ABI size");
	memcpy(&seen, data, sizeof(seen));
	calls++;
	if (ioctl_result)
		return ioctl_result;
	if (seen.in.op == LOONGGPU_USERQ_OP_CREATE)
		((union drm_loonggpu_userq *)data)->out.queue_id = 5;
	return 0;
}

static void check_create(void)
{
	struct loonggpu_device dev = { .fd = 23 };
	uint32_t queue_id = 0;

	calls = 0;
	ioctl_result = 0;
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE,
				    0x100000, 4096, &queue_id) == 0,
	      "a well formed CREATE succeeds");
	CHECK(calls == 1 && seen.in.op == LOONGGPU_USERQ_OP_CREATE &&
	      seen.in.ip_type == LOONGGPU_HW_IP_COMPUTE &&
	      seen.in.queue_va == 0x100000 && seen.in.queue_size == 4096,
	      "CREATE names the op, engine, ring address and ring size");
	CHECK(queue_id == 5, "CREATE returns the queue id the kernel assigned");

	calls = 0;
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 0x100000,
				    6144, &queue_id) == -EINVAL,
	      "a ring size that is not a power of two is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 0x100000,
				    LOONGGPU_USERQ_MIN_SIZE / 2,
				    &queue_id) == -EINVAL,
	      "a ring smaller than the minimum is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 0x100000,
				    LOONGGPU_USERQ_MAX_SIZE * 2,
				    &queue_id) == -EINVAL,
	      "a ring larger than the maximum is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 0x100008,
				    4096, &queue_id) == -EINVAL,
	      "a ring address that is not 64-byte aligned is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 32, 4096,
				    &queue_id) == -EINVAL,
	      "a ring address that leaves no room for the index block is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE, 0x100000,
				    4096, NULL) == -EINVAL,
	      "CREATE without an output is refused");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_GFX, 0x100000,
				    4096, &queue_id) == -EINVAL,
	      "only the compute engine supports user queues");
	CHECK(loonggpu_userq_create(&dev, LOONGGPU_HW_IP_COMPUTE,
				    UINT64_MAX - 63, 4096, &queue_id) == -EINVAL,
	      "a wrapping ring range is refused");
	CHECK(calls == 0, "no refused CREATE reached the kernel");
}

static void check_free_and_doorbell(void)
{
	struct loonggpu_device dev = { .fd = 23 };

	calls = 0;
	ioctl_result = 0;
	CHECK(loonggpu_userq_free(&dev, 7) == 0, "FREE succeeds");
	CHECK(calls == 1 && seen.in.op == LOONGGPU_USERQ_OP_FREE &&
	      seen.in.queue_id == 7 &&
	      seen.in.ip_type == LOONGGPU_HW_IP_COMPUTE,
	      "FREE names the queue and its engine");

	calls = 0;
	CHECK(loonggpu_userq_doorbell(&dev, 7, 128) == 0, "DOORBELL succeeds");
	CHECK(calls == 1 && seen.in.op == LOONGGPU_USERQ_OP_DOORBELL &&
	      seen.in.queue_id == 7 && seen.in.wptr == 128 &&
	      seen.in.ip_type == LOONGGPU_HW_IP_COMPUTE,
	      "DOORBELL names the queue, its engine and the write pointer");

	calls = 0;
	CHECK(loonggpu_userq_doorbell(&dev, 7, 129) == -EINVAL,
	      "a write pointer that is not 64-byte aligned is refused");
	CHECK(calls == 0, "the refused DOORBELL did not reach the kernel");

	/* A kernel refusal is reported unchanged. */
	calls = 0;
	ioctl_result = -EBUSY;
	CHECK(loonggpu_userq_doorbell(&dev, 7, 0) == -EBUSY,
	      "a kernel error is passed through");
	CHECK(loonggpu_userq_free(&dev, 7) == -EBUSY,
	      "a kernel error is passed through on FREE");
	ioctl_result = 0;

	CHECK(loonggpu_userq_free(NULL, 7) == -EINVAL,
	      "FREE without a device is refused");
	CHECK(loonggpu_userq_doorbell(NULL, 7, 0) == -EINVAL,
	      "DOORBELL without a device is refused");
}

int main(void)
{
	check_create();
	check_free_and_doorbell();

	if (failures) {
		fprintf(stderr, "%u user-queue command checks failed\n", failures);
		return 1;
	}
	puts("user-queue command checks passed");
	return 0;
}
