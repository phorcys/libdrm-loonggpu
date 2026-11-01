/* SPDX-License-Identifier: MIT */
/*
 * Walk over the public API against a real device: queries, BO allocation, CPU
 * mapping, export and import, VA ranges, BO lists, contexts, sync objects and
 * semaphores.  It does not submit command buffers.
 *
 * Skips (exit 77) unless LOONGGPU_TEST_DEVICE names the node to use.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loonggpu.h"
#include "loonggpu_drm.h"

static unsigned failures;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

static void check_queries(loonggpu_device_handle dev)
{
	struct loonggpu_gpu_info gpu = {0};
	struct loonggpu_heap_info heap = {0};
	struct loonggpu_gds_resource_info gds;
	struct loonggpu_buffer_size_alignments align = {0};
	uint32_t accel = 0, sw_hi = 0, fw_version = 0, fw_feature = 0;
	uint64_t timestamp = 0;
	unsigned type;
	int r;

	CHECK(loonggpu_query_info(dev, LOONGGPU_INFO_ACCEL_WORKING, sizeof(accel),
				  &accel) == 0,
	      "LOONGGPU_INFO_ACCEL_WORKING is answered");
	CHECK(accel == 1, "acceleration is reported as working");

	CHECK(loonggpu_query_gpu_info(dev, &gpu) == 0, "gpu info is answered");
	CHECK(gpu.asic_id != 0, "the device reports a PCI id");
	CHECK(gpu.virtual_address_max > gpu.virtual_address_offset,
	      "the device reports a non empty VA range");
	printf("asic=0x%04x family=%u engines=%u arrays=%u cu/sh=%u wave=%u\n",
	       gpu.asic_id, gpu.family_id, gpu.num_shader_engines,
	       gpu.num_shader_arrays_per_engine, gpu.num_cu_per_sh,
	       gpu.wave_front_size);

	CHECK(loonggpu_query_heap_info(dev, LOONGGPU_GEM_DOMAIN_VRAM, 0, &heap) == 0,
	      "the VRAM heap is described");
	CHECK(heap.total_heap_size > 0, "VRAM is reported as non empty");
	printf("VRAM total=%" PRIu64 " usage=%" PRIu64 "\n",
	       heap.total_heap_size, heap.heap_usage);

	CHECK(loonggpu_query_heap_info(dev, LOONGGPU_GEM_DOMAIN_GTT, 0, &heap) == 0,
	      "the GTT heap is described");
	CHECK(heap.total_heap_size > 0, "GTT is reported as non empty");

	CHECK(loonggpu_query_buffer_size_alignment(dev, &align) == 0,
	      "size alignments are answered");
	CHECK(align.size_local >= 4096, "the local size alignment is a page at least");

	CHECK(loonggpu_query_sw_info(dev, loonggpu_sw_info_address32_hi, &sw_hi) == 0,
	      "the address32 high word is answered");
	CHECK(loonggpu_query_info(dev, LOONGGPU_INFO_TIMESTAMP, sizeof(timestamp),
				  &timestamp) == 0,
	      "the GPU timestamp is answered");
	printf("address32_hi=0x%x timestamp=%" PRIu64 "\n", sw_hi, timestamp);

	CHECK(loonggpu_query_firmware_version(dev, LOONGGPU_INFO_FW_GFX_ME, 0, 0,
					      &fw_version, &fw_feature) == 0,
	      "the GFX ME firmware version is answered");

	/* A device without a GDS block answers -ENODATA rather than zeroes. */
	r = loonggpu_query_gds_info(dev, &gds);
	if (r == -ENODATA)
		printf("note: the device reports no GDS configuration\n");
	else
		CHECK(r == 0, "the GDS configuration is answered");

	for (type = 0; type < LOONGGPU_HW_IP_NUM; type++) {
		struct drm_loonggpu_info_hw_ip info;
		bool available = false;
		uint32_t count = 0;

		r = loonggpu_query_hw_ip_available(dev, type, &available);
		CHECK(r == 0, "engine availability query succeeds");
		if (r || !available)
			continue;
		r = loonggpu_query_hw_ip_count(dev, type, &count);
		CHECK(r == 0 && count, "an available engine has an instance");
		if (r || !count)
			continue;
		memset(&info, 0, sizeof(info));
		CHECK(loonggpu_query_hw_ip_info(dev, type, 0, &info) == 0,
		      "an available engine reports its info");
		printf("IP %u count=%u version=%u.%u rings=0x%x ib_align=%u/%u\n",
		       type, count, info.hw_ip_version_major,
		       info.hw_ip_version_minor, info.available_rings,
		       info.ib_start_alignment, info.ib_size_alignment);
	}
}

static void check_bo(loonggpu_device_handle dev, uint64_t alignment)
{
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = 256 * 1024,
		.phys_alignment = alignment,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
		.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
	};
	struct loonggpu_bo_import_result imported = {0};
	struct loonggpu_bo_metadata metadata = {0};
	struct loonggpu_bo_info info;
	loonggpu_bo_handle bo = NULL;
	uint32_t dmabuf_fd = 0, kms_handle = 0;
	void *map = NULL, *map2 = NULL;
	uint32_t *words;
	uint32_t i;
	bool busy = true;

	if (loonggpu_bo_alloc(dev, &request, &bo) || !bo) {
		CHECK(0, "a GTT BO with CPU access is allocated");
		return;
	}
	CHECK(1, "a GTT BO with CPU access is allocated");

	if (loonggpu_bo_cpu_map(bo, &map) || !map) {
		CHECK(0, "the BO can be mapped for the CPU");
		loonggpu_bo_free(bo);
		return;
	}
	CHECK(loonggpu_bo_cpu_map(bo, &map2) == 0 && map2 == map,
	      "a second map returns the same address");

	words = map;
	for (i = 0; i < request.alloc_size / sizeof(*words); i++)
		words[i] = 0xa5000000u ^ i;
	CHECK(words[1] == (0xa5000000u ^ 1), "the mapping round trips its stores");

	CHECK(loonggpu_bo_cpu_unmap(bo) == 0, "the first unmap succeeds");
	CHECK(loonggpu_bo_cpu_unmap(bo) == 0, "the second unmap succeeds");
	CHECK(loonggpu_bo_cpu_unmap(bo) == -EINVAL,
	      "an unmap without a mapping is refused");

	memset(&info, 0, sizeof(info));
	CHECK(loonggpu_bo_query_info(bo, &info) == 0, "the BO info is answered");
	CHECK(info.alloc_size == request.alloc_size,
	      "the reported allocation size matches the request");

	metadata.tiling_info = 0x11;
	metadata.size_metadata = 2 * sizeof(uint32_t);
	metadata.umd_metadata[0] = 0x4c473230;
	metadata.umd_metadata[1] = 0x54455354;
	CHECK(loonggpu_bo_set_metadata(bo, &metadata) == 0,
	      "the BO metadata is accepted");
	/* The UAPI carries 256 bytes of metadata. */
	metadata.size_metadata = sizeof(metadata.umd_metadata) + 1;
	CHECK(loonggpu_bo_set_metadata(bo, &metadata) == -EINVAL,
	      "metadata larger than the UAPI field is refused");

	CHECK(loonggpu_bo_wait_for_idle(bo, 1000000000ull, &busy) == 0,
	      "the BO wait is answered");
	CHECK(!busy, "a BO without work is idle");

	if (loonggpu_bo_export(bo, loonggpu_bo_handle_type_dma_buf_fd, &dmabuf_fd)) {
		CHECK(0, "the BO can be exported as a dma-buf");
	} else {
		CHECK(loonggpu_bo_import(dev, loonggpu_bo_handle_type_dma_buf_fd,
					 dmabuf_fd, &imported) == 0,
		      "the dma-buf can be imported back");
		CHECK(imported.alloc_size >= request.alloc_size,
		      "the imported BO is at least as large as the export");
		close((int)dmabuf_fd);
		if (imported.buf_handle)
			loonggpu_bo_free(imported.buf_handle);
	}

	CHECK(loonggpu_bo_export(bo, loonggpu_bo_handle_type_kms,
				 &kms_handle) == 0 && kms_handle != 0,
	      "the BO can be exported as a GEM handle");
	CHECK(loonggpu_bo_is_shared(bo), "an exported BO is marked as shared");

	loonggpu_bo_free(bo);
}

static void check_va(loonggpu_device_handle dev, uint64_t alignment)
{
	struct loonggpu_bo_alloc_request request = {
		.alloc_size = 64 * 1024,
		.phys_alignment = alignment,
		.preferred_heap = LOONGGPU_GEM_DOMAIN_GTT,
		.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
	};
	loonggpu_bo_handle bo = NULL, bo2 = NULL;
	loonggpu_bo_handle resources[2];
	loonggpu_va_handle va = NULL, va2 = NULL;
	loonggpu_bo_list_handle list = NULL;
	uint8_t prios[2] = { 0, 1 };
	uint64_t addr = 0, addr2 = 0, addr3 = 0;
	uint64_t start = 0, end = 0;

	CHECK(loonggpu_va_range_query(dev, loonggpu_gpu_va_range_general, &start,
				      &end) == 0,
	      "the VA range is answered");
	CHECK(end > start, "the VA range is not empty");

	if (loonggpu_bo_alloc(dev, &request, &bo) ||
	    loonggpu_bo_alloc(dev, &request, &bo2)) {
		CHECK(0, "two BOs are allocated for the VA test");
		goto out;
	}
	if (loonggpu_va_range_alloc(dev, loonggpu_gpu_va_range_general,
				    request.alloc_size, alignment, 0, &addr, &va, 0) ||
	    loonggpu_va_range_alloc(dev, loonggpu_gpu_va_range_general,
				    request.alloc_size, alignment, 0, &addr2, &va2, 0)) {
		CHECK(0, "two VA ranges are allocated");
		goto out;
	}
	CHECK(addr != addr2, "the two VA ranges differ");

	CHECK(loonggpu_bo_va_op(bo, 0, request.alloc_size, addr, 0,
				LOONGGPU_VA_OP_MAP) == 0,
	      "the first BO is mapped");
	CHECK(loonggpu_bo_va_op(bo2, 0, request.alloc_size, addr2, 0,
				LOONGGPU_VA_OP_MAP) == 0,
	      "the second BO is mapped");

	resources[0] = bo;
	resources[1] = bo2;
	CHECK(loonggpu_bo_list_create(dev, 2, resources, prios, &list) == 0,
	      "a BO list with two entries is created");
	CHECK(loonggpu_bo_list_update(list, 1, &bo, NULL) == 0,
	      "the BO list can be shrunk");
	CHECK(loonggpu_bo_list_update(list, 2, resources, prios) == 0,
	      "the BO list can be grown again");

	CHECK(loonggpu_bo_va_op(bo, 0, request.alloc_size, addr, 0,
				LOONGGPU_VA_OP_UNMAP) == 0,
	      "the first BO is unmapped");
	CHECK(loonggpu_bo_va_op(bo2, 0, request.alloc_size, addr2, 0,
				LOONGGPU_VA_OP_UNMAP) == 0,
	      "the second BO is unmapped");

	/* A freed range is reusable, and the manager prefers the hole. */
	CHECK(loonggpu_va_range_free(va) == 0, "the first VA range is freed");
	va = NULL;
	CHECK(loonggpu_va_range_alloc(dev, loonggpu_gpu_va_range_general,
				      request.alloc_size, alignment, 0, &addr3,
				      &va, 0) == 0,
	      "a VA range is allocated again");
	CHECK(addr3 == addr, "the freed range is reused");

out:
	if (va)
		loonggpu_va_range_free(va);
	if (va2)
		loonggpu_va_range_free(va2);
	if (list)
		loonggpu_bo_list_destroy(list);
	if (bo2)
		loonggpu_bo_free(bo2);
	if (bo)
		loonggpu_bo_free(bo);
}

static void check_context_and_sync(loonggpu_device_handle dev)
{
	loonggpu_context_handle context = NULL, context2 = NULL;
	loonggpu_semaphore_handle sem = NULL;
	uint32_t state = 0, hangs = 0, flags = 0;
	uint32_t obj = 0, obj2 = 0, imported = 0;
	uint64_t point = 1, queried = 0;
	int fd = -1, sync_file_fd = -1;

	CHECK(loonggpu_cs_ctx_create(dev, &context) == 0, "a context is created");
	CHECK(loonggpu_cs_ctx_create2(dev, 0, &context2) == 0,
	      "a second context with an explicit priority is created");
	CHECK(loonggpu_cs_query_reset_state(context, &state, &hangs) == 0,
	      "the reset state is answered");
	CHECK(state == LOONGGPU_CTX_NO_RESET && hangs == 0,
	      "a fresh context reports no reset");
	CHECK(loonggpu_cs_query_reset_state2(context, &flags, &hangs) == 0,
	      "the sticky state flags are answered");

	CHECK(loonggpu_cs_create_syncobj(dev, &obj) == 0, "a sync object is created");
	CHECK(loonggpu_cs_create_syncobj2(dev, 0, &obj2) == 0,
	      "a second sync object is created");
	CHECK(loonggpu_cs_syncobj_signal(dev, &obj, 1) == 0, "it can be signalled");
	CHECK(loonggpu_cs_syncobj_wait(dev, &obj, 1, 0, 0, NULL) == 0,
	      "a signalled sync object does not block");
	CHECK(loonggpu_cs_syncobj_reset(dev, &obj, 1) == 0, "it can be reset");
	CHECK(loonggpu_cs_syncobj_timeline_signal(dev, &obj2, &point, 1) == 0,
	      "a timeline point can be signalled");
	CHECK(loonggpu_cs_syncobj_timeline_wait(dev, &obj2, &point, 1, 0, 0,
						NULL) == 0,
	      "the timeline point does not block");
	CHECK(loonggpu_cs_syncobj_query(dev, &obj2, &queried, 1) == 0,
	      "the timeline point can be queried");
	CHECK(queried == point, "the queried point is the signalled one");

	CHECK(loonggpu_cs_export_syncobj(dev, obj, &fd) == 0 && fd >= 0,
	      "the sync object can be exported as an fd");
	if (fd >= 0) {
		CHECK(loonggpu_cs_import_syncobj(dev, fd, &imported) == 0,
		      "the fd can be imported back");
		close(fd);
	}
	CHECK(loonggpu_cs_syncobj_export_sync_file(dev, obj2, &sync_file_fd) == 0,
	      "the sync object can be exported as a sync file");
	if (sync_file_fd >= 0)
		close(sync_file_fd);

	CHECK(loonggpu_cs_create_semaphore(&sem) == 0, "a semaphore is created");
	CHECK(loonggpu_cs_signal_semaphore(context, LOONGGPU_HW_IP_GFX, 0, 0,
					   sem) == 0,
	      "the semaphore records the last sequence of the context");
	CHECK(loonggpu_cs_wait_semaphore(context, LOONGGPU_HW_IP_GFX, 0, 0,
					 sem) == 0,
	      "waiting on the semaphore returns");
	CHECK(loonggpu_cs_signal_semaphore(context, LOONGGPU_HW_IP_NUM, 0, 0,
					   sem) == -EINVAL,
	      "an unknown engine type is refused");
	CHECK(loonggpu_cs_destroy_semaphore(sem) == 0, "the semaphore is destroyed");

	loonggpu_cs_destroy_syncobj(dev, obj);
	loonggpu_cs_destroy_syncobj(dev, obj2);
	if (imported)
		loonggpu_cs_destroy_syncobj(dev, imported);
	if (context2)
		loonggpu_cs_ctx_free(context2);
	if (context)
		loonggpu_cs_ctx_free(context);
}

static void check_vmid(loonggpu_device_handle dev)
{
	int r = loonggpu_vm_reserve_vmid(dev, 0);

	if (r) {
		printf("note: reserving a VMID returned %d; skipping the pair\n", r);
		return;
	}
	CHECK(loonggpu_vm_unreserve_vmid(dev, 0) == 0, "a reserved VMID is released");
}

int main(void)
{
	const char *path = getenv("LOONGGPU_TEST_DEVICE");
	struct loonggpu_buffer_size_alignments align = { .size_local = 4096 };
	loonggpu_device_handle dev = NULL;
	uint32_t major = 0, minor = 0;
	int fd;

	if (!path || !path[0]) {
		puts("LOONGGPU_TEST_DEVICE is unset; skipping the smoke test");
		return 77;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (loonggpu_device_initialize(fd, &major, &minor, &dev) || !dev) {
		fprintf(stderr, "device init failed\n");
		close(fd);
		return 1;
	}
	CHECK(major == 1, "the driver reports a compatible DRM version");
	printf("%s on DRM %u.%u\n",
	       loonggpu_get_marketing_name(dev) ? loonggpu_get_marketing_name(dev) : "?",
	       major, minor);

	if (loonggpu_query_buffer_size_alignment(dev, &align) || !align.size_local)
		align.size_local = 4096;

	check_queries(dev);
	check_bo(dev, align.size_local);
	check_va(dev, align.size_local);
	check_context_and_sync(dev);
	check_vmid(dev);

	loonggpu_device_deinitialize(dev);
	close(fd);

	if (failures) {
		fprintf(stderr, "%u smoke checks failed\n", failures);
		return 1;
	}
	puts("smoke test passed");
	return 0;
}
