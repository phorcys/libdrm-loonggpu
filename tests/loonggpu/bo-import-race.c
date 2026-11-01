/* SPDX-License-Identifier: MIT */
/*
 * The BO table must stay consistent when a PRIME import races the final
 * reference to the same object.  All DRM calls are interposed and the import
 * ioctl is gated, so the interleaving is deterministic and no device is
 * opened.
 */

#include "loonggpu_internal.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool prime_entered, free_entered, release_prime;
static int gem_closes;

/* Exported by the executable to interpose the DRM entry points. */
int drmCommandWriteRead(int fd, unsigned long cmd, void *data,
			unsigned long size)
{
	(void)fd;
	(void)size;
	if (cmd != DRM_LOONGGPU_GEM_CREATE)
		abort();
	((union drm_loonggpu_gem_create *)data)->out.handle = 7;
	return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	(void)prime_fd;
	pthread_mutex_lock(&gate_lock);
	prime_entered = true;
	pthread_cond_broadcast(&gate_cv);
	while (!release_prime)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
	/* The PRIME import returned a handle this file already owns. */
	*handle = 7;
	return 0;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	assert(request == DRM_IOCTL_GEM_CLOSE);
	assert(((struct drm_gem_close *)arg)->handle == 7);
	__atomic_add_fetch(&gem_closes, 1, __ATOMIC_RELAXED);
	return 0;
}

struct thread_args {
	struct loonggpu_device *dev;
	loonggpu_bo_handle original;
	int prime_fd;
	struct loonggpu_bo_import_result imported;
	int import_ret, free_ret;
};

static void *import_thread(void *opaque)
{
	struct thread_args *args = opaque;

	args->import_ret = loonggpu_bo_import(args->dev,
					     loonggpu_bo_handle_type_dma_buf_fd,
					     (uint32_t)args->prime_fd,
					     &args->imported);
	return NULL;
}

static void *free_thread(void *opaque)
{
	struct thread_args *args = opaque;

	pthread_mutex_lock(&gate_lock);
	free_entered = true;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&gate_lock);
	args->free_ret = loonggpu_bo_free(args->original);
	return NULL;
}

int main(void)
{
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = 4096,
		.phys_alignment = 4096,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
	};
	struct loonggpu_device dev = {0};
	struct loonggpu_bo_import_result repeated = {0};
	struct loonggpu_bo_import_result borrowed = {0}, same_borrowed = {0};
	struct thread_args args;
	loonggpu_bo_handle original = NULL;
	pthread_t importer, freer;
	int prime_fd;
	FILE *prime_file;

	pthread_mutex_init(&dev.bo_table_mutex, NULL);
	assert(loonggpu_bo_alloc(&dev, &request, &original) == 0);
	assert(original);
	assert(!loonggpu_bo_is_shared(original));

	prime_file = tmpfile();
	assert(prime_file);
	prime_fd = fileno(prime_file);
	assert(ftruncate(prime_fd, 65536) == 0);
	memset(&args, 0, sizeof(args));
	args.dev = &dev;
	args.original = original;
	args.prime_fd = prime_fd;
	assert(pthread_create(&importer, NULL, import_thread, &args) == 0);

	pthread_mutex_lock(&gate_lock);
	while (!prime_entered)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);

	/* The final free starts while the import ioctl holds the table mutex.  It
	 * may finish only after that import has acquired the existing wrapper. */
	assert(pthread_create(&freer, NULL, free_thread, &args) == 0);
	pthread_mutex_lock(&gate_lock);
	while (!free_entered)
		pthread_cond_wait(&gate_cv, &gate_lock);
	assert(__atomic_load_n(&gem_closes, __ATOMIC_RELAXED) == 0);
	release_prime = true;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&gate_lock);

	assert(pthread_join(importer, NULL) == 0);
	assert(pthread_join(freer, NULL) == 0);
	assert(args.import_ret == 0 && args.free_ret == 0);
	/* The import found the wrapper the free was about to retire, so the
	 * object is still alive and the handle was never closed. */
	assert(args.imported.buf_handle == original);
	assert(loonggpu_bo_is_shared(original));
	assert(__atomic_load_n(&gem_closes, __ATOMIC_RELAXED) == 0);

	assert(loonggpu_bo_import(&dev, loonggpu_bo_handle_type_dma_buf_fd,
				  (uint32_t)prime_fd, &repeated) == 0);
	assert(repeated.buf_handle == original);
	assert(loonggpu_bo_free(repeated.buf_handle) == 0);
	assert(__atomic_load_n(&gem_closes, __ATOMIC_RELAXED) == 0);
	assert(loonggpu_bo_free(args.imported.buf_handle) == 0);
	assert(__atomic_load_n(&gem_closes, __ATOMIC_RELAXED) == 1);

	/* A borrowed handle and an imported one for the same GEM handle share
	 * one wrapper, and only the owner closes it. */
	assert(loonggpu_bo_import(&dev, loonggpu_bo_handle_type_kms, 7,
				  &borrowed) == 0);
	assert(loonggpu_bo_import(&dev, loonggpu_bo_handle_type_dma_buf_fd,
				  (uint32_t)prime_fd, &same_borrowed) == 0);
	assert(borrowed.buf_handle == same_borrowed.buf_handle);
	assert(same_borrowed.alloc_size == 65536);
	assert(same_borrowed.buf_handle->alloc_size == 65536);
	assert(loonggpu_bo_free(borrowed.buf_handle) == 0);
	assert(loonggpu_bo_free(same_borrowed.buf_handle) == 0);
	assert(__atomic_load_n(&gem_closes, __ATOMIC_RELAXED) == 1);

	fclose(prime_file);
	loonggpu_handle_table_fini(&dev.bo_handles);
	pthread_mutex_destroy(&dev.bo_table_mutex);
	puts("BO import against final close interleaving passed");
	return 0;
}
