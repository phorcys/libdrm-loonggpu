/* SPDX-License-Identifier: MIT */
/*
 * Device registry and child object lifetime, with the DRM entry points
 * interposed so no GPU node is needed.
 *
 * The registry is keyed by open file description, so two fds of the same
 * description must give the same handle, and a device must stay alive while a
 * BO, a VA range, a BO list or a semaphore still refers to it.
 */

#include "loonggpu_internal.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static unsigned next_handle = 7;
static unsigned gem_closes;
/* Every context gets its own id, so a wait that presents the wrong one is
 * visible instead of accidentally matching. */
static uint32_t next_ctx_id = 9;
static uint32_t last_wait_cs_ctx_id;
static bool live_contexts[64];
static unsigned context_frees;
static int list_destroy_error;

drmVersionPtr drmGetVersion(int fd)
{
	drmVersionPtr version;

	assert(fcntl(fd, F_GETFD) >= 0);
	version = calloc(1, sizeof(*version));
	assert(version);
	version->version_major = 1;
	version->name = "loonggpu";
	return version;
}

void drmFreeVersion(drmVersionPtr version)
{
	free(version);
}

int drmCommandWriteRead(int fd, unsigned long cmd, void *data,
			unsigned long size)
{
	(void)size;
	assert(fcntl(fd, F_GETFD) >= 0);
	if (cmd == DRM_LOONGGPU_INFO) {
		struct drm_loonggpu_info *info = data;
		void *out = (void *)(uintptr_t)info->return_pointer;

		if (info->query == LOONGGPU_INFO_ACCEL_WORKING) {
			*(uint32_t *)out = 1;
		} else if (info->query == LOONGGPU_INFO_DEV_INFO) {
			struct drm_loonggpu_info_device *dev_info = out;

			dev_info->virtual_address_offset = 0x100000000ull;
			dev_info->virtual_address_max = 0x200000000ull;
			dev_info->virtual_address_alignment = 4096;
		} else {
			abort();
		}
		return 0;
	}
	if (cmd == DRM_LOONGGPU_GEM_CREATE) {
		((union drm_loonggpu_gem_create *)data)->out.handle = next_handle++;
		return 0;
	}
	if (cmd == DRM_LOONGGPU_CTX) {
		union drm_loonggpu_ctx *ctx = data;
		uint32_t op = ctx->in.op;

		if (op == LOONGGPU_CTX_OP_ALLOC_CTX) {
			ctx->out.alloc.ctx_id = next_ctx_id++;
			assert(ctx->out.alloc.ctx_id < 64);
			live_contexts[ctx->out.alloc.ctx_id] = true;
		} else if (op == LOONGGPU_CTX_OP_QUERY_STATE2) {
			ctx->out.state.flags = LOONGGPU_CTX_QUERY2_FLAGS_RESET |
					       LOONGGPU_CTX_QUERY2_FLAGS_GUILTY |
					       LOONGGPU_CTX_QUERY2_FLAGS_RECOVERY_PENDING |
					       LOONGGPU_CTX_QUERY2_FLAGS_DEVICE_LOST;
		} else if (op == LOONGGPU_CTX_OP_FREE_CTX) {
			assert(ctx->in.ctx_id < 64 && live_contexts[ctx->in.ctx_id]);
			live_contexts[ctx->in.ctx_id] = false;
			context_frees++;
		} else {
			abort();
		}
		return 0;
	}
	if (cmd == DRM_LOONGGPU_WAIT_CS) {
		union drm_loonggpu_wait_cs *wait = data;

		/* The wait is where a semaphore's context id is read; record what
		 * the caller presented. */
		assert(wait->in.ctx_id < 64 && live_contexts[wait->in.ctx_id]);
		assert(wait->in.handle == 37);
		last_wait_cs_ctx_id = wait->in.ctx_id;
		wait->out.status = 0;
		return 0;
	}
	if (cmd == DRM_LOONGGPU_CTX_STATE3) {
		struct drm_loonggpu_ctx_state3 *state = data;

		state->flags = LOONGGPU_CTX_QUERY2_FLAGS_RESET |
			       LOONGGPU_CTX_QUERY2_FLAGS_RECOVERY_PENDING |
			       LOONGGPU_CTX_QUERY2_FLAGS_DEVICE_LOST;
		state->reset_epoch = 8;
		state->admission_epoch = 13;
		state->admission_state = 3;
		state->quiesced_domains = 0;
		state->hangs = 2;
		return 0;
	}
	if (cmd == DRM_LOONGGPU_BO_LIST) {
		union drm_loonggpu_bo_list *list = data;
		uint32_t operation = list->in.operation;

		if (operation == LOONGGPU_BO_LIST_OP_CREATE)
			list->out.list_handle = 11;
		else if (operation == LOONGGPU_BO_LIST_OP_DESTROY) {
			assert(list->in.list_handle == 11);
			return list_destroy_error;
		} else
			abort();
		return 0;
	}
	abort();
}

int drmIoctl(int fd, unsigned long request, void *data)
{
	assert(fcntl(fd, F_GETFD) >= 0);
	if (request == DRM_IOCTL_GEM_CLOSE) {
		assert(((struct drm_gem_close *)data)->handle != 0);
		gem_closes++;
		return 0;
	}
	abort();
}

int main(void)
{
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = 4096,
		.phys_alignment = 4096,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
	};
	loonggpu_device_handle dev_a = NULL, dev_same = NULL, dev_other = NULL;
	loonggpu_device_handle cloned = NULL;
	loonggpu_bo_handle bo = NULL;
	loonggpu_bo_list_handle list = NULL;
	loonggpu_context_handle context = NULL;
	loonggpu_va_handle va = NULL;
	struct drm_loonggpu_ctx_state3 state3;
	uint64_t va_address = 0;
	uint32_t flags = 0;
	int a = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int same = dup(a);
	int other = open("/dev/null", O_RDONLY | O_CLOEXEC);

	assert(a >= 0 && same >= 0 && other >= 0);

	/* Two fds of the same description share one device; a separate open of
	 * the same node does not. */
	assert(loonggpu_device_initialize(a, NULL, NULL, &dev_a) == 0);
	assert(loonggpu_device_initialize(same, NULL, NULL, &dev_same) == 0);
	assert(dev_same == dev_a);
	assert(loonggpu_device_initialize(other, NULL, NULL, &dev_other) == 0);
	assert(dev_other != dev_a);

	assert(loonggpu_device_reference(dev_a, &cloned) == 0 && cloned == dev_a);

	assert(loonggpu_bo_alloc(dev_a, &request, &bo) == 0 && bo);
	assert(loonggpu_bo_list_create(dev_a, 1, &bo, NULL, &list) == 0 && list);
	assert(loonggpu_cs_ctx_create(dev_a, &context) == 0 && context);
	assert(loonggpu_cs_query_reset_state2(context, &flags, NULL) == 0 &&
	       flags == (LOONGGPU_CTX_QUERY2_FLAGS_RESET |
			 LOONGGPU_CTX_QUERY2_FLAGS_GUILTY |
			 LOONGGPU_CTX_QUERY2_FLAGS_RECOVERY_PENDING |
			 LOONGGPU_CTX_QUERY2_FLAGS_DEVICE_LOST));
	assert(loonggpu_cs_query_state3(context, &state3) == 0);
	assert(state3.reset_epoch == 8 && state3.admission_epoch == 13 &&
	       state3.quiesced_domains == 0 &&
	       (state3.flags & LOONGGPU_CTX_QUERY2_FLAGS_DEVICE_LOST));
	assert(loonggpu_va_range_alloc(dev_a, loonggpu_gpu_va_range_general,
				       4096, 4096, 0, &va_address, &va, 0) == 0);
	assert(va_address && va);

	/* The caller's fds and references go away; the BO and the VA range still
	 * own the dup and the allocator. */
	close(a);
	close(same);
	assert(loonggpu_device_deinitialize(dev_a) == 0);
	assert(loonggpu_device_deinitialize(dev_same) == 0);
	assert(loonggpu_device_deinitialize(cloned) == 0);
	assert(fcntl(loonggpu_device_get_fd(dev_a), F_GETFD) >= 0);
	assert(loonggpu_va_range_free(va) == 0);
	assert(loonggpu_bo_free(bo) == 0);
	assert(gem_closes == 1);
	assert(fcntl(loonggpu_device_get_fd(dev_a), F_GETFD) >= 0);

	/* The mock checks that a semaphore retains the kernel context as well as
	 * its wrapper.  A nonzero sequence must remain queryable after the caller
	 * releases its context reference. */
	{
		uint32_t ctx_id = context->id;
		context->last_seq[LOONGGPU_HW_IP_GFX][0][0] = 37;
		loonggpu_context_handle context2 = NULL;
		loonggpu_semaphore_handle sem = NULL;

		assert(loonggpu_cs_create_semaphore(&sem) == 0 && sem);
		assert(loonggpu_cs_signal_semaphore(context, LOONGGPU_HW_IP_GFX,
						   0, 0, sem) == 0);

		/* A re-signal moves the reference to the new context and back;
		 * piling references up would leak, dropping one too many would
		 * free a context that is still named. */
		assert(loonggpu_cs_ctx_create2(dev_a, 0, &context2) == 0 && context2);
		assert(context2->id != ctx_id);
		assert(loonggpu_cs_signal_semaphore(context2, LOONGGPU_HW_IP_GFX,
						   0, 0, sem) == 0);
		assert(loonggpu_cs_signal_semaphore(context, LOONGGPU_HW_IP_GFX,
						   0, 0, sem) == 0);
		assert(loonggpu_cs_ctx_free(context2) == 0);

		/* The handle goes away while the semaphore still names it ... */
		assert(loonggpu_cs_ctx_free(context) == 0);
		assert(context_frees == 1 && live_contexts[ctx_id]);
		assert(fcntl(loonggpu_device_get_fd(dev_a), F_GETFD) >= 0);

		/* ... and the wait still reads a live context with its own id. */
		assert(loonggpu_cs_wait_semaphore(context, LOONGGPU_HW_IP_GFX,
						  0, 0, sem) == 0);
		assert(last_wait_cs_ctx_id == ctx_id);

		/* The last reference does the teardown, exactly once. */
		assert(loonggpu_cs_destroy_semaphore(sem) == 0);
		assert(context_frees == 2 && !live_contexts[ctx_id]);
		assert(fcntl(loonggpu_device_get_fd(dev_a), F_GETFD) >= 0);
	}
	list_destroy_error = -EIO;
	assert(loonggpu_bo_list_destroy(list) == -EIO);
	assert(fcntl(loonggpu_device_get_fd(dev_a), F_GETFD) >= 0);
	list_destroy_error = 0;
	assert(loonggpu_bo_list_destroy(list) == 0);
	assert(loonggpu_device_deinitialize(dev_other) == 0);
	close(other);
	puts("device description and child object lifetime checks passed");
	return 0;
}
