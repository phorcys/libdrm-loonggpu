/* SPDX-License-Identifier: MIT */
/* Submission argument validation and chunk construction with an ioctl mock. */

#include "loonggpu_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <xf86drm.h>

static unsigned calls;
static int submit_error;

int drmCommandWriteRead(int fd, unsigned long command, void *data,
			unsigned long size)
{
	union drm_loonggpu_cs *args = data;
	const uint64_t *pointers;
	const struct drm_loonggpu_cs_chunk *chunk;
	const struct drm_loonggpu_cs_chunk_fence *fence;
	unsigned i;

	assert(fd == 17 && command == DRM_LOONGGPU_CS && size == sizeof(*args));
	assert(args->in.ctx_id == 9 && args->in.bo_list_handle == 3);
	assert(args->in.num_chunks == 3);
	pointers = (const void *)(uintptr_t)args->in.chunks;
	for (i = 0; i < 2; i++) {
		const struct drm_loonggpu_cs_chunk_ib *ib;

		chunk = (const void *)(uintptr_t)pointers[i];
		assert(chunk->chunk_id == LOONGGPU_CHUNK_ID_IB);
		assert(chunk->length_dw == sizeof(*ib) / 4);
		ib = (const void *)(uintptr_t)chunk->chunk_data;
		assert(ib->va_start == 0x100000 + i * 4096);
		assert(ib->ib_bytes == 64 && !ib->flags && !ib->_pad);
		assert(ib->ip_type == LOONGGPU_HW_IP_GFX);
		assert(!ib->ip_instance && !ib->ring);
	}
	chunk = (const void *)(uintptr_t)pointers[2];
	assert(chunk->chunk_id == LOONGGPU_CHUNK_ID_FENCE);
	assert(chunk->length_dw == sizeof(*fence) / 4);
	fence = (const void *)(uintptr_t)chunk->chunk_data;
	assert(fence->handle == 7 && fence->offset == 24);
	calls++;
	if (submit_error)
		return submit_error;
	args->out.handle = 37;
	return 0;
}

int main(void)
{
	struct loonggpu_device dev = { .fd = 17 }, other = { .fd = 18 };
	struct loonggpu_context ctx = { .dev = &dev, .id = 9 };
	struct loonggpu_bo_list list = { .dev = &dev, .handle = 3 };
	struct loonggpu_bo bo = { .dev = &dev, .handle = 7, .alloc_size = 4096 };
	struct loonggpu_cs_ib_info ibs[2] = {
		{ .ib_mc_address = 0x100000, .size = 64 },
		{ .ib_mc_address = 0x101000, .size = 64 },
	};
	struct loonggpu_cs_request req = {
		.context = &ctx, .ip_type = LOONGGPU_HW_IP_GFX,
		.resources = &list, .ibs = ibs, .number_of_ibs = 2,
		.fence_memory = &bo, .fence_offset = 24,
	};
	struct loonggpu_cs_fence fence = { .context = &ctx, .fence = 37 };
	struct drm_loonggpu_cs_chunk chunk = {0};
	uint32_t handle = 0, expired = 0;
	uint64_t seq = 0;

	pthread_mutex_init(&ctx.sequence_mutex, NULL);
	pthread_mutex_init(&bo.cpu_access_mutex, NULL);
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == 0);
	assert(req.seq_no == 37 && ctx.last_seq[LOONGGPU_HW_IP_GFX][0][0] == 37);
	assert(calls == 1);
	submit_error = -EIO;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EIO);
	assert(req.seq_no == 37 && ctx.last_seq[LOONGGPU_HW_IP_GFX][0][0] == 37);
	assert(calls == 2);

	assert(loonggpu_cs_submit(&dev, 1, &req, 1) == -EINVAL);
	req.flags = 1;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EINVAL);
	req.flags = 0;
	assert(loonggpu_cs_submit(&dev, 0, &req, 2) == -EINVAL);
	assert(loonggpu_cs_submit(&other, 0, &req, 1) == -EINVAL);
	list.dev = &other;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EINVAL);
	list.dev = &dev;
	bo.dev = &other;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EINVAL);
	bo.dev = &dev;
	req.fence_offset = 4096;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EINVAL);
	req.fence_offset = UINT64_MAX;
	assert(loonggpu_cs_submit(&dev, 0, &req, 1) == -EINVAL);
	assert(loonggpu_cs_submit_raw2(&other, &ctx, 0, 1, &chunk, &seq) == -EINVAL);
	assert(loonggpu_cs_submit_raw(&dev, &ctx, NULL, 0, NULL, &seq) == -EINVAL);
	assert(loonggpu_cs_fence_to_handle(&other, &fence, 0, &handle) == -EINVAL);
	assert(loonggpu_cs_query_fence_status(&fence, 0, UINT64_MAX, &expired) == -EINVAL);
	assert(loonggpu_cs_query_fence_status(&fence, 0, 1ull << 32, &expired) == -EINVAL);
	assert(calls == 2);
	pthread_mutex_destroy(&bo.cpu_access_mutex);
	pthread_mutex_destroy(&ctx.sequence_mutex);
	puts("submission chunk and validation checks passed");
	return 0;
}
