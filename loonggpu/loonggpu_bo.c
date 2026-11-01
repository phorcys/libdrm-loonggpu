/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "loonggpu_internal.h"
#include "handle_table.h"

static struct loonggpu_bo *
bo_create(struct loonggpu_device *dev, uint32_t handle, uint64_t size,
	  uint64_t align, uint32_t heap, uint64_t flags, bool owns_handle)
{
	struct loonggpu_bo *bo = calloc(1, sizeof(*bo));

	if (!bo)
		return NULL;
	atomic_set(&bo->refcount, 1);
	bo->dev = dev;
	bo->handle = handle;
	bo->alloc_size = size;
	bo->align = align;
	bo->preferred_heap = heap;
	bo->flags = flags;
	bo->owns_handle = owns_handle;
	pthread_mutex_init(&bo->cpu_access_mutex, NULL);
	loonggpu_device_ref_internal(dev);
	return bo;
}

static struct loonggpu_bo *
bo_lookup_locked(struct loonggpu_device *dev, uint32_t handle)
{
	return loonggpu_handle_table_lookup(&dev->bo_handles, handle);
}

static int
bo_insert_locked(struct loonggpu_device *dev, struct loonggpu_bo *bo)
{
	return loonggpu_handle_table_insert(&dev->bo_handles, bo->handle, bo);
}

static void
bo_remove_locked(struct loonggpu_device *dev, struct loonggpu_bo *bo)
{
	loonggpu_handle_table_remove(&dev->bo_handles, bo->handle);
}

/*
 * Look a GEM handle up and wrap it, or return the wrapper that already exists
 * for it.  Callers hold bo_table_mutex so that "not found, insert" cannot race
 * against the final close of the same handle in loonggpu_bo_free().
 */
static struct loonggpu_bo *
bo_lookup_or_create_locked(struct loonggpu_device *dev, uint32_t handle,
			   uint64_t size, uint64_t align, uint32_t heap,
			   uint64_t flags, bool owns_handle, bool *created)
{
	struct loonggpu_bo *bo;
	int r;

	*created = false;
	bo = bo_lookup_locked(dev, handle);
	if (bo) {
		pthread_mutex_lock(&bo->cpu_access_mutex);
		if (!bo->alloc_size)
			bo->alloc_size = size;
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		loonggpu_bo_ref(bo);
		return bo;
	}

	bo = bo_create(dev, handle, size, align, heap, flags, owns_handle);
	if (!bo)
		return NULL;
	r = bo_insert_locked(dev, bo);
	if (r) {
		pthread_mutex_destroy(&bo->cpu_access_mutex);
		free(bo);
		loonggpu_device_unref_internal(dev);
		return NULL;
	}
	*created = true;
	return bo;
}

static struct loonggpu_bo *
bo_lookup_or_create(struct loonggpu_device *dev, uint32_t handle,
		    uint64_t size, uint64_t align, uint32_t heap,
		    uint64_t flags, bool owns_handle, bool *created)
{
	struct loonggpu_bo *bo;

	pthread_mutex_lock(&dev->bo_table_mutex);
	bo = bo_lookup_or_create_locked(dev, handle, size, align, heap,
					flags, owns_handle, created);
	pthread_mutex_unlock(&dev->bo_table_mutex);
	return bo;
}

/*
 * Fill in a BO size the kernel knows but the import path did not receive: a
 * borrowed KMS handle carries no size, and both mmap() and the bookkeeping
 * need one.  Called with cpu_access_mutex held, like every other writer of
 * bo->alloc_size.
 */
static void
bo_refresh_size(struct loonggpu_device *dev, struct loonggpu_bo *bo)
{
	struct drm_loonggpu_gem_create_in info;
	struct drm_loonggpu_gem_op op;

	if (!dev || !bo || bo->alloc_size)
		return;

	memset(&info, 0, sizeof(info));
	memset(&op, 0, sizeof(op));
	op.handle = bo->handle;
	op.op = LOONGGPU_GEM_OP_GET_GEM_CREATE_INFO;
	op.value = (uint64_t)(uintptr_t)&info;
	if (drmCommandWriteRead(dev->fd, DRM_LOONGGPU_GEM_OP, &op, sizeof(op)))
		return;

	bo->alloc_size = info.bo_size;
}

int loonggpu_bo_alloc(loonggpu_device_handle dev,
		      struct loonggpu_bo_alloc_request *request,
		      loonggpu_bo_handle *buf_handle)
{
	union drm_loonggpu_gem_create args;
	struct loonggpu_bo *bo;
	bool created;
	int r;

	if (!dev || !request || !buf_handle || !request->alloc_size)
		return -EINVAL;
	*buf_handle = NULL;

	memset(&args, 0, sizeof(args));
	args.in.bo_size = request->alloc_size;
	args.in.alignment = request->phys_alignment;
	args.in.domains = request->preferred_heap;
	args.in.domain_flags = request->flags;

	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_GEM_CREATE, &args, sizeof(args));
	if (r)
		return r;

	bo = bo_lookup_or_create(dev, args.out.handle, request->alloc_size,
			      request->phys_alignment, request->preferred_heap,
			      request->flags, true, &created);
	if (!bo) {
		struct drm_gem_close cl = { .handle = args.out.handle };

		drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
		return -ENOMEM;
	}
	if (!created) {
		/* GEM_CREATE hands out a fresh handle, so a wrapper that already
		 * exists means the table and the kernel disagree; drop the handle
		 * we just got rather than aliasing the two objects. */
		struct drm_gem_close cl = { .handle = args.out.handle };

		drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
		loonggpu_bo_free(bo);
		return -EEXIST;
	}
	*buf_handle = bo;
	return 0;
}

int loonggpu_bo_free(loonggpu_bo_handle bo)
{
	struct drm_gem_close cl;
	struct loonggpu_device *dev;

	if (!bo)
		return -EINVAL;
	dev = bo->dev;
	/*
	 * The final close runs under the table mutex that import also holds, so
	 * a concurrent import cannot publish the handle after GEM_CLOSE.
	 */
	pthread_mutex_lock(&dev->bo_table_mutex);
	if (!atomic_dec_and_test(&bo->refcount)) {
		pthread_mutex_unlock(&dev->bo_table_mutex);
		return 0;
	}
	bo_remove_locked(dev, bo);

	if (bo->cpu_ptr && bo->cpu_map_count) {
		munmap(bo->cpu_ptr, bo->cpu_map_size);
		bo->cpu_ptr = NULL;
		bo->cpu_map_size = 0;
		bo->cpu_map_count = 0;
	}
	if (bo->owns_handle) {
		memset(&cl, 0, sizeof(cl));
		cl.handle = bo->handle;
		drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
	}
	pthread_mutex_unlock(&dev->bo_table_mutex);
	pthread_mutex_destroy(&bo->cpu_access_mutex);
	free(bo);
	loonggpu_device_unref_internal(dev);
	return 0;
}

int loonggpu_bo_set_metadata(loonggpu_bo_handle bo, struct loonggpu_bo_metadata *md)
{
	struct drm_loonggpu_gem_metadata args;

	if (!bo || !md)
		return -EINVAL;
	if (md->size_metadata > sizeof(args.data.data))
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.handle = bo->handle;
	args.op = LOONGGPU_GEM_METADATA_OP_SET_METADATA;
	args.data.flags = md->flags;
	args.data.tiling_info = md->tiling_info;
	args.data.data_size_bytes = md->size_metadata;
	memcpy(args.data.data, md->umd_metadata, md->size_metadata);
	return drmCommandWriteRead(bo->dev->fd, DRM_LOONGGPU_GEM_METADATA, &args, sizeof(args));
}

int loonggpu_bo_query_info(loonggpu_bo_handle bo, struct loonggpu_bo_info *info)
{
	struct drm_loonggpu_gem_create_in create_info;
	struct drm_loonggpu_gem_metadata md;
	struct drm_loonggpu_gem_op op;
	int r;

	if (!bo || !info)
		return -EINVAL;
	memset(info, 0, sizeof(*info));
	info->alloc_size = loonggpu_bo_size(bo);
	info->phys_alignment = bo->align;
	info->preferred_heap = bo->preferred_heap;
	info->alloc_flags = bo->flags;

	memset(&md, 0, sizeof(md));
	md.handle = bo->handle;
	md.op = LOONGGPU_GEM_METADATA_OP_GET_METADATA;
	r = drmCommandWriteRead(bo->dev->fd, DRM_LOONGGPU_GEM_METADATA, &md, sizeof(md));
	if (r)
		return r;
	if (md.data.data_size_bytes > sizeof(info->metadata.umd_metadata))
		return -EOVERFLOW;
	info->metadata.flags = md.data.flags;
	info->metadata.tiling_info = md.data.tiling_info;
	info->metadata.size_metadata = md.data.data_size_bytes;
	memcpy(info->metadata.umd_metadata, md.data.data, md.data.data_size_bytes);

	/* The kernel is authoritative for size, placement and creation flags. */
	memset(&create_info, 0, sizeof(create_info));
	memset(&op, 0, sizeof(op));
	op.handle = bo->handle;
	op.op = LOONGGPU_GEM_OP_GET_GEM_CREATE_INFO;
	op.value = (uint64_t)(uintptr_t)&create_info;
	r = drmCommandWriteRead(bo->dev->fd, DRM_LOONGGPU_GEM_OP, &op, sizeof(op));
	if (r)
		return r;

	info->alloc_size = create_info.bo_size;
	info->phys_alignment = create_info.alignment;
	info->preferred_heap = create_info.domains;
	info->alloc_flags = create_info.domain_flags;
	pthread_mutex_lock(&bo->cpu_access_mutex);
	if (!bo->alloc_size)
		bo->alloc_size = create_info.bo_size;
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	return 0;
}

int loonggpu_bo_export(loonggpu_bo_handle bo, enum loonggpu_bo_handle_type type,
		       uint32_t *shared_handle)
{
	int fd, r;

	if (!bo || !shared_handle)
		return -EINVAL;
	/*
	 * Every handle this function hands out can outlive the caller: a flink
	 * name or a dma-buf fd is usable by another process, and a KMS handle is
	 * consumed by the display ioctls.  Mark the BO before any of them can
	 * leave the library.  An ioctl failure may leave the mark set, which is
	 * the conservative direction.
	 */
	__atomic_store_n(&bo->shared, true, __ATOMIC_RELEASE);

	switch (type) {
	case loonggpu_bo_handle_type_kms:
		*shared_handle = bo->handle;
		return 0;
	case loonggpu_bo_handle_type_gem_flink_name: {
		struct drm_gem_flink fl = { .handle = bo->handle };

		r = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &fl);
		if (r)
			return -errno;
		*shared_handle = fl.name;
		return 0;
	}
	case loonggpu_bo_handle_type_dma_buf_fd:
		r = drmPrimeHandleToFD(bo->dev->fd, bo->handle,
				       DRM_CLOEXEC | DRM_RDWR, &fd);
		if (r)
			return r;
		*shared_handle = (uint32_t)fd;
		return 0;
	default:
		return -EINVAL;
	}
}

int loonggpu_bo_import(loonggpu_device_handle dev, enum loonggpu_bo_handle_type type,
		       uint32_t shared_handle, struct loonggpu_bo_import_result *output)
{
	uint32_t handle = 0;
	struct loonggpu_bo *bo = NULL;
	bool created;
	int r = 0;

	if (!dev || !output)
		return -EINVAL;
	memset(output, 0, sizeof(*output));
	/*
	 * GEM_OPEN and PRIME may return a handle this file already owns, and the
	 * lookup below has to publish the wrapper before another thread can
	 * close the last reference to it, so the table lock spans both.
	 */
	pthread_mutex_lock(&dev->bo_table_mutex);

	switch (type) {
	case loonggpu_bo_handle_type_kms:
		/* Same DRM file only: wrap an existing GEM handle. */
		handle = shared_handle;
		bo = bo_lookup_or_create_locked(dev, handle, 0, 0, 0, 0,
						false, &created);
		if (!bo) {
			r = -ENOMEM;
			break;
		}
		/* A borrowed handle is closed by its owner, not by this wrapper. */
		output->buf_handle = bo;
		output->alloc_size = loonggpu_bo_size(bo);
		break;
	case loonggpu_bo_handle_type_gem_flink_name: {
		struct drm_gem_open op = { .name = shared_handle };

		r = drmIoctl(dev->fd, DRM_IOCTL_GEM_OPEN, &op);
		if (r) {
			r = -errno;
			break;
		}
		handle = op.handle;
		bo = bo_lookup_or_create_locked(dev, handle, op.size, 0, 0, 0,
						true, &created);
		if (!bo) {
			struct drm_gem_close cl = { .handle = handle };

			drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
			r = -ENOMEM;
			break;
		}
		output->buf_handle = bo;
		output->alloc_size = loonggpu_bo_size(bo);
		break;
	}
	case loonggpu_bo_handle_type_dma_buf_fd: {
		off_t size;

		r = drmPrimeFDToHandle(dev->fd, (int)shared_handle, &handle);
		if (r)
			break;
		/* dma-buf supports SEEK_END and SEEK_SET with offset zero.
		 * Reset the position after querying the size. */
		size = lseek((int)shared_handle, 0, SEEK_END);
		if (size < 0 || lseek((int)shared_handle, 0, SEEK_SET) < 0)
			size = 0;
		bo = bo_lookup_or_create_locked(dev, handle, (uint64_t)size,
						0, 0, 0, true, &created);
		if (!bo) {
			struct drm_gem_close cl = { .handle = handle };

			drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
			r = -ENOMEM;
			break;
		}
		output->buf_handle = bo;
		output->alloc_size = loonggpu_bo_size(bo);
		break;
	}
	default:
		r = -EINVAL;
		break;
	}
	if (output->buf_handle)
		__atomic_store_n(&output->buf_handle->shared, true, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&dev->bo_table_mutex);
	return r;
}

int loonggpu_create_bo_from_user_mem(loonggpu_device_handle dev, void *cpu,
				     uint64_t size, loonggpu_bo_handle *buf_handle)
{
	struct drm_loonggpu_gem_userptr args;
	struct loonggpu_bo *bo;
	bool created;
	int r;

	if (buf_handle)
		*buf_handle = NULL;
	if (!dev || !cpu || !size || !buf_handle)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.addr = (uintptr_t)cpu;
	args.size = size;
	/* Writable user memory requires an MMU notifier.  Validate the pages
	 * before publishing the handle so registration failures reach the caller. */
	args.flags = LOONGGPU_GEM_USERPTR_REGISTER | LOONGGPU_GEM_USERPTR_VALIDATE;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_GEM_USERPTR, &args, sizeof(args));
	if (r)
		return r;
	bo = bo_lookup_or_create(dev, args.handle, size, 0,
			      LOONGGPU_GEM_DOMAIN_CPU, 0, true, &created);
	if (!bo) {
		struct drm_gem_close cl = { .handle = args.handle };

		drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
		return -ENOMEM;
	}
	if (!created) {
		loonggpu_bo_free(bo);
		return -EEXIST;
	}
	*buf_handle = bo;
	return 0;
}

int loonggpu_bo_cpu_map(loonggpu_bo_handle bo, void **cpu)
{
	union drm_loonggpu_gem_mmap args;
	uint64_t size;
	void *ptr;
	int r;

	if (!bo || !cpu)
		return -EINVAL;

	pthread_mutex_lock(&bo->cpu_access_mutex);
	if (bo->cpu_ptr) {
		bo->cpu_map_count++;
		*cpu = bo->cpu_ptr;
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return 0;
	}

	/*
	 * A borrowed KMS handle is imported without a size, and the value the
	 * kernel returns from GEM_MMAP is an mmap *offset*, not a length.  Ask
	 * for the real object size so that the mapping and the matching
	 * munmap() cover the object instead of arbitrary bytes.
	 */
	bo_refresh_size(bo->dev, bo);
	size = bo->alloc_size;
	if (!size || size > SIZE_MAX) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return -EINVAL;
	}

	memset(&args, 0, sizeof(args));
	args.in.handle = bo->handle;
	r = drmCommandWriteRead(bo->dev->fd, DRM_LOONGGPU_GEM_MMAP, &args, sizeof(args));
	if (r) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return r;
	}
	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   bo->dev->fd, args.out.addr_ptr);
	if (ptr == MAP_FAILED) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return -errno;
	}
	bo->cpu_ptr = ptr;
	bo->cpu_map_size = size;
	bo->cpu_map_count = 1;
	*cpu = ptr;
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	return 0;
}

int loonggpu_bo_cpu_unmap(loonggpu_bo_handle bo)
{
	if (!bo)
		return -EINVAL;
	pthread_mutex_lock(&bo->cpu_access_mutex);
	if (!bo->cpu_ptr || bo->cpu_map_count == 0) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return -EINVAL;
	}
	bo->cpu_map_count--;
	if (bo->cpu_map_count == 0) {
		munmap(bo->cpu_ptr, bo->cpu_map_size);
		bo->cpu_ptr = NULL;
		bo->cpu_map_size = 0;
	}
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	return 0;
}

int loonggpu_bo_wait_for_idle(loonggpu_bo_handle bo, uint64_t timeout_ns, bool *busy)
{
	union drm_loonggpu_gem_wait_idle args;
	int r;

	if (!bo)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.handle = bo->handle;
	args.in.timeout = loonggpu_timeout_to_deadline(timeout_ns);
	r = drmCommandWriteRead(bo->dev->fd, DRM_LOONGGPU_GEM_WAIT_IDLE, &args, sizeof(args));
	if (r)
		return r;
	if (busy)
		*busy = args.out.status != 0;
	return 0;
}

/*
 * Build the array the BO_LIST ioctl takes.  Rejects a NULL entry and a BO that
 * belongs to another device: the first would fault in the caller's address
 * space and the second would send a foreign handle to this fd.
 */
static int
bo_list_build_entries(struct loonggpu_device *dev, uint32_t count,
		      loonggpu_bo_handle *resources, uint8_t *resource_prios,
		      struct drm_loonggpu_bo_list_entry **out)
{
	struct drm_loonggpu_bo_list_entry *entries;
	uint32_t i;

	*out = NULL;
	if (!count)
		return 0;
	if (!resources)
		return -EINVAL;
	entries = calloc(count, sizeof(*entries));
	if (!entries)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		if (!resources[i] || resources[i]->dev != dev) {
			free(entries);
			return -EINVAL;
		}
		entries[i].bo_handle = resources[i]->handle;
		entries[i].bo_priority = resource_prios ? resource_prios[i] : 0;
	}
	*out = entries;
	return 0;
}

int loonggpu_bo_list_create(loonggpu_device_handle dev, uint32_t number_of_resources,
			    loonggpu_bo_handle *resources, uint8_t *resource_prios,
			    loonggpu_bo_list_handle *result)
{
	struct drm_loonggpu_bo_list_entry *entries = NULL;
	union drm_loonggpu_bo_list args;
	struct loonggpu_bo_list *list;
	int r;

	if (result)
		*result = NULL;
	if (!dev || !result)
		return -EINVAL;
	list = calloc(1, sizeof(*list));
	if (!list)
		return -ENOMEM;

	r = bo_list_build_entries(dev, number_of_resources, resources,
				  resource_prios, &entries);
	if (r) {
		free(list);
		return r;
	}

	memset(&args, 0, sizeof(args));
	args.in.operation = LOONGGPU_BO_LIST_OP_CREATE;
	args.in.bo_number = number_of_resources;
	args.in.bo_info_size = sizeof(*entries);
	args.in.bo_info_ptr = (uintptr_t)entries;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_BO_LIST, &args, sizeof(args));
	free(entries);
	if (r) {
		free(list);
		return r;
	}
	list->dev = dev;
	list->handle = args.out.list_handle;
	loonggpu_device_ref_internal(dev);
	*result = list;
	return 0;
}

int loonggpu_bo_list_destroy(loonggpu_bo_list_handle list)
{
	union drm_loonggpu_bo_list args;
	struct loonggpu_device *dev;
	int r;

	if (!list)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.operation = LOONGGPU_BO_LIST_OP_DESTROY;
	args.in.list_handle = list->handle;
	r = drmCommandWriteRead(list->dev->fd, DRM_LOONGGPU_BO_LIST, &args, sizeof(args));
	if (r)
		return r;
	dev = list->dev;
	free(list);
	loonggpu_device_unref_internal(dev);
	return r;
}

int loonggpu_bo_list_update(loonggpu_bo_list_handle list, uint32_t number_of_resources,
			    loonggpu_bo_handle *resources, uint8_t *resource_prios)
{
	struct drm_loonggpu_bo_list_entry *entries = NULL;
	union drm_loonggpu_bo_list args;
	int r;

	if (!list)
		return -EINVAL;
	r = bo_list_build_entries(list->dev, number_of_resources, resources,
				  resource_prios, &entries);
	if (r)
		return r;
	memset(&args, 0, sizeof(args));
	args.in.operation = LOONGGPU_BO_LIST_OP_UPDATE;
	args.in.list_handle = list->handle;
	args.in.bo_number = number_of_resources;
	args.in.bo_info_size = sizeof(*entries);
	args.in.bo_info_ptr = (uintptr_t)entries;
	r = drmCommandWriteRead(list->dev->fd, DRM_LOONGGPU_BO_LIST, &args, sizeof(args));
	free(entries);
	return r;
}

int loonggpu_va_range_query(loonggpu_device_handle dev, enum loonggpu_gpu_va_range type,
			    uint64_t *start, uint64_t *end)
{
	if (!dev || !start || !end)
		return -EINVAL;
	/* Only the general range exists.  Refuse an unknown class instead of
	 * reporting the same window for a type the caller treats as different. */
	if (type != loonggpu_gpu_va_range_general)
		return -EINVAL;
	*start = dev->dev_info.virtual_address_offset;
	*end = dev->dev_info.virtual_address_max;
	return 0;
}

int loonggpu_va_range_alloc(loonggpu_device_handle dev, enum loonggpu_gpu_va_range va_range_type,
			    uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
			    uint64_t *va_base_allocated, loonggpu_va_handle *va_range_handle,
			    uint64_t flags)
{
	struct loonggpu_va *va;
	uint64_t addr = 0;
	int r;

	if (!dev || !va_base_allocated || !va_range_handle)
		return -EINVAL;
	if (va_range_type != loonggpu_gpu_va_range_general || flags)
		return -EINVAL;

	va = calloc(1, sizeof(*va));
	if (!va)
		return -ENOMEM;
	r = loonggpu_vamgr_alloc(&dev->va_mgr, size, va_base_alignment,
				va_base_required, &addr);
	if (r) {
		free(va);
		return r;
	}
	va->mgr = &dev->va_mgr;
	va->dev = dev;
	loonggpu_device_ref_internal(dev);
	va->address = addr;
	va->size = size;
	*va_base_allocated = addr;
	*va_range_handle = va;
	return 0;
}

int loonggpu_va_range_free(loonggpu_va_handle va)
{
	struct loonggpu_device *dev;
	int r;

	if (!va)
		return -EINVAL;
	/* Without a node for the hole the range could never be reused, so the
	 * handle stays valid and the failure is reported to the caller. */
	r = loonggpu_vamgr_free(va->mgr, va->address, va->size);
	if (r)
		return r;
	dev = va->dev;
	free(va);
	loonggpu_device_unref_internal(dev);
	return 0;
}

int loonggpu_bo_va_op_raw(loonggpu_device_handle dev, loonggpu_bo_handle bo,
			  uint64_t offset, uint64_t size, uint64_t addr,
			  uint64_t flags, uint32_t ops)
{
	struct drm_loonggpu_gem_va args;

	if (!dev)
		return -EINVAL;
	/* The UAPI field is 32 bits; refuse rather than reinterpret the high bits
	 * as a different, valid request. */
	if (flags > UINT32_MAX)
		return -EINVAL;
	/* A handle is only meaningful on the device that created it. */
	if (bo && bo->dev != dev)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.handle = bo ? bo->handle : 0;
	args.operation = ops;
	args.flags = (uint32_t)flags;
	args.va_address = addr;
	args.offset_in_bo = offset;
	args.map_size = size;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_GEM_VA, &args, sizeof(args));
}

int loonggpu_bo_va_op(loonggpu_bo_handle bo, uint64_t offset, uint64_t size,
		      uint64_t addr, uint64_t flags, uint32_t ops)
{
	uint64_t f = flags;

	if (!bo)
		return -EINVAL;
	/* Mapping with no flags means the usual readable/writable/executable
	 * mapping; every other operation uses exactly what the caller passed. */
	if (ops == LOONGGPU_VA_OP_MAP && f == 0)
		f = LOONGGPU_VM_PAGE_READABLE | LOONGGPU_VM_PAGE_WRITEABLE |
		    LOONGGPU_VM_PAGE_EXECUTABLE;
	return loonggpu_bo_va_op_raw(bo->dev, bo, offset, size, addr, f, ops);
}

int loonggpu_vm_reserve_vmid(loonggpu_device_handle dev, uint32_t flags)
{
	union drm_loonggpu_vm args;

	if (!dev)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_VM_OP_RESERVE_VMID;
	args.in.flags = flags;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_VM, &args, sizeof(args));
}

int loonggpu_vm_unreserve_vmid(loonggpu_device_handle dev, uint32_t flags)
{
	union drm_loonggpu_vm args;

	if (!dev)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.op = LOONGGPU_VM_OP_UNRESERVE_VMID;
	args.in.flags = flags;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_VM, &args, sizeof(args));
}

uint32_t loonggpu_bo_gem_handle(loonggpu_bo_handle bo)
{
	return bo ? bo->handle : 0;
}

bool loonggpu_bo_is_shared(loonggpu_bo_handle bo)
{
	return bo && __atomic_load_n(&bo->shared, __ATOMIC_ACQUIRE);
}
