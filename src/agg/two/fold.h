/*
 * agg/two/fold.h - the cells of the arms of a two-level aggregate folded into its outer groups: each cell's inputs (agg/two/int.h) evaluated into the vectors of a batch, which the
 * groups of SiftAggV6 (agg/engine.h) take as they take the batches of a scan.
 */
#ifndef SIFT_AGG_TWO_FOLD_H
#define SIFT_AGG_TWO_FOLD_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "nodes/execnodes.h"

#include "agg/engine.h"

typedef struct TwoFold TwoFold;

/* the fold of the spec's inputs into groups that leave as tuples of desc, in memory under cx */
extern TwoFold *sift_two_fold_new(const SiftV6Spec *spec, List *inputs, EState *estate, MemoryContext cx, TupleDesc desc);
/* one cell, the tuple of an arm, and the row of the spine it was joined to (NULL: no spine) */
extern void sift_two_fold_cell(TwoFold *f, TupleTableSlot *cell, TupleTableSlot *spine);
/* the last cells folded: the groups, done */
extern SiftV6Group *sift_two_fold_finish(TwoFold *f);
extern uint64 sift_two_fold_cells(const TwoFold *f);

#endif							/* SIFT_AGG_TWO_FOLD_H */
