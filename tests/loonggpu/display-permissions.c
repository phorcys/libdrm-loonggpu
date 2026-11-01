/* SPDX-License-Identifier: MIT */
/*
 * A render node must not reach the private display ioctls.
 *
 * LAYER_DISPLAY, LAYER_ZOOM, LAYER_TILE, LAYER_ROTATE, VIDEO_GAMMA, GET_META
 * and SET_META program layer and gamma registers or touch the global meta
 * buffer, so they are DRM_MASTER-only and must be refused for a client that
 * only holds a render node.
 *
 * The check is about the permission gate, not about the handlers: every call is
 * made with an invalid CRTC ID, so the request has to be refused before any
 * handler logic runs.  drm_ioctl_permit() answers -EACCES; -EPERM is
 * deliberately not accepted, because a handler that fails on its own would
 * report a different error and that is exactly what this test must catch.
 *
 * Skips (exit 77) unless LOONGGPU_TEST_DEVICE names the node to use.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>

#include <loonggpu_drm.h>

static unsigned failures;

static void report(const char *what, int rc, const char *detail)
{
	if (rc >= 0) {
		fprintf(stderr, "FAIL: %s was allowed (%s)\n", what, detail);
		failures++;
		return;
	}
	if (errno != EACCES) {
		fprintf(stderr, "FAIL: %s failed with %s, expected EACCES (%s)\n",
			what, strerror(errno), detail);
		failures++;
		return;
	}
	printf("PASS %s is refused with EACCES\n", what);
}

int main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	struct drm_loonggpu_layer_display layer;
	struct drm_loonggpu_layer_zoom zoom;
	struct drm_loonggpu_layer_tile tile;
	struct drm_loonggpu_layer_rotate rotate;
	struct drm_loonggpu_video_gamma gamma;
	int fd;
	drmVersionPtr version;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping display permission check");
		return 77;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (drmGetNodeTypeFromFd(fd) != DRM_NODE_RENDER) {
		puts("display permission checks require a render node");
		close(fd);
		return 77;
	}
	version = drmGetVersion(fd);
	if (!version || !version->name || strcmp(version->name, "loonggpu")) {
		drmFreeVersion(version);
		close(fd);
		fprintf(stderr, "not a LoongGPU DRM device\n");
		return 1;
	}
	drmFreeVersion(version);

	memset(&layer, 0, sizeof(layer));
	layer.layer_num = 0;
	layer.l_width = 16;
	layer.l_height = 16;
	errno = 0;
	report("LAYER_DISPLAY", ioctl(fd, DRM_IOCTL_LOONGGPU_LAYER_DISPLAY, &layer),
	       "request with an invalid CRTC ID");

	memset(&zoom, 0, sizeof(zoom));
	errno = 0;
	report("LAYER_ZOOM", ioctl(fd, DRM_IOCTL_LOONGGPU_LAYER_ZOOM, &zoom),
	       "zeroed request");

	memset(&tile, 0, sizeof(tile));
	errno = 0;
	report("LAYER_TILE", ioctl(fd, DRM_IOCTL_LOONGGPU_LAYER_TILE, &tile),
	       "zeroed request");

	memset(&rotate, 0, sizeof(rotate));
	errno = 0;
	report("LAYER_ROTATE", ioctl(fd, DRM_IOCTL_LOONGGPU_LAYER_ROTATE, &rotate),
	       "zeroed request");

	memset(&gamma, 0, sizeof(gamma));
	errno = 0;
	report("VIDEO_GAMMA", ioctl(fd, DRM_IOCTL_LOONGGPU_VIDEO_GAMMA, &gamma),
	       "zeroed request");

	/* These two take no argument, so the gate is the only thing between the
	 * caller and the global meta buffer. */
	errno = 0;
	report("GET_META", ioctl(fd, DRM_IOCTL_LOONGGPU_GET_META, NULL),
	       "argument-less call");

	errno = 0;
	report("SET_META", ioctl(fd, DRM_IOCTL_LOONGGPU_SET_META, NULL),
	       "argument-less call");

	close(fd);

	if (failures) {
		fprintf(stderr, "%u display permission checks failed\n", failures);
		return 1;
	}
	puts("display permission checks passed");
	return 0;
}
