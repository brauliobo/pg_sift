/*
 * core/v1_col/tomb_int.h - what tomb.c, tomb_write.c and tomb_iter.c share of the tombstone log: the reset of the cache, the merge of its recent entries into the main array and the lookup of a key in one array
 */
#ifndef SIFT_CORE_TOMB_INT_H
#define SIFT_CORE_TOMB_INT_H

#include "core/v1_col/tomb.h"

extern void tomb_reset(SiftTombLog *t, const SiftSegEntry *e);
extern void tomb_flatten(SiftTombLog *t);
/* The run of entries for key in the sorted array a: its first element and length */
extern const SiftTomb *tomb_find_run(const SiftTomb *a, uint32 na, uint64 key, uint32 *n);

#endif							/* SIFT_CORE_TOMB_INT_H */
