/*
 * core/tomb_int.h - what tomb.c and tomb_write.c share of the tombstone log: the reset of the cache and the merge of its recent entries into the main array
 */
#ifndef SIFT_CORE_TOMB_INT_H
#define SIFT_CORE_TOMB_INT_H

#include "core/tomb.h"

extern void tomb_reset(SiftTombLog *t, const SiftSegEntry *e);
extern void tomb_flatten(SiftTombLog *t);

#endif							/* SIFT_CORE_TOMB_INT_H */
