/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "loonggpu_internal.h"

/* Round v up to a multiple of a, saturating instead of wrapping. */
static uint64_t align64(uint64_t v, uint64_t a)
{
	if (!a)
		return v;
	if (v > UINT64_MAX - (a - 1))
		return UINT64_MAX;
	return (v + a - 1) & ~(a - 1);
}

void loonggpu_vamgr_init(struct loonggpu_va_mgr *mgr, uint64_t start,
			 uint64_t max, uint64_t alignment)
{
	memset(mgr, 0, sizeof(*mgr));
	pthread_mutex_init(&mgr->mutex, NULL);
	mgr->va_offset = start;
	mgr->va_base = start;
	mgr->va_max = max;
	mgr->alignment = alignment ? alignment : 4096;
	mgr->holes = NULL;
}

void loonggpu_vamgr_deinit(struct loonggpu_va_mgr *mgr)
{
	struct loonggpu_va_node *n = mgr->holes, *next;

	while (n) {
		next = n->next;
		free(n);
		n = next;
	}
	pthread_mutex_destroy(&mgr->mutex);
}

/*
 * Remove [offset, offset + size) from the free node *link, which must contain
 * it, splitting the node when the request sits in its middle.  Nothing is
 * modified when the split cannot be allocated.
 */
static int hole_take(struct loonggpu_va_node **link, uint64_t offset,
		     uint64_t size)
{
	struct loonggpu_va_node *node = *link;
	uint64_t before = offset - node->offset;
	uint64_t after = node->offset + node->size - (offset + size);
	struct loonggpu_va_node *tail = NULL;

	if (before && after) {
		tail = calloc(1, sizeof(*tail));
		if (!tail)
			return -ENOMEM;
	}

	if (!before && !after) {
		*link = node->next;
		free(node);
	} else if (!before) {
		node->offset = offset + size;
		node->size = after;
	} else {
		node->size = before;
		if (after) {
			tail->offset = offset + size;
			tail->size = after;
			tail->next = node->next;
			node->next = tail;
		}
	}
	return 0;
}

/* Append a range above all existing holes, merging an adjacent last hole. */
static void hole_append(struct loonggpu_va_mgr *mgr, struct loonggpu_va_node *node)
{
	struct loonggpu_va_node **link = &mgr->holes;

	while (*link) {
		if (!(*link)->next && (*link)->offset + (*link)->size == node->offset) {
			(*link)->size += node->size;
			free(node);
			return;
		}
		link = &(*link)->next;
	}
	*link = node;
}

/* Allocate size bytes at exactly base from the free holes. */
static int alloc_exact(struct loonggpu_va_mgr *mgr, uint64_t base, uint64_t size)
{
	struct loonggpu_va_node **link = &mgr->holes;
	struct loonggpu_va_node *node;

	for (; (node = *link); link = &node->next) {
		if (base >= node->offset &&
		    base + size <= node->offset + node->size)
			return hole_take(link, base, size);
	}
	return -ENOMEM;
}

/* Allocate size bytes with the given alignment from the free holes. */
static int alloc_from_holes(struct loonggpu_va_mgr *mgr, uint64_t size,
			    uint64_t align, uint64_t *out)
{
	struct loonggpu_va_node **link = &mgr->holes;
	struct loonggpu_va_node *node;

	for (; (node = *link); link = &node->next) {
		uint64_t end = node->offset + node->size;
		uint64_t start = align64(node->offset, align);
		int r;

		if (start > end || size > end - start)
			continue;
		r = hole_take(link, start, size);
		if (r)
			return r;
		*out = start;
		return 0;
	}
	return -ENOMEM;
}

/*
 * Allocate a VA range.  @base_required == 0 asks for any address; a non-zero
 * value asks for that exact base, which either lands in a free hole or grows
 * the bump region and records the address range that gets skipped.
 *
 * Everything handed out lies below mgr->va_offset, and every hole lies below
 * it too: the bump pointer only moves forward, and loonggpu_vamgr_free() keeps
 * a free inside the allocated part of the range.  That invariant keeps the
 * bump region and the hole list from ever describing the same address.
 */
int loonggpu_vamgr_alloc(struct loonggpu_va_mgr *mgr, uint64_t size,
			uint64_t align, uint64_t base_required,
			uint64_t *allocated)
{
	uint64_t a;
	int r;

	if (!mgr || !size || !allocated || !mgr->alignment ||
	    (mgr->alignment & (mgr->alignment - 1)) ||
	    (align && (align & (align - 1))))
		return -EINVAL;
	a = align > mgr->alignment ? align : mgr->alignment;
	if (size > UINT64_MAX - (mgr->alignment - 1))
		return -EINVAL;

	size = align64(size, mgr->alignment);
	/* The size alone must fit within the managed address space. */
	if (size > mgr->va_max)
		return -EINVAL;
	pthread_mutex_lock(&mgr->mutex);

	if (base_required) {
		uint64_t base = base_required;
		struct loonggpu_va_node *gap;

		if ((base & (a - 1)) || base < mgr->va_base || base > mgr->va_max ||
		    size > mgr->va_max - base) {
			pthread_mutex_unlock(&mgr->mutex);
			return -EINVAL;
		}

		if (base < mgr->va_offset) {
			/* Inside the managed range: only a hole can hold it. */
			r = alloc_exact(mgr, base, size);
			pthread_mutex_unlock(&mgr->mutex);
			if (r)
				return r;
			*allocated = base;
			return 0;
		}

		if (base > mgr->va_offset) {
			/* Record the skipped range so it stays allocatable. */
			gap = calloc(1, sizeof(*gap));
			if (!gap) {
				pthread_mutex_unlock(&mgr->mutex);
				return -ENOMEM;
			}
			gap->offset = mgr->va_offset;
			gap->size = base - mgr->va_offset;
			hole_append(mgr, gap);
		}
		*allocated = base;
		mgr->va_offset = base + size;
		pthread_mutex_unlock(&mgr->mutex);
		return 0;
	}

	r = alloc_from_holes(mgr, size, a, allocated);
	if (!r) {
		pthread_mutex_unlock(&mgr->mutex);
		return 0;
	}
	if (r != -ENOMEM) {
		pthread_mutex_unlock(&mgr->mutex);
		return r;
	}

	{
		uint64_t start = align64(mgr->va_offset, a);
		struct loonggpu_va_node *gap;

		if (start > mgr->va_max || size > mgr->va_max - start) {
			pthread_mutex_unlock(&mgr->mutex);
			return -ENOMEM;
		}
		if (start > mgr->va_offset) {
			gap = calloc(1, sizeof(*gap));
			if (!gap) {
				pthread_mutex_unlock(&mgr->mutex);
				return -ENOMEM;
			}
			gap->offset = mgr->va_offset;
			gap->size = start - mgr->va_offset;
			hole_append(mgr, gap);
		}
		*allocated = start;
		mgr->va_offset = start + size;
	}
	pthread_mutex_unlock(&mgr->mutex);
	return 0;
}

/*
 * Record a freed range.  A correct free always describes an allocation this
 * manager handed out, which means it is aligned, inside the managed range, does
 * not wrap, and lies below the bump pointer without touching a hole that is
 * already recorded.  Accepting anything else either corrupts the hole list or
 * hands the same address out twice.
 */
int loonggpu_vamgr_free(struct loonggpu_va_mgr *mgr, uint64_t address, uint64_t size)
{
	struct loonggpu_va_node *node, *prev = NULL, *n;
	uint64_t aligned;

	if (!mgr)
		return -EINVAL;
	if (!size)
		return 0;
	if (mgr->alignment && (address & (mgr->alignment - 1)))
		return -EINVAL;
	aligned = align64(size, mgr->alignment);
	pthread_mutex_lock(&mgr->mutex);
	if (address < mgr->va_base || address >= mgr->va_offset ||
	    aligned > mgr->va_offset - address) {
		pthread_mutex_unlock(&mgr->mutex);
		return -EINVAL;
	}
	for (n = mgr->holes; n; n = n->next) {
		if (address < n->offset + n->size && n->offset < address + aligned) {
			pthread_mutex_unlock(&mgr->mutex);
			return -EINVAL;
		}
	}
	node = calloc(1, sizeof(*node));
	if (!node) {
		pthread_mutex_unlock(&mgr->mutex);
		/* Without a node the range could never be reused, so the caller
		 * must keep its handle and report the failure. */
		return -ENOMEM;
	}
	node->offset = address;
	node->size = aligned;

	/* Insert sorted, then merge with the neighbours. */
	n = mgr->holes;
	while (n && n->offset < address) {
		prev = n;
		n = n->next;
	}
	if (prev)
		prev->next = node;
	else
		mgr->holes = node;
	node->next = n;

	if (node->next && node->offset + node->size == node->next->offset) {
		struct loonggpu_va_node *next = node->next;

		node->size += next->size;
		node->next = next->next;
		free(next);
	}
	if (prev && prev->offset + prev->size == node->offset) {
		prev->size += node->size;
		prev->next = node->next;
		free(node);
	}
	pthread_mutex_unlock(&mgr->mutex);
	return 0;
}
