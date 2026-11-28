/*
 * agg/two/hash.h - the rows of the spine of a two-level aggregate (agg/two/spine.c), hashed on their side of the equalities of the join: each cell finds the rows it equals, and
 * the rows of a LEFT JOIN no cell matched are read once all the cells are in.
 */
#ifndef SIFT_AGG_TWO_HASH_H
#define SIFT_AGG_TWO_HASH_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "nodes/execnodes.h"

typedef struct TwoHash TwoHash;

/* the hash of the rows of spine (PlanState) under cx, of the plan's equalities (the spine list of agg/two/int.h); econtext evaluates the keys, cell over the scan tuple and spine over the outer one */
extern TwoHash *sift_two_hash_new(List *spine, PlanState *ps, ExprContext *econtext, MemoryContext cx);
extern bool sift_two_hash_left(const TwoHash *h);
/* the rows the cell equals and the other conditions of the join hold for, one at a time into the spine's slot (NULL after the last) */
extern void sift_two_hash_probe(TwoHash *h, TupleTableSlot *cell);
extern TupleTableSlot *sift_two_hash_next(TwoHash *h);
/* the rows no cell matched, one at a time */
extern TupleTableSlot *sift_two_hash_unmatched(TwoHash *h);

#endif							/* SIFT_AGG_TWO_HASH_H */
