/*
 * agg/vnum.h - sum and avg of numeric programs on int64 vectors (agg/vnum.c)
 */
#ifndef SIFT_AGG_VNUM_H
#define SIFT_AGG_VNUM_H

#include "agg/agg_fwd_int.h"
#include "agg/spec.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* whether the program is made of the steps vnum_sum_update runs (leaves, constants, + - * and the unary minus) and fits its stack */
extern bool vnum_program_ok(const SiftAggNum *p);

/* sum of program p over the nsel selected tuples of the chunk (ok: the tuples that count, NULL all) into aggregate ai; the caller counts them */
extern void vnum_sum_update(AggExec *ex, int ai, const SiftAggNum *p, int nsel, const uint64 *ok);

/* the next call reads the leaves again: a new set of tuples (a chunk, or a grouping set's) */
extern void vnum_chunk_reset(AggExec *ex);

#pragma GCC visibility pop

#endif							/* SIFT_AGG_VNUM_H */
