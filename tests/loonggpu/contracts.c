/* SPDX-License-Identifier: MIT */
/*
 * Device independent checks of the value helpers and of the argument
 * validation every entry point performs.  Nothing here opens a DRM node.
 */

#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "handle_table.h"
#include "loonggpu_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition, message)                                              \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "FAIL: %s\n", message);                \
			failures++;                                            \
		}                                                              \
	} while (0)

static void check_handle_table(void)
{
	struct loonggpu_handle_table table = {0};
	int first = 1, second = 2;

	CHECK(loonggpu_handle_table_insert(&table, 7, &first) == 0,
	      "handle table insert succeeds");
	CHECK(loonggpu_handle_table_insert(&table, 70, &second) == 0,
	      "handle table grows to cover a higher key");
	CHECK(loonggpu_handle_table_lookup(&table, 7) == &first,
	      "handle table lookup returns inserted value");
	CHECK(loonggpu_handle_table_lookup(&table, 70) == &second,
	      "handle table preserves entries across growth");
	CHECK(loonggpu_handle_table_insert(&table, 7, &second) == -EEXIST,
	      "handle table rejects duplicate key");
	CHECK(loonggpu_handle_table_insert(&table, UINT32_MAX, &second) == 0,
	      "a huge GEM handle uses the bounded sparse storage");
	CHECK(loonggpu_handle_table_lookup(&table, UINT32_MAX) == &second,
	      "sparse handle lookup returns inserted value");
	CHECK(loonggpu_handle_table_insert(&table, UINT32_MAX, &first) == -EEXIST,
	      "sparse handle table rejects duplicate key");
	CHECK(loonggpu_handle_table_lookup(&table, 7) == &first,
	      "sparse insert preserves dense entries");
	loonggpu_handle_table_remove(&table, 7);
	CHECK(loonggpu_handle_table_lookup(&table, 7) == NULL,
	      "handle table remove clears the key");
	loonggpu_handle_table_remove(&table, 70);
	loonggpu_handle_table_remove(&table, UINT32_MAX);
	CHECK(loonggpu_handle_table_lookup(&table, UINT32_MAX) == NULL,
	      "sparse handle remove clears the key");
	loonggpu_handle_table_fini(&table);
}

/*
 * The fence chunk helpers cross a unit boundary: struct loonggpu_cs_fence_info
 * keeps its offset in units of uint64_t, the chunk carries bytes.
 */
static void check_fence_helpers(void)
{
	struct loonggpu_bo fake_bo = {0};
	struct loonggpu_cs_fence_info info = {0};
	struct loonggpu_cs_fence fence = {0};
	struct loonggpu_context fake_ctx = {0};
	struct drm_loonggpu_cs_chunk_dep dep;
	struct drm_loonggpu_cs_chunk_data data;

	fake_bo.handle = 0x42;
	info.handle = &fake_bo;
	info.offset = 3;
	memset(&data, 0xa5, sizeof(data));
	loonggpu_cs_chunk_fence_info_to_data(&info, &data);
	CHECK(data.fence_data.handle == 0x42,
	      "fence info helper copies the BO handle");
	CHECK(data.fence_data.offset == 3 * sizeof(uint64_t),
	      "fence info helper converts the offset to bytes");

	memset(&data, 0xa5, sizeof(data));
	loonggpu_cs_chunk_fence_info_to_data(NULL, &data);
	CHECK(data.fence_data.handle == 0 && data.fence_data.offset == 0,
	      "fence info helper zeroes the chunk on NULL input");

	info.offset = (UINT64_MAX / sizeof(uint64_t)) + 1;
	loonggpu_cs_chunk_fence_info_to_data(&info, &data);
	CHECK(!data.fence_data.handle && !data.fence_data.offset,
	      "an overflowing fence offset cannot wrap to a valid chunk");

	fake_ctx.id = 9;
	fence.context = &fake_ctx;
	fence.ip_type = LOONGGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 1;
	fence.fence = 77;
	memset(&dep, 0xa5, sizeof(dep));
	loonggpu_cs_chunk_fence_to_dep(&fence, &dep);
	CHECK(dep.ctx_id == 9 && dep.ip_type == LOONGGPU_HW_IP_COMPUTE &&
	      dep.ring == 1 && dep.handle == 77,
	      "fence to dependency helper copies the fence");
}

static void check_argument_validation(void)
{
	loonggpu_context_handle context = (loonggpu_context_handle)(uintptr_t)1;
	loonggpu_bo_list_handle list = (loonggpu_bo_list_handle)(uintptr_t)1;
	struct drm_loonggpu_ctx_state3 state3 = {
		.reset_epoch = UINT64_MAX,
		.admission_epoch = UINT64_MAX,
		.flags = UINT32_MAX,
	};
	uint32_t state = UINT32_MAX, hangs = UINT32_MAX;
	bool available = true;
	int ret;

	ret = loonggpu_cs_ctx_create(NULL, &context);
	CHECK(ret == -EINVAL && context == NULL,
	      "context create clears its output on a NULL device");

	ret = loonggpu_bo_list_create(NULL, 0, NULL, NULL, &list);
	CHECK(ret == -EINVAL && list == NULL,
	      "BO list create clears its output on a NULL device");

	ret = loonggpu_cs_query_reset_state(NULL, &state, &hangs);
	CHECK(ret == -EINVAL && state == 0 && hangs == 0,
	      "reset query clears its outputs on a NULL context");

	ret = loonggpu_cs_query_state3(NULL, &state3);
	CHECK(ret == -EINVAL && state3.reset_epoch == 0 &&
	      state3.admission_epoch == 0 && state3.flags == 0,
	      "typed state query clears its output on a NULL context");

	CHECK(loonggpu_bo_list_update(NULL, 1, NULL, NULL) == -EINVAL,
	      "BO list update rejects a NULL list");
	CHECK(loonggpu_bo_va_op_raw(NULL, NULL, 0, 0, 0, 0, 0) == -EINVAL,
	      "raw VA operation rejects a NULL device");
	CHECK(loonggpu_cs_submit_raw2(NULL, NULL, 0, 0, NULL, NULL) == -EINVAL,
	      "raw submission rejects a NULL device");

	ret = loonggpu_query_hw_ip_available(NULL, LOONGGPU_HW_IP_COMPUTE,
					    &available);
	CHECK(ret == -EINVAL && !available,
	      "HW-IP availability clears its output on a NULL device");
	CHECK(loonggpu_query_hw_ip_available(NULL, LOONGGPU_HW_IP_COMPUTE,
					    NULL) == -EINVAL,
	      "HW-IP availability rejects a NULL output");

	CHECK(loonggpu_bo_gem_handle(NULL) == 0,
	      "the GEM handle of a NULL BO is zero");
	CHECK(!loonggpu_bo_is_shared(NULL),
	      "a NULL BO is never reported as shared");
	CHECK(loonggpu_device_get_fd(NULL) < 0,
	      "the fd of a NULL device is invalid");
	CHECK(loonggpu_get_marketing_name(NULL) == NULL,
	      "the marketing name of a NULL device is NULL");
}

int main(void)
{
	check_handle_table();
	check_fence_helpers();
	check_argument_validation();

	if (failures) {
		fprintf(stderr, "%u contract checks failed\n", failures);
		return 1;
	}

	puts("LoongGPU contract checks passed");
	return 0;
}
