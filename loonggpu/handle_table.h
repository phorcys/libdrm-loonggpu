/* SPDX-License-Identifier: MIT */
#ifndef LOONGGPU_HANDLE_TABLE_H
#define LOONGGPU_HANDLE_TABLE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Handle -> value map for GEM handles, indexed directly by the handle itself.
 *
 * DRM handle IDs are small integers, so the common case is a single array
 * lookup.  Handles above LOONGGPU_HANDLE_TABLE_DENSE_LIMIT are kept in a short
 * overflow list instead, so an arbitrary 32-bit ID cannot force a multi
 * gigabyte allocation.
 *
 * The table is not internally locked; the owner serialises access.
 */
struct loonggpu_handle_table {
	uint32_t max_key;
	void **values;
	struct loonggpu_handle_table_entry *overflow;
};

struct loonggpu_handle_table_entry {
	uint32_t key;
	void *value;
	struct loonggpu_handle_table_entry *next;
};

#define LOONGGPU_HANDLE_TABLE_DENSE_LIMIT (1u << 20)

static inline int
loonggpu_handle_table_insert(struct loonggpu_handle_table *table,
			     uint32_t key, void *value)
{
	if (!table || !value)
		return -EINVAL;
	if (key < table->max_key && table->values[key])
		return -EEXIST;
	if (key >= LOONGGPU_HANDLE_TABLE_DENSE_LIMIT) {
		struct loonggpu_handle_table_entry *entry;

		for (entry = table->overflow; entry; entry = entry->next) {
			if (entry->key == key)
				return -EEXIST;
		}

		entry = malloc(sizeof(*entry));
		if (!entry)
			return -ENOMEM;
		entry->key = key;
		entry->value = value;
		entry->next = table->overflow;
		table->overflow = entry;
		return 0;
	}
	if (key >= table->max_key) {
		const uint64_t alignment = 64;
		uint64_t new_max_key = ((uint64_t)key + alignment) & ~(alignment - 1);
		size_t new_size;
		void **values;

		if (new_max_key > UINT32_MAX ||
		    new_max_key > SIZE_MAX / sizeof(void *))
			return -ENOMEM;
		new_size = (size_t)new_max_key * sizeof(void *);
		values = realloc(table->values, new_size);
		if (!values)
			return -ENOMEM;
		memset(values + table->max_key, 0,
		       ((size_t)new_max_key - table->max_key) * sizeof(void *));
		table->max_key = (uint32_t)new_max_key;
		table->values = values;
	}
	table->values[key] = value;
	return 0;
}

static inline void
loonggpu_handle_table_remove(struct loonggpu_handle_table *table,
			     uint32_t key)
{
	struct loonggpu_handle_table_entry **link;
	struct loonggpu_handle_table_entry *entry;

	if (!table)
		return;
	if (key < table->max_key) {
		table->values[key] = NULL;
		return;
	}
	link = &table->overflow;
	while (*link) {
		if ((*link)->key == key) {
			entry = *link;
			*link = entry->next;
			free(entry);
			return;
		}
		link = &(*link)->next;
	}
}

static inline void *
loonggpu_handle_table_lookup(struct loonggpu_handle_table *table,
			     uint32_t key)
{
	struct loonggpu_handle_table_entry *entry;

	if (!table)
		return NULL;
	if (key < table->max_key)
		return table->values[key];
	for (entry = table->overflow; entry; entry = entry->next) {
		if (entry->key == key)
			return entry->value;
	}
	return NULL;
}

static inline void
loonggpu_handle_table_fini(struct loonggpu_handle_table *table)
{
	if (!table)
		return;
	while (table->overflow) {
		struct loonggpu_handle_table_entry *entry = table->overflow;

		table->overflow = entry->next;
		free(entry);
	}
	free(table->values);
	table->max_key = 0;
	table->values = NULL;
}

#endif
