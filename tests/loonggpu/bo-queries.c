/* SPDX-License-Identifier: MIT */
/* BO query errors, userptr registration and heap selection without a device. */

#include "loonggpu_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <xf86drm.h>

static int metadata_error, create_info_error, userptr_error;
static unsigned metadata_size, userptr_calls, closes, info_calls;

int drmCommandWriteRead(int fd, unsigned long command, void *data,
			unsigned long size)
{
	assert(fd == 17);
	switch (command) {
	case DRM_LOONGGPU_GEM_METADATA: {
		struct drm_loonggpu_gem_metadata *args = data;

		assert(size == sizeof(*args));
		assert(args->op == LOONGGPU_GEM_METADATA_OP_GET_METADATA);
		args->data.data_size_bytes = metadata_size;
		args->data.data[0] = 0x12345678;
		return metadata_error;
	}
	case DRM_LOONGGPU_GEM_OP: {
		struct drm_loonggpu_gem_op *args = data;
		struct drm_loonggpu_gem_create_in *out = (void *)(uintptr_t)args->value;

		assert(size == sizeof(*args));
		assert(args->op == LOONGGPU_GEM_OP_GET_GEM_CREATE_INFO);
		out->bo_size = 65536;
		out->alignment = 4096;
		out->domains = LOONGGPU_GEM_DOMAIN_GTT;
		return create_info_error;
	}
	case DRM_LOONGGPU_GEM_USERPTR: {
		struct drm_loonggpu_gem_userptr *args = data;

		assert(size == sizeof(*args));
		assert(args->addr == 0x10000 && args->size == 4096);
		userptr_calls++;
		if (!(args->flags & LOONGGPU_GEM_USERPTR_REGISTER))
			return -EACCES;
		assert(args->flags & LOONGGPU_GEM_USERPTR_VALIDATE);
		if (userptr_error)
			return userptr_error;
		args->handle = 8;
		return 0;
	}
	case DRM_LOONGGPU_INFO: {
		struct drm_loonggpu_info *args = data;
		struct drm_loonggpu_memory_info *out = (void *)(uintptr_t)args->return_pointer;

		assert(size == sizeof(*args));
		assert(args->query == LOONGGPU_INFO_MEMORY);
		assert(args->return_size == sizeof(*out));
		info_calls++;
		out->vram.total_heap_size = 100;
		out->cpu_accessible_vram.total_heap_size = 25;
		out->gtt.total_heap_size = 200;
		return 0;
	}
	default:
		assert(0);
		return -EINVAL;
	}
}

int drmIoctl(int fd, unsigned long request, void *data)
{
	assert(fd == 17 && request == DRM_IOCTL_GEM_CLOSE);
	assert(((struct drm_gem_close *)data)->handle == 8);
	closes++;
	return 0;
}

int main(void)
{
	struct loonggpu_device dev = { .fd = 17 };
	struct loonggpu_bo bo = { .dev = &dev, .handle = 7 };
	struct loonggpu_bo_info info;
	struct loonggpu_heap_info heap;
	loonggpu_bo_handle userptr = NULL;
	uint32_t value;

	pthread_mutex_init(&dev.bo_table_mutex, NULL);
	pthread_mutex_init(&bo.cpu_access_mutex, NULL);
	metadata_error = -EIO;
	assert(loonggpu_bo_query_info(&bo, &info) == -EIO);
	metadata_error = 0;
	metadata_size = sizeof(info.metadata.umd_metadata) + 1;
	assert(loonggpu_bo_query_info(&bo, &info) == -EOVERFLOW);
	metadata_size = sizeof(uint32_t);
	create_info_error = -ENOENT;
	assert(loonggpu_bo_query_info(&bo, &info) == -ENOENT);
	create_info_error = 0;
	assert(loonggpu_bo_query_info(&bo, &info) == 0);
	assert(info.alloc_size == 65536 && info.phys_alignment == 4096);
	assert(info.metadata.size_metadata == 4 &&
	       info.metadata.umd_metadata[0] == 0x12345678);

	assert(loonggpu_create_bo_from_user_mem(&dev, (void *)0x10000,
					       4096, &userptr) == 0);
	assert(userptr && userptr_calls == 1);
	assert(loonggpu_bo_free(userptr) == 0 && closes == 1);
	userptr = (void *)1;
	userptr_error = -ENOMEM;
	assert(loonggpu_create_bo_from_user_mem(&dev, (void *)0x10000,
					       4096, &userptr) == -ENOMEM);
	assert(!userptr && closes == 1);

	assert(loonggpu_query_heap_info(&dev, LOONGGPU_GEM_DOMAIN_VRAM, 0, &heap) == 0);
	assert(heap.total_heap_size == 100);
	assert(loonggpu_query_heap_info(&dev, LOONGGPU_GEM_DOMAIN_VRAM,
					LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &heap) == 0);
	assert(heap.total_heap_size == 25);
	assert(loonggpu_query_heap_info(&dev, LOONGGPU_GEM_DOMAIN_GTT, 0, &heap) == 0);
	assert(heap.total_heap_size == 200);
	assert(loonggpu_query_heap_info(&dev, LOONGGPU_GEM_DOMAIN_GDS, 0, &heap) == -EINVAL);
	assert(loonggpu_query_heap_info(&dev, LOONGGPU_GEM_DOMAIN_VRAM, UINT32_MAX, &heap) == -EINVAL);
	assert(loonggpu_read_mm_registers(&dev, 0, UINT32_MAX, 0, 0, &value) == -EINVAL);
	assert(info_calls == 3);

	loonggpu_handle_table_fini(&dev.bo_handles);
	pthread_mutex_destroy(&bo.cpu_access_mutex);
	pthread_mutex_destroy(&dev.bo_table_mutex);
	puts("BO queries, userptr registration and heap selection passed");
	return 0;
}
