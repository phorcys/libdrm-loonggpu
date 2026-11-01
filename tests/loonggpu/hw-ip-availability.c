/* SPDX-License-Identifier: MIT */
/*
 * loonggpu_query_hw_ip_available() answers "is there a queue for this engine"
 * and must not turn an absent engine into an error.  The two INFO queries it
 * uses are interposed so every branch, including the transport failures, is
 * reachable without a device.
 */

#include "loonggpu_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int count_result;
static int info_result;
static uint32_t reported_count;
static uint32_t reported_rings;
static unsigned count_calls;
static unsigned info_calls;
static unsigned failures;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

int
drmCommandWriteRead(int fd, unsigned long command, void *data,
		    unsigned long size)
{
	struct drm_loonggpu_info *query = data;

	CHECK(fd == 17, "query uses the device fd");
	CHECK(command == DRM_LOONGGPU_INFO, "query uses DRM_LOONGGPU_INFO");
	CHECK(size == sizeof(*query), "query has the expected ABI size");

	if (query->query == LOONGGPU_INFO_HW_IP_COUNT) {
		count_calls++;
		if (count_result)
			return count_result;
		*(uint32_t *)(uintptr_t)query->return_pointer = reported_count;
		return 0;
	}

	if (query->query == LOONGGPU_INFO_HW_IP_INFO) {
		struct drm_loonggpu_info_hw_ip *info;

		info_calls++;
		if (info_result)
			return info_result;
		info = (void *)(uintptr_t)query->return_pointer;
		memset(info, 0, sizeof(*info));
		info->available_rings = reported_rings;
		return 0;
	}

	CHECK(false, "unexpected INFO query selector");
	return -EINVAL;
}

static void
reset_mock(void)
{
	count_result = 0;
	info_result = 0;
	reported_count = 0;
	reported_rings = 0;
	count_calls = 0;
	info_calls = 0;
}

static void
check_query(struct loonggpu_device *device, unsigned type, int expected_result,
	    bool expected_available, unsigned expected_count_calls,
	    unsigned expected_info_calls, const char *label)
{
	bool available = true;
	int result = loonggpu_query_hw_ip_available(device, type, &available);

	if (result != expected_result || available != expected_available ||
	    count_calls != expected_count_calls || info_calls != expected_info_calls) {
		fprintf(stderr,
			"FAIL: %s: result=%d available=%u count_calls=%u "
			"info_calls=%u\n",
			label, result, available, count_calls, info_calls);
		failures++;
	}
}

int
main(void)
{
	struct loonggpu_device device = {.fd = 17};

	/* The kernel answers -EINVAL for an engine slot it does not implement,
	 * which is "no queue", not a failure. */
	reset_mock();
	count_result = -EINVAL;
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, 0, false, 1, 0,
		    "an unimplemented COMPUTE engine is reported as unavailable");

	reset_mock();
	count_result = -EINVAL;
	check_query(&device, LOONGGPU_HW_IP_GFX, 0, false, 1, 0,
		    "an unimplemented engine of any type is reported as unavailable");

	reset_mock();
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, 0, false, 1, 0,
		    "a zero instance count is unavailable without an info query");

	reset_mock();
	reported_count = 1;
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, 0, false, 1, 1,
		    "a zero available-rings mask is unavailable");

	reset_mock();
	reported_count = 1;
	reported_rings = 2;
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, 0, true, 1, 1,
		    "a nonzero available-rings mask is available");

	reset_mock();
	count_result = -EIO;
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, -EIO, false, 1, 0,
		    "a count transport error is reported");

	reset_mock();
	reported_count = 1;
	info_result = -EFAULT;
	check_query(&device, LOONGGPU_HW_IP_COMPUTE, -EFAULT, false, 1, 1,
		    "an info transport error is reported");

	if (failures) {
		fprintf(stderr, "%u HW-IP availability checks failed\n", failures);
		return 1;
	}

	puts("LoongGPU HW-IP availability checks passed");
	return 0;
}
