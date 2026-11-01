/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

#include "loonggpu_internal.h"

/* Per-chunk payload storage: an IB description or a fence description. */
union loonggpu_cs_chunk_data {
	struct drm_loonggpu_cs_chunk_ib ib;
	struct drm_loonggpu_cs_chunk_fence fence;
};

/* Release the kernel context only after its last reference is dropped. */
static int loonggpu_context_put(struct loonggpu_context *ctx)
{
	struct loonggpu_device *dev;
	union drm_loonggpu_ctx args = {0};
	int r;

	if (!ctx || !atomic_dec_and_test(&ctx->refcount))
		return 0;
	dev = ctx->dev;
	args.in.op = LOONGGPU_CTX_OP_FREE_CTX;
	args.in.ctx_id = ctx->id;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_CTX, &args, sizeof(args));
	pthread_mutex_destroy(&ctx->sequence_mutex);
	free(ctx);
	loonggpu_device_unref_internal(dev);
	return r;
}

int loonggpu_cs_ctx_create2(loonggpu_device_handle dev, uint32_t priority,
			    loonggpu_context_handle *out_ctx)
{
	union drm_loonggpu_ctx args;
	struct loonggpu_context *ctx;
	int r;

	if (out_ctx)
		*out_ctx = NULL;
	if (!dev || !out_ctx)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_CTX_OP_ALLOC_CTX;
	args.in.priority = priority;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_CTX, &args, sizeof(args));
	if (r)
		return r;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		uint32_t allocated = args.out.alloc.ctx_id;

		memset(&args, 0, sizeof(args));
		args.in.op = LOONGGPU_CTX_OP_FREE_CTX;
		args.in.ctx_id = allocated;
		drmCommandWriteRead(dev->fd, DRM_LOONGGPU_CTX, &args, sizeof(args));
		return -ENOMEM;
	}
	ctx->dev = dev;
	loonggpu_device_ref_internal(dev);
	ctx->id = args.out.alloc.ctx_id;
	atomic_set(&ctx->refcount, 1);
	pthread_mutex_init(&ctx->sequence_mutex, NULL);
	*out_ctx = ctx;
	return 0;
}

int loonggpu_cs_ctx_create(loonggpu_device_handle dev, loonggpu_context_handle *context)
{
	return loonggpu_cs_ctx_create2(dev, 0, context);
}

int loonggpu_cs_ctx_free(loonggpu_context_handle ctx)
{
	if (!ctx)
		return -EINVAL;
	return loonggpu_context_put(ctx);
}

int loonggpu_cs_query_reset_state(loonggpu_context_handle ctx, uint32_t *state, uint32_t *hangs)
{
	union drm_loonggpu_ctx args;
	int r;

	if (state)
		*state = 0;
	if (hangs)
		*hangs = 0;
	if (!ctx)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_CTX_OP_QUERY_STATE;
	args.in.ctx_id = ctx->id;
	r = drmCommandWriteRead(ctx->dev->fd, DRM_LOONGGPU_CTX, &args, sizeof(args));
	if (!r) {
		if (state)
			*state = args.out.state.reset_status;
		if (hangs)
			*hangs = args.out.state.hangs;
	}
	return r;
}

int loonggpu_cs_query_reset_state2(loonggpu_context_handle ctx,
				   uint32_t *flags, uint32_t *hangs)
{
	union drm_loonggpu_ctx args = {0};
	int r;

	if (flags)
		*flags = 0;
	if (hangs)
		*hangs = 0;
	if (!ctx)
		return -EINVAL;
	args.in.op = LOONGGPU_CTX_OP_QUERY_STATE2;
	args.in.ctx_id = ctx->id;
	r = drmCommandWriteRead(ctx->dev->fd, DRM_LOONGGPU_CTX, &args, sizeof(args));
	if (!r) {
		if (flags)
			*flags = args.out.state.flags;
		if (hangs)
			*hangs = args.out.state.hangs;
	}
	return r;
}

int loonggpu_cs_query_state3(loonggpu_context_handle ctx,
			     struct drm_loonggpu_ctx_state3 *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
	if (!ctx || !state)
		return -EINVAL;
	state->ctx_id = ctx->id;
	return drmCommandWriteRead(ctx->dev->fd, DRM_LOONGGPU_CTX_STATE3, state,
				   sizeof(*state));
}

int loonggpu_cs_submit_raw2(loonggpu_device_handle dev, loonggpu_context_handle context,
			    uint32_t bo_list_handle, int num_chunks,
			    struct drm_loonggpu_cs_chunk *chunks, uint64_t *seq_no)
{
	union drm_loonggpu_cs args;
	uint64_t *chunk_array;
	int i, r;

	if (!dev || !context || context->dev != dev || num_chunks <= 0 || !chunks)
		return -EINVAL;

	chunk_array = calloc(num_chunks, sizeof(uint64_t));
	if (!chunk_array)
		return -ENOMEM;
	for (i = 0; i < num_chunks; i++)
		chunk_array[i] = (uintptr_t)&chunks[i];

	memset(&args, 0, sizeof(args));
	args.in.ctx_id = context->id;
	args.in.bo_list_handle = bo_list_handle;
	args.in.num_chunks = num_chunks;
	args.in.chunks = (uintptr_t)chunk_array;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_CS, &args, sizeof(args));
	free(chunk_array);
	if (!r && seq_no)
		*seq_no = args.out.handle;
	return r;
}

int loonggpu_cs_submit_raw(loonggpu_device_handle dev, loonggpu_context_handle context,
			   loonggpu_bo_list_handle bo_list_handle, int num_chunks,
			   struct drm_loonggpu_cs_chunk *chunks, uint64_t *seq_no)
{
	if (bo_list_handle && bo_list_handle->dev != dev)
		return -EINVAL;
	return loonggpu_cs_submit_raw2(dev, context,
				       bo_list_handle ? bo_list_handle->handle : 0,
				       num_chunks, chunks, seq_no);
}

int loonggpu_cs_submit(loonggpu_device_handle dev, uint64_t flags,
		       struct loonggpu_cs_request *req, uint32_t number_of_requests)
{
	struct drm_loonggpu_cs_chunk *chunks;
	union loonggpu_cs_chunk_data *chunk_data;
	uint32_t n, i;
	int r;

	/* No submission flags are defined yet, so an unknown one is an error
	 * rather than something silently dropped. */
	if (flags)
		return -EINVAL;
	if (!dev || !req || !number_of_requests)
		return -EINVAL;
	/* The multi-request form is not implemented: every caller submits one
	 * request at a time. */
	if (number_of_requests != 1)
		return -EINVAL;
	if (req->flags || !req->number_of_ibs || !req->ibs || !req->context)
		return -EINVAL;
	if (req->context->dev != dev ||
	    (req->resources && req->resources->dev != dev) ||
	    (req->fence_memory && req->fence_memory->dev != dev))
		return -EINVAL;
	if (req->number_of_ibs > INT_MAX - (req->fence_memory ? 1 : 0))
		return -EINVAL;
	if (req->fence_memory) {
		uint64_t size = loonggpu_bo_size(req->fence_memory);

		if (req->fence_offset > UINT32_MAX - sizeof(uint64_t) ||
		    (req->fence_offset & (sizeof(uint64_t) - 1)))
			return -EINVAL;
		if (size && (size < sizeof(uint64_t) ||
			     req->fence_offset > size - sizeof(uint64_t)))
			return -EINVAL;
	}

	n = req->number_of_ibs + (req->fence_memory ? 1 : 0);
	chunks = calloc(n, sizeof(*chunks));
	chunk_data = calloc(n, sizeof(*chunk_data));
	if (!chunks || !chunk_data) {
		free(chunks);
		free(chunk_data);
		return -ENOMEM;
	}

	for (i = 0; i < req->number_of_ibs; i++) {
		struct drm_loonggpu_cs_chunk_ib *ib = &chunk_data[i].ib;

		ib->va_start = req->ibs[i].ib_mc_address;
		ib->ib_bytes = req->ibs[i].size;
		ib->ip_type = req->ip_type;
		ib->ip_instance = req->ip_instance;
		ib->ring = req->ring;
		ib->flags = req->ibs[i].flags;
		chunks[i].chunk_id = LOONGGPU_CHUNK_ID_IB;
		chunks[i].length_dw = sizeof(*ib) / 4;
		chunks[i].chunk_data = (uintptr_t)ib;
	}

	if (req->fence_memory) {
		struct drm_loonggpu_cs_chunk_fence *fence = &chunk_data[n - 1].fence;

		fence->handle = loonggpu_bo_gem_handle(req->fence_memory);
		fence->offset = (uint32_t)req->fence_offset;
		chunks[n - 1].chunk_id = LOONGGPU_CHUNK_ID_FENCE;
		chunks[n - 1].length_dw = sizeof(*fence) / 4;
		chunks[n - 1].chunk_data = (uintptr_t)fence;
	}
	/* Keep the recorded sequence in submission order even when callers share
	 * a context across threads. */
	pthread_mutex_lock(&req->context->sequence_mutex);
	r = loonggpu_cs_submit_raw(dev, req->context, req->resources, n, chunks, &req->seq_no);
	if (!r && req->ip_type < LOONGGPU_HW_IP_NUM &&
	    req->ip_instance < LOONGGPU_HW_IP_INSTANCE_MAX_COUNT &&
	    req->ring < LOONGGPU_CS_MAX_RINGS) {
		req->context->last_seq[req->ip_type][req->ip_instance][req->ring] =
			req->seq_no;
	}
	pthread_mutex_unlock(&req->context->sequence_mutex);
	free(chunks);
	free(chunk_data);
	return r;
}

void loonggpu_cs_chunk_fence_info_to_data(struct loonggpu_cs_fence_info *fence_info,
					  struct drm_loonggpu_cs_chunk_data *data)
{
	uint64_t off;

	if (!data)
		return;
	memset(data, 0, sizeof(*data));
	if (!fence_info || !fence_info->handle)
		return;

	/* The public offset is in units of uint64_t, the chunk offset is in
	 * bytes. */
	if (fence_info->offset > UINT32_MAX / sizeof(uint64_t))
		return;
	off = fence_info->offset * sizeof(uint64_t);

	data->fence_data.handle = loonggpu_bo_gem_handle(fence_info->handle);
	data->fence_data.offset = (uint32_t)off;
}

void loonggpu_cs_chunk_fence_to_dep(struct loonggpu_cs_fence *fence,
				    struct drm_loonggpu_cs_chunk_dep *dep)
{
	if (!dep)
		return;
	memset(dep, 0, sizeof(*dep));
	if (!fence || !fence->context)
		return;
	dep->ip_type = fence->ip_type;
	dep->ip_instance = fence->ip_instance;
	dep->ring = fence->ring;
	dep->ctx_id = fence->context->id;
	dep->handle = fence->fence;
}

int loonggpu_cs_query_fence_status(struct loonggpu_cs_fence *fence, uint64_t timeout_ns,
				   uint64_t flags, uint32_t *expired)
{
	union drm_loonggpu_wait_cs args;
	int r;

	if (!fence || !fence->context || !expired ||
	    (flags & ~(uint64_t)LOONGGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE))
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.handle = fence->fence;
	if (flags & LOONGGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE)
		args.in.timeout = timeout_ns;
	else
		args.in.timeout = loonggpu_timeout_to_deadline(timeout_ns);
	args.in.ip_type = fence->ip_type;
	args.in.ip_instance = fence->ip_instance;
	args.in.ring = fence->ring;
	args.in.ctx_id = fence->context->id;
	r = drmCommandWriteRead(fence->context->dev->fd, DRM_LOONGGPU_WAIT_CS,
				&args, sizeof(args));
	if (!r)
		*expired = (args.out.status == 0);
	return r;
}

int loonggpu_cs_query_fence_result(struct loonggpu_cs_fence *fence,
				   struct loonggpu_cs_fence_result *result)
{
	union drm_loonggpu_fence_status args;
	int r;

	if (!fence || !fence->context || !result)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	memset(result, 0, sizeof(*result));
	args.in.ctx_id = fence->context->id;
	args.in.ip_type = fence->ip_type;
	args.in.ip_instance = fence->ip_instance;
	args.in.ring = fence->ring;
	args.in.seq_no = fence->fence;
	r = drmCommandWriteRead(fence->context->dev->fd, DRM_LOONGGPU_FENCE_STATUS,
				&args, sizeof(args));
	if (r)
		return r;
	result->flags = args.out.flags;
	result->error = args.out.error;
	result->seq_epoch = args.out.seq_epoch;
	result->current_epoch = args.out.current_epoch;
	result->confirmed = args.out.confirmed;
	return 0;
}

int loonggpu_cs_wait_fences(struct loonggpu_cs_fence *fences, uint32_t fence_count,
			    bool wait_all, uint64_t timeout_ns, uint32_t *status,
			    uint32_t *first)
{
	struct drm_loonggpu_fence *arr;
	union drm_loonggpu_wait_fences args;
	uint32_t i;
	int r;

	if (!fences || !fence_count)
		return -EINVAL;
	arr = calloc(fence_count, sizeof(*arr));
	if (!arr)
		return -ENOMEM;
	for (i = 0; i < fence_count; i++) {
		if (!fences[i].context) {
			free(arr);
			return -EINVAL;
		}
		/*
		 * A context id is only meaningful inside its own DRM file, so a
		 * second device's context would resolve the id in fences[0]'s fd
		 * and wait on the wrong job.
		 */
		if (fences[i].context->dev != fences[0].context->dev) {
			free(arr);
			return -EINVAL;
		}
		if (fences[i].ip_type >= LOONGGPU_HW_IP_NUM ||
		    fences[i].ip_instance >= LOONGGPU_HW_IP_INSTANCE_MAX_COUNT ||
		    fences[i].ring >= LOONGGPU_CS_MAX_RINGS) {
			free(arr);
			return -EINVAL;
		}
		arr[i].ctx_id = fences[i].context->id;
		arr[i].ip_type = fences[i].ip_type;
		arr[i].ip_instance = fences[i].ip_instance;
		arr[i].ring = fences[i].ring;
		arr[i].seq_no = fences[i].fence;
	}
	memset(&args, 0, sizeof(args));
	args.in.fences = (uintptr_t)arr;
	args.in.fence_count = fence_count;
	args.in.wait_all = wait_all ? 1 : 0;
	args.in.timeout_ns = loonggpu_timeout_to_deadline(timeout_ns);
	r = drmCommandWriteRead(fences[0].context->dev->fd, DRM_LOONGGPU_WAIT_FENCES,
				&args, sizeof(args));
	free(arr);
	if (!r) {
		if (status)
			*status = args.out.status;
		if (first)
			*first = args.out.first_signaled;
	}
	return r;
}

/*
 * Semaphores are a user-space convenience: a semaphore records the sequence of
 * the last submission on one ring of one context, and waiting on it waits for
 * that submission.  The recorded context is kept alive by a reference, because
 * the client is allowed to free its context handle between the signal and the
 * wait.
 */
int loonggpu_cs_create_semaphore(loonggpu_semaphore_handle *sem)
{
	struct loonggpu_semaphore *s;

	if (!sem)
		return -EINVAL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	atomic_set(&s->refcount, 1);
	*sem = s;
	return 0;
}

int loonggpu_cs_signal_semaphore(loonggpu_context_handle ctx, uint32_t ip_type,
				 uint32_t ip_instance, uint32_t ring,
				 loonggpu_semaphore_handle sem)
{
	loonggpu_context_handle previous;

	if (!ctx || !sem)
		return -EINVAL;
	if (ip_type >= LOONGGPU_HW_IP_NUM ||
	    ip_instance >= LOONGGPU_HW_IP_INSTANCE_MAX_COUNT ||
	    ring >= LOONGGPU_CS_MAX_RINGS)
		return -EINVAL;

	/*
	 * The fence below keeps naming this context id, so the semaphore holds a
	 * reference on it.  A re-signal with another context moves that
	 * reference; taking the new one before dropping the old one keeps the
	 * count from reaching zero in between.
	 */
	if (sem->signal_fence.context != ctx) {
		previous = sem->signal_fence.context;
		atomic_inc(&ctx->refcount);
		sem->signal_fence.context = ctx;
		if (previous)
			loonggpu_context_put(previous);
	}
	sem->signal_fence.ip_type = ip_type;
	sem->signal_fence.ip_instance = ip_instance;
	sem->signal_fence.ring = ring;
	pthread_mutex_lock(&ctx->sequence_mutex);
	sem->signal_fence.fence = ctx->last_seq[ip_type][ip_instance][ring];
	pthread_mutex_unlock(&ctx->sequence_mutex);
	sem->signaled = 1;
	return 0;
}

int loonggpu_cs_wait_semaphore(loonggpu_context_handle ctx, uint32_t ip_type,
			       uint32_t ip_instance, uint32_t ring,
			       loonggpu_semaphore_handle sem)
{
	uint32_t expired = 0;

	if (!ctx || !sem || !sem->signaled)
		return -EINVAL;
	/* The engine arguments are part of the API for symmetry with the signal;
	 * the wait uses the engine the semaphore recorded. */
	if (ip_type >= LOONGGPU_HW_IP_NUM ||
	    ip_instance >= LOONGGPU_HW_IP_INSTANCE_MAX_COUNT ||
	    ring >= LOONGGPU_CS_MAX_RINGS)
		return -EINVAL;
	return loonggpu_cs_query_fence_status(&sem->signal_fence,
					      LOONGGPU_TIMEOUT_INFINITE, 0, &expired);
}

int loonggpu_cs_destroy_semaphore(loonggpu_semaphore_handle sem)
{
	if (!sem)
		return -EINVAL;
	if (atomic_dec_and_test(&sem->refcount)) {
		loonggpu_context_handle ctx = sem->signal_fence.context;

		sem->signal_fence.context = NULL;
		free(sem);
		return loonggpu_context_put(ctx);
	}
	return 0;
}

/* Sync object helpers, all of them thin wrappers over the generic DRM ones. */

int loonggpu_cs_create_syncobj2(loonggpu_device_handle dev, uint32_t flags, uint32_t *handle)
{
	if (!dev || !handle)
		return -EINVAL;
	return drmSyncobjCreate(dev->fd, flags, handle);
}

int loonggpu_cs_create_syncobj(loonggpu_device_handle dev, uint32_t *handle)
{
	return loonggpu_cs_create_syncobj2(dev, 0, handle);
}

int loonggpu_cs_destroy_syncobj(loonggpu_device_handle dev, uint32_t handle)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjDestroy(dev->fd, handle);
}

int loonggpu_cs_syncobj_reset(loonggpu_device_handle dev, const uint32_t *syncobjs, uint32_t count)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjReset(dev->fd, syncobjs, count);
}

int loonggpu_cs_syncobj_signal(loonggpu_device_handle dev, const uint32_t *syncobjs, uint32_t count)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjSignal(dev->fd, syncobjs, count);
}

int loonggpu_cs_syncobj_timeline_signal(loonggpu_device_handle dev, const uint32_t *syncobjs,
					uint64_t *points, uint32_t count)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjTimelineSignal(dev->fd, syncobjs, points, count);
}

int loonggpu_cs_syncobj_wait(loonggpu_device_handle dev, uint32_t *handles, unsigned num_handles,
			     int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjWait(dev->fd, handles, num_handles, timeout_nsec, flags,
			      first_signaled);
}

int loonggpu_cs_syncobj_timeline_wait(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points,
				      unsigned num_handles, int64_t timeout_nsec, unsigned flags,
				      uint32_t *first_signaled)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjTimelineWait(dev->fd, handles, points, num_handles, timeout_nsec,
				      flags, first_signaled);
}

int loonggpu_cs_syncobj_query(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points,
			      unsigned num_handles)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjQuery(dev->fd, handles, points, num_handles);
}

int loonggpu_cs_syncobj_query2(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points,
			       unsigned num_handles, uint32_t flags)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjQuery2(dev->fd, handles, points, num_handles, flags);
}

int loonggpu_cs_export_syncobj(loonggpu_device_handle dev, uint32_t handle, int *shared_fd)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjHandleToFD(dev->fd, handle, shared_fd);
}

int loonggpu_cs_import_syncobj(loonggpu_device_handle dev, int shared_fd, uint32_t *handle)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjFDToHandle(dev->fd, shared_fd, handle);
}

int loonggpu_cs_syncobj_export_sync_file(loonggpu_device_handle dev, uint32_t syncobj, int *sync_file_fd)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjExportSyncFile(dev->fd, syncobj, sync_file_fd);
}

int loonggpu_cs_syncobj_import_sync_file(loonggpu_device_handle dev, uint32_t syncobj, int sync_file_fd)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjImportSyncFile(dev->fd, syncobj, sync_file_fd);
}

int loonggpu_cs_syncobj_export_sync_file2(loonggpu_device_handle dev, uint32_t syncobj, uint64_t point,
					  uint32_t flags, int *sync_file_fd)
{
	uint32_t tmp = 0;
	int r;

	if (!dev)
		return -EINVAL;
	if (!point)
		return loonggpu_cs_syncobj_export_sync_file(dev, syncobj, sync_file_fd);
	/* A sync_file can only carry a binary fence, so materialise the queried
	 * point into a temporary binary sync object first. */
	r = drmSyncobjCreate(dev->fd, 0, &tmp);
	if (r)
		return r;
	r = drmSyncobjTransfer(dev->fd, tmp, 0, syncobj, point, flags);
	if (!r)
		r = drmSyncobjExportSyncFile(dev->fd, tmp, sync_file_fd);
	drmSyncobjDestroy(dev->fd, tmp);
	return r;
}

int loonggpu_cs_syncobj_import_sync_file2(loonggpu_device_handle dev, uint32_t syncobj, uint64_t point,
					  int sync_file_fd)
{
	uint32_t tmp = 0;
	int r;

	if (!dev)
		return -EINVAL;
	if (!point)
		return loonggpu_cs_syncobj_import_sync_file(dev, syncobj, sync_file_fd);
	r = drmSyncobjCreate(dev->fd, 0, &tmp);
	if (r)
		return r;
	r = drmSyncobjImportSyncFile(dev->fd, tmp, sync_file_fd);
	if (!r)
		r = drmSyncobjTransfer(dev->fd, syncobj, point, tmp, 0, 0);
	drmSyncobjDestroy(dev->fd, tmp);
	return r;
}

int loonggpu_cs_syncobj_transfer(loonggpu_device_handle dev, uint32_t dst_handle, uint64_t dst_point,
				 uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	if (!dev)
		return -EINVAL;
	return drmSyncobjTransfer(dev->fd, dst_handle, dst_point, src_handle, src_point, flags);
}

int loonggpu_cs_fence_to_handle(loonggpu_device_handle dev, struct loonggpu_cs_fence *fence,
				uint32_t what, uint32_t *out_handle)
{
	union drm_loonggpu_fence_to_handle args;
	int r;

	if (!dev || !fence || !fence->context || !out_handle ||
	    fence->context->dev != dev)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.fence.ctx_id = fence->context->id;
	args.in.fence.ip_type = fence->ip_type;
	args.in.fence.ip_instance = fence->ip_instance;
	args.in.fence.ring = fence->ring;
	args.in.fence.seq_no = fence->fence;
	args.in.what = what;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_FENCE_TO_HANDLE, &args, sizeof(args));
	if (!r)
		*out_handle = args.out.handle;
	return r;
}

int loonggpu_hw_sema_get(loonggpu_device_handle dev, uint32_t *handle)
{
	struct drm_loonggpu_hw_sema args;
	int r;

	if (!dev || !handle)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.ops = LOONGGPU_HW_SEMA_GET;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_HWSEMA_OP, &args, sizeof(args));
	if (!r)
		*handle = (uint32_t)args.id;
	return r;
}

int loonggpu_hw_sema_put(loonggpu_device_handle dev, uint32_t handle)
{
	struct drm_loonggpu_hw_sema args;

	if (!dev)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.ops = LOONGGPU_HW_SEMA_PUT;
	args.id = handle;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_HWSEMA_OP, &args, sizeof(args));
}
