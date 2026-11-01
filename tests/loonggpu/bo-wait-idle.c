/* SPDX-License-Identifier: MIT */
/*
 * The public wait takes a relative duration while
 * DRM_IOCTL_LOONGGPU_GEM_WAIT_IDLE takes an absolute CLOCK_MONOTONIC deadline.
 * The DRM call and the clock are interposed here so the conversion, the
 * infinite deadline, the saturation case and the error path are all checkable
 * without a device.
 */

#include "loonggpu_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <xf86drm.h>

static const uint64_t now_ns = 12345000000678ull;
static uint64_t expected_timeout;
static unsigned ioctl_calls;
static unsigned clock_calls;
static int ioctl_error;
static uint32_t ioctl_status;

int clock_gettime(clockid_t clock, struct timespec *ts)
{
	assert(clock == CLOCK_MONOTONIC);
	clock_calls++;
	ts->tv_sec = now_ns / 1000000000ull;
	ts->tv_nsec = now_ns % 1000000000ull;
	return 0;
}

int drmCommandWriteRead(int fd, unsigned long cmd, void *data,
			unsigned long size)
{
	union drm_loonggpu_gem_wait_idle *args = data;

	assert(fd == 123);
	assert(cmd == DRM_LOONGGPU_GEM_WAIT_IDLE);
	assert(size == sizeof(*args));
	assert(args->in.handle == 17);
	assert(args->in.timeout == expected_timeout);
	ioctl_calls++;
	if (ioctl_error)
		return ioctl_error;
	args->out.status = ioctl_status;
	return 0;
}

int main(void)
{
	struct loonggpu_device dev = { .fd = 123 };
	struct loonggpu_bo bo = { .dev = &dev, .handle = 17 };
	bool busy = false;

	/* A five second wait must still wait after five seconds of uptime. */
	expected_timeout = now_ns + 5000000000ull;
	ioctl_status = 1;
	assert(loonggpu_bo_wait_for_idle(&bo, 5000000000ull, &busy) == 0);
	assert(busy && clock_calls == 1 && ioctl_calls == 1);

	/* A zero duration is an already expiring absolute deadline. */
	expected_timeout = now_ns;
	ioctl_status = 0;
	assert(loonggpu_bo_wait_for_idle(&bo, 0, &busy) == 0);
	assert(!busy && clock_calls == 2);

	/* An infinite wait is passed through without reading the clock. */
	expected_timeout = LOONGGPU_TIMEOUT_INFINITE;
	assert(loonggpu_bo_wait_for_idle(&bo, LOONGGPU_TIMEOUT_INFINITE, NULL) == 0);
	assert(clock_calls == 2);

	/* A deadline that would overflow saturates instead of wrapping into the
	 * past. */
	assert(loonggpu_bo_wait_for_idle(&bo, UINT64_MAX - 1, &busy) == 0);
	assert(clock_calls == 3);

	expected_timeout = now_ns + 1;
	ioctl_error = -EINTR;
	busy = true;
	assert(loonggpu_bo_wait_for_idle(&bo, 1, &busy) == -EINTR);
	assert(busy);
	assert(loonggpu_bo_wait_for_idle(NULL, 1, &busy) == -EINVAL);
	assert(ioctl_calls == 5 && clock_calls == 4);

	puts("BO wait deadline, poll, infinite, saturation and error checks passed");
	return 0;
}
