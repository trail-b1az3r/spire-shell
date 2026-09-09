#ifndef SPIRE_HISTORY_H
#define SPIRE_HISTORY_H

#include "common.h"

void history_init(int max_entries, const char *path);
void history_add(const char *line);          /* also appends to the history file */
void history_save(void);
size_t history_count(void);
const char *history_get(size_t index_from_oldest); /* NULL if out of range */
const char *history_get_relative(int back);   /* 1 = most recent, 2 = one before, ... */
void history_print(void);

/* Most recent entry that starts with `prefix` (case-sensitive), other than
 * an exact match. NULL if none or prefix is empty. Used for fish-style
 * inline autosuggestions. */
const char *history_find_prefix_match(const char *prefix);

/* For Ctrl-R reverse incremental search: the "back" distance (as used by
 * history_get_relative) of the most recent entry containing `needle` as a
 * substring, searched starting strictly further back than `search_from_back`
 * (0 to start from the very latest entry). Returns 0 if none found. */
int history_find_substring(const char *needle, int search_from_back);

#endif
