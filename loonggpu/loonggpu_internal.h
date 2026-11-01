/* SPDX-License-Identifier: MIT */
#ifndef LOONGGPU_INTERNAL_H
#define LOONGGPU_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include "handle_table.h"
#include <xf86atomic.h>
#include "libdrm_macros.h"
#include "util_math.h"
#include "loonggpu.h"
#include "loonggpu_drm.h"

/** Number of rings per engine the sequence bookkeeping keeps track of. */
#define LOONGGPU_CS_MAX_RINGS 8
#define LOONGGPU_INVALID_VA 0xffffffffffffffffull

/** One free range in the userspace VA manager, sorted by offset. */
struct loonggpu_va_node {
	struct loonggpu_va_node *next;
	uint64_t offset;
	uint64_t size;
};

struct loonggpu_va_mgr {
	pthread_mutex_t mutex;
	/* Bump pointer for fresh allocations.  It moves as space is handed out,
	 * so it is not the lower bound of a legal free. */
	uint64_t va_offset;
	/* Bounds of the managed range, set by loonggpu_vamgr_init(). */
	uint64_t va_base;
	uint64_t va_max;
	uint64_t alignment;
	struct loonggpu_va_node *holes;
};

struct loonggpu_device {
	int fd;
	/* References and the process registry are protected by
	 * device_registry_mutex in loonggpu_device.c. */
	unsigned refs;
	bool registered;
	struct loonggpu_device *registry_next;
	unsigned major_version;
	unsigned minor_version;
	char *marketing_name;
	struct drm_loonggpu_info_device dev_info;
	struct loonggpu_gpu_info info;
	struct loonggpu_va_mgr va_mgr;
	pthread_mutex_t bo_table_mutex;
	struct loonggpu_handle_table bo_handles;
};

struct loonggpu_bo {
	atomic_t refcount;
	struct loonggpu_device *dev;
	uint32_t handle;
	/* alloc_size is written by both the import path and
	 * loonggpu_bo_cpu_map(), which is why it is read under
	 * cpu_access_mutex. */
	uint64_t alloc_size;
	uint64_t align;
	uint32_t preferred_heap;
	uint64_t flags;
	/* Whether the GEM handle belongs to this wrapper and must be closed on
	 * free.  Borrowed handles (kms import) keep the caller's ownership. */
	bool owns_handle;
	/* Set once the BO leaves the library through export or import; sticky so
	 * that a cache of "private" BOs cannot go stale. */
	bool shared;
	pthread_mutex_t cpu_access_mutex;
	void *cpu_ptr;
	/* Length cpu_ptr was mmap()ed with.  munmap() must use exactly this
	 * value: alloc_size is not known for every BO (a borrowed KMS handle
	 * starts with size 0), and unmapping with a different length either
	 * leaks the mapping or tears down neighbouring VMAs. */
	uint64_t cpu_map_size;
	int cpu_map_count;
};

struct loonggpu_bo_list {
	struct loonggpu_device *dev;
	uint32_t handle;
};

struct loonggpu_context {
	struct loonggpu_device *dev;
	uint32_t id;
	/* Semaphores retain both the kernel context and its userspace wrapper
	 * until their recorded fence is no longer needed. */
	atomic_t refcount;
	pthread_mutex_t sequence_mutex;
	uint64_t last_seq[LOONGGPU_HW_IP_NUM][LOONGGPU_HW_IP_INSTANCE_MAX_COUNT][LOONGGPU_CS_MAX_RINGS];
};

struct loonggpu_va {
	struct loonggpu_device *dev;
	struct loonggpu_va_mgr *mgr;
	uint64_t address;
	uint64_t size;
};

struct loonggpu_semaphore {
	atomic_t refcount;
	struct loonggpu_cs_fence signal_fence;
	int signaled;
};

drm_private int loonggpu_query_gpu_info_init(loonggpu_device_handle dev);
drm_private void loonggpu_parse_asic_ids(loonggpu_device_handle dev);
drm_private void loonggpu_vamgr_init(struct loonggpu_va_mgr *mgr, uint64_t start,
				     uint64_t max, uint64_t alignment);
drm_private void loonggpu_vamgr_deinit(struct loonggpu_va_mgr *mgr);
drm_private int loonggpu_vamgr_alloc(struct loonggpu_va_mgr *mgr, uint64_t size,
				     uint64_t align, uint64_t base_required,
				     uint64_t *allocated);
drm_private int loonggpu_vamgr_free(struct loonggpu_va_mgr *mgr, uint64_t address,
				    uint64_t size);
drm_private uint64_t loonggpu_timeout_to_deadline(uint64_t timeout);
drm_private void loonggpu_device_ref_internal(struct loonggpu_device *dev);
drm_private void loonggpu_device_unref_internal(struct loonggpu_device *dev);

static inline void loonggpu_bo_ref(struct loonggpu_bo *bo)
{
	atomic_inc(&bo->refcount);
}

/* alloc_size may be populated by a concurrent query or CPU mapping. */
static inline uint64_t loonggpu_bo_size(struct loonggpu_bo *bo)
{
	uint64_t size;

	pthread_mutex_lock(&bo->cpu_access_mutex);
	size = bo->alloc_size;
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	return size;
}

#endif
