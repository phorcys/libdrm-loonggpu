/* SPDX-License-Identifier: MIT */
/*
 * The VA manager must refuse a free that cannot describe an allocation it
 * handed out.  The cases are about relations, so every address is taken from
 * the allocator instead of being assumed.
 *
 * The manager keeps two structures: a bump pointer for fresh ranges and a list
 * of holes left by frees.  Everything it hands out lies below the bump pointer,
 * so a free at or above it, an unaligned one, one that overlaps a recorded hole
 * or one that wraps cannot come from this manager.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "loonggpu_internal.h"

static int failures;

static void check(bool ok, const char *name)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		failures++;
}

int main(void)
{
	struct loonggpu_va_mgr mgr;
	uint64_t a = 0, b = 0, c = 0;
	int r;

	/* va_max is the end of the managed range, not a size. */
	loonggpu_vamgr_init(&mgr, 0x100000, 0x200000, 0x1000);

	r = loonggpu_vamgr_alloc(&mgr, 0x1000, 0, 0, &a);
	check(r == 0 && a == 0x100000,
	      "the first allocation lands at the managed start");
	r = loonggpu_vamgr_alloc(&mgr, 0x1000, 0, 0, &c);
	check(r == 0 && c == 0x101000, "the next allocation follows it");

	check(loonggpu_vamgr_free(&mgr, a, 0x1000) == 0,
	      "a matching free is accepted");
	check(loonggpu_vamgr_free(&mgr, a, 0x1000) == -EINVAL,
	      "the second free of the same range is refused");
	check(loonggpu_vamgr_free(&mgr, a + 8, 0x1000) == -EINVAL,
	      "an unaligned address is refused");
	check(loonggpu_vamgr_free(&mgr, 0x0f0000, 0x1000) == -EINVAL,
	      "an address below the managed range is refused");
	check(loonggpu_vamgr_free(&mgr, 0x1ff000, 0x1000) == -EINVAL,
	      "an address above the bump pointer is refused");
	check(loonggpu_vamgr_free(&mgr, 0xfffffffffffff000ull, 0x2000) == -EINVAL,
	      "a range that wraps is refused");

	check(loonggpu_vamgr_free(&mgr, c, 0x1000) == 0,
	      "a second allocated range frees as well");
	check(loonggpu_vamgr_free(&mgr, a + 0x800, 0x1000) == -EINVAL,
	      "a free overlapping a recorded hole is refused");

	/* The two frees above are adjacent, so they must coalesce into one hole
	 * that the next allocation reuses from its start. */
	r = loonggpu_vamgr_alloc(&mgr, 0x2000, 0, 0, &b);
	check(r == 0 && b == a, "adjacent holes coalesce and are reused");

	check(loonggpu_vamgr_free(&mgr, 0x1ff000, 0) == 0,
	      "a zero-size free is a no-op");

	/* The refused frees left the manager usable. */
	check(loonggpu_vamgr_free(&mgr, b, 0x2000) == 0,
	      "the manager still accepts a correct free after the refusals");

	loonggpu_vamgr_deinit(&mgr);

	/* Requested alignment cannot weaken the device page alignment. */
	loonggpu_vamgr_init(&mgr, 0x1000, 0x10000, 0x1000);
	check(loonggpu_vamgr_alloc(&mgr, 1, 1, 0x1001, &a) == -EINVAL,
	      "a fixed address must satisfy the device alignment");
	check(loonggpu_vamgr_alloc(&mgr, 1, 1, 0, &a) == 0 && a == 0x1000,
	      "a small requested alignment still produces page aligned storage");
	check(loonggpu_vamgr_free(&mgr, a, 1) == 0,
	      "an allocation with a small alignment can be freed");
	loonggpu_vamgr_deinit(&mgr);

	/* Alignment padding remains available to smaller allocations. */
	loonggpu_vamgr_init(&mgr, 0x1000, 0x5000, 0x1000);
	check(loonggpu_vamgr_alloc(&mgr, 0x1000, 0x4000, 0, &a) == 0 &&
	      a == 0x4000, "a larger alignment skips the initial pages");
	check(loonggpu_vamgr_alloc(&mgr, 0x3000, 0, 0, &b) == 0 && b == 0x1000,
	      "alignment padding can be allocated after the bump region is full");
	loonggpu_vamgr_deinit(&mgr);

	loonggpu_vamgr_init(&mgr, 0x1000, 0xa000, 0x1000);
	check(loonggpu_vamgr_alloc(&mgr, 0x1000, 0, 0, &a) == 0 &&
	      loonggpu_vamgr_free(&mgr, a, 0x1000) == 0,
	      "a free hole reaches the bump pointer");
	check(loonggpu_vamgr_alloc(&mgr, 0x2000, 0x8000, 0, &a) == 0 &&
	      a == 0x8000, "an aligned allocation extends the adjacent gap");
	check(loonggpu_vamgr_alloc(&mgr, 0x7000, 0, 0x1000, &b) == 0,
	      "alignment padding coalesces with an adjacent free hole");
	loonggpu_vamgr_deinit(&mgr);

	loonggpu_vamgr_init(&mgr, UINT64_MAX - 0x1fff, UINT64_MAX, 0x1000);
	check(loonggpu_vamgr_alloc(&mgr, 0x3000, 0, 0, &a) == -ENOMEM,
	      "a wrapping allocation end is refused");
	check(loonggpu_vamgr_alloc(&mgr, 0x1000, 0x4000, 0, &a) == -ENOMEM,
	      "overflow when aligning the base is refused");
	check(loonggpu_vamgr_alloc(&mgr, UINT64_MAX, 0, 0, &a) == -EINVAL,
	      "overflow when rounding the size is refused");
	loonggpu_vamgr_deinit(&mgr);

	if (failures) {
		fprintf(stderr, "%d VA manager checks failed\n", failures);
		return 1;
	}
	puts("VA manager guard checks passed");
	return 0;
}
