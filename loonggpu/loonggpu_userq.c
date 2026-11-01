/* SPDX-License-Identifier: MIT */
/*
 * User-queue (compute) wrappers.
 *
 * These helpers manage LG200/LG210 CPIPE queues in userspace memory.  The client
 * writes a command stream into a ring; DRM_IOCTL_LOONGGPU_USERQ registers the
 * queue with the firmware and publishes write pointers through a doorbell.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <xf86drm.h>

#include "loonggpu_internal.h"

/* The ring sits after a 64-byte index block, so its address is 64-byte
 * aligned and so is every write pointer the doorbell publishes. */
#define LOONGGPU_USERQ_ALIGN 64

static int
loonggpu_userq_ioctl(loonggpu_device_handle dev, union drm_loonggpu_userq *args)
{
	if (!dev)
		return -EINVAL;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_USERQ, args,
				   sizeof(*args));
}

int
loonggpu_userq_create(loonggpu_device_handle dev, uint32_t ip_type,
		      uint64_t queue_va, uint64_t queue_size, uint32_t *queue_id)
{
	union drm_loonggpu_userq args;
	int ret;

	if (!dev || !queue_id)
		return -EINVAL;
	/* Validate the engine and queue geometry before issuing CREATE. */
	if (ip_type != LOONGGPU_HW_IP_COMPUTE ||
	    queue_size > UINT64_MAX - queue_va ||
	    queue_size < LOONGGPU_USERQ_MIN_SIZE ||
	    queue_size > LOONGGPU_USERQ_MAX_SIZE ||
	    (queue_size & (queue_size - 1)) ||
	    (queue_size & (LOONGGPU_USERQ_ALIGN - 1)) ||
	    (queue_va & (LOONGGPU_USERQ_ALIGN - 1)) ||
	    queue_va < LOONGGPU_USERQ_INDEX_SIZE)
		return -EINVAL;

	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_USERQ_OP_CREATE;
	args.in.ip_type = ip_type;
	args.in.queue_va = queue_va;
	args.in.queue_size = queue_size;
	ret = loonggpu_userq_ioctl(dev, &args);
	if (ret)
		return ret;

	*queue_id = args.out.queue_id;
	return 0;
}

int
loonggpu_userq_free(loonggpu_device_handle dev, uint32_t queue_id)
{
	union drm_loonggpu_userq args;

	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_USERQ_OP_FREE;
	args.in.queue_id = queue_id;
	/* The kernel validates ip_type before it looks at op, so every request
	 * has to name the engine even though queue_id identifies the queue. */
	args.in.ip_type = LOONGGPU_HW_IP_COMPUTE;
	return loonggpu_userq_ioctl(dev, &args);
}

int
loonggpu_userq_doorbell(loonggpu_device_handle dev, uint32_t queue_id,
			uint64_t wptr)
{
	union drm_loonggpu_userq args;

	if (wptr & (LOONGGPU_USERQ_ALIGN - 1))
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_USERQ_OP_DOORBELL;
	args.in.queue_id = queue_id;
	args.in.wptr = wptr;
	/* Required for the same reason as in loonggpu_userq_free(). */
	args.in.ip_type = LOONGGPU_HW_IP_COMPUTE;
	return loonggpu_userq_ioctl(dev, &args);
}
