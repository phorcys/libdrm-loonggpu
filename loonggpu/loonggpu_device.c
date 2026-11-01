/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/kcmp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "loonggpu_internal.h"

/* Table format this parser understands; see data/loonggpu.ids. */
#define LOONGGPU_ASIC_ID_TABLE_VERSION_MAJOR 1

/*
 * The handle tables and the VA manager belong to a DRM open file description,
 * not to a numeric fd, so two fds of the same description must share one
 * loonggpu_device.  The registry below is the weak index used to find it:
 * every user of a device holds a reference, and lookup and removal of the last
 * reference share this mutex.
 */
static pthread_mutex_t device_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct loonggpu_device *device_registry;

enum file_match { FILE_DIFFERENT, FILE_SAME, FILE_UNKNOWN };

/*
 * Decide whether two fds are the same open file description.  F_DUPFD_QUERY
 * answers it with fcntl() on recent kernels; kcmp() answers it for the
 * rest.  FILE_UNKNOWN means neither could be asked, and callers must not guess:
 * treating two different descriptions as one would share a handle table across
 * unrelated files.
 */
static enum file_match
same_file_description(int a, int b)
{
	int r;

	if (a == b)
		return FILE_SAME;
#ifdef F_DUPFD_QUERY
	r = fcntl(a, F_DUPFD_QUERY, b);
	if (r >= 0)
		return r ? FILE_SAME : FILE_DIFFERENT;
#endif
#ifdef SYS_kcmp
	r = syscall(SYS_kcmp, getpid(), getpid(), KCMP_FILE, a, b);
	if (r >= 0)
		return r ? FILE_DIFFERENT : FILE_SAME;
#endif
	return FILE_UNKNOWN;
}

void loonggpu_device_ref_internal(struct loonggpu_device *dev)
{
	if (!dev || !dev->registered)
		return;
	pthread_mutex_lock(&device_registry_mutex);
	dev->refs++;
	pthread_mutex_unlock(&device_registry_mutex);
}

void loonggpu_device_unref_internal(struct loonggpu_device *dev)
{
	struct loonggpu_device **link;

	if (!dev || !dev->registered)
		return;
	pthread_mutex_lock(&device_registry_mutex);
	if (--dev->refs) {
		pthread_mutex_unlock(&device_registry_mutex);
		return;
	}
	for (link = &device_registry; *link; link = &(*link)->registry_next) {
		if (*link == dev) {
			*link = dev->registry_next;
			break;
		}
	}
	dev->registered = false;
	pthread_mutex_unlock(&device_registry_mutex);

	loonggpu_vamgr_deinit(&dev->va_mgr);
	free(dev->marketing_name);
	loonggpu_handle_table_fini(&dev->bo_handles);
	pthread_mutex_destroy(&dev->bo_table_mutex);
	close(dev->fd);
	free(dev);
}

int loonggpu_device_reference(loonggpu_device_handle dev,
			      loonggpu_device_handle *out)
{
	if (out)
		*out = NULL;
	if (!dev || !out || !dev->registered)
		return -EINVAL;
	loonggpu_device_ref_internal(dev);
	*out = dev;
	return 0;
}

/*
 * Turn a relative timeout into the absolute CLOCK_MONOTONIC deadline the
 * waiting ioctls take.  A relative timeout that would overflow saturates
 * instead of wrapping into the past.
 */
uint64_t loonggpu_timeout_to_deadline(uint64_t timeout)
{
	struct timespec ts;
	uint64_t now;

	if (timeout == LOONGGPU_TIMEOUT_INFINITE)
		return LOONGGPU_TIMEOUT_INFINITE;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
	if (now + timeout < now)
		return LOONGGPU_TIMEOUT_INFINITE;
	return now + timeout;
}

int loonggpu_device_initialize(int fd,
			      uint32_t *major_version,
			      uint32_t *minor_version,
			      loonggpu_device_handle *device_handle)
{
	struct loonggpu_device *dev;
	struct loonggpu_device *candidate;
	drmVersionPtr version;
	struct stat st;
	uint32_t accel = 0;
	int r, owned_fd;

	if (!device_handle)
		return -EINVAL;
	*device_handle = NULL;
	owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (owned_fd < 0)
		return -errno;
	if (fstat(owned_fd, &st)) {
		r = -errno;
		close(owned_fd);
		return r;
	}

	version = drmGetVersion(owned_fd);
	if (!version) {
		close(owned_fd);
		return -EIO;
	}

	if (!version->name || strcmp(version->name, "loonggpu")) {
		drmFreeVersion(version);
		close(owned_fd);
		return -ENODEV;
	}
	if (version->version_major != 1) {
		fprintf(stderr,
			"%s: DRM version is %d.%d.%d but this driver is only compatible with 1.x.x.\n",
			__func__, version->version_major, version->version_minor,
			version->version_patchlevel);
		drmFreeVersion(version);
		close(owned_fd);
		return -EBADFD;
	}

	pthread_mutex_lock(&device_registry_mutex);
	for (candidate = device_registry; candidate;
	     candidate = candidate->registry_next) {
		struct stat current;
		enum file_match match;

		if (fstat(candidate->fd, &current) || current.st_rdev != st.st_rdev)
			continue;
		match = same_file_description(owned_fd, candidate->fd);
		if (match == FILE_DIFFERENT)
			continue;
		if (match == FILE_UNKNOWN) {
			r = -EOPNOTSUPP;
			goto fail_registry;
		}
		candidate->refs++;
		if (major_version)
			*major_version = candidate->major_version;
		if (minor_version)
			*minor_version = candidate->minor_version;
		*device_handle = candidate;
		pthread_mutex_unlock(&device_registry_mutex);
		drmFreeVersion(version);
		close(owned_fd);
		return 0;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		r = -ENOMEM;
		goto fail_registry;
	}

	dev->fd = owned_fd;
	dev->major_version = version->version_major;
	dev->minor_version = version->version_minor;
	drmFreeVersion(version);
	version = NULL;
	pthread_mutex_init(&dev->bo_table_mutex, NULL);

	r = loonggpu_query_info(dev, LOONGGPU_INFO_ACCEL_WORKING, sizeof(accel),
				&accel);
	if (r) {
		fprintf(stderr, "%s: loonggpu_query_info(ACCEL_WORKING) failed (%i)\n",
			__func__, r);
		goto fail;
	}
	if (!accel) {
		fprintf(stderr, "%s: LOONGGPU_INFO_ACCEL_WORKING = 0\n", __func__);
		r = -EBADFD;
		goto fail;
	}

	r = loonggpu_query_gpu_info_init(dev);
	if (r) {
		fprintf(stderr, "%s: loonggpu_query_gpu_info_init failed\n", __func__);
		goto fail;
	}

	loonggpu_vamgr_init(&dev->va_mgr,
			    dev->dev_info.virtual_address_offset,
			    dev->dev_info.virtual_address_max,
			    dev->dev_info.virtual_address_alignment ?
				dev->dev_info.virtual_address_alignment : 4096);

	loonggpu_parse_asic_ids(dev);
	dev->refs = 1;
	dev->registered = true;
	dev->registry_next = device_registry;
	device_registry = dev;

	if (major_version)
		*major_version = dev->major_version;
	if (minor_version)
		*minor_version = dev->minor_version;
	*device_handle = dev;
	pthread_mutex_unlock(&device_registry_mutex);
	return 0;

fail:
	loonggpu_handle_table_fini(&dev->bo_handles);
	pthread_mutex_destroy(&dev->bo_table_mutex);
	free(dev);
fail_registry:
	pthread_mutex_unlock(&device_registry_mutex);
	if (version)
		drmFreeVersion(version);
	close(owned_fd);
	return r;
}

int loonggpu_device_deinitialize(loonggpu_device_handle dev)
{
	if (!dev)
		return -EINVAL;
	loonggpu_device_unref_internal(dev);
	return 0;
}

int loonggpu_device_get_fd(loonggpu_device_handle device)
{
	return device ? device->fd : -1;
}

const char *loonggpu_get_marketing_name(loonggpu_device_handle device)
{
	if (!device)
		return NULL;
	return device->marketing_name;
}

/*
 * Look the device up in the marketing name table and keep the result, falling
 * back to a generic name built from the PCI ID.  The table is data, not policy:
 * a missing or unreadable file is not an error.
 */
void loonggpu_parse_asic_ids(loonggpu_device_handle dev)
{
	const char *path;
	char line[256];
	FILE *f;

	path = NULL;
#if HAVE_SECURE_GETENV
	path = secure_getenv("LOONGGPU_IDS_PATH");
#endif
	if (!path)
		path = LOONGGPU_ASIC_ID_TABLE;

	f = fopen(path, "r");
	if (!f)
		goto fallback;

	while (fgets(line, sizeof(line), f)) {
		unsigned did = 0, rev = 0;
		unsigned major = 0, minor = 0, patch = 0;
		char name[128];
		char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		/* The first content line carries the table format version.  A
		 * format this parser does not understand ends the lookup rather
		 * than being misread as device entries. */
		if (sscanf(p, "%u.%u.%u", &major, &minor, &patch) == 3) {
			if (major != LOONGGPU_ASIC_ID_TABLE_VERSION_MAJOR)
				break;
			continue;
		}

		/*
		 * "<device_id>,\t<revision>,\t<product name>".  A space in the
		 * scanf format matches any run of whitespace, so tab and space
		 * separated files both parse.  The revision column is
		 * informational: the kernel reports no chip revision to match
		 * it against, so the device ID alone selects the entry, and the
		 * first match wins.
		 */
		if (sscanf(p, "%x , %x , %127[^\n]", &did, &rev, name) != 3)
			continue;
		if (did != dev->dev_info.device_id)
			continue;

		{
			size_t n = strlen(name);

			while (n && (name[n - 1] == '\n' || name[n - 1] == '\r' ||
				     name[n - 1] == ' ' || name[n - 1] == '\t'))
				name[--n] = 0;
		}
		free(dev->marketing_name);
		dev->marketing_name = strdup(name);
		break;
	}
	fclose(f);

fallback:
	if (!dev->marketing_name) {
		char tmp[64];

		snprintf(tmp, sizeof(tmp), "LOONGSON LoongGPU (0x%04x)",
			 dev->dev_info.device_id);
		dev->marketing_name = strdup(tmp);
	}
}
