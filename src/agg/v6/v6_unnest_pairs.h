/*
 * agg/v6/v6_unnest_pairs.h - the pairs of (row, element) of a LATERAL unnest as the batches of a pipe (agg/v6/v6_pipe.h): the columns of the row beside the element, gathered by the pairs that
 * agg/v6/v6_unnest.h made. Pure C: the run of the leader and the compute threads (agg/v6/par/unnest.h) make their pairs the same way.
 */
#ifndef SIFT_AGG_V6_UNNEST_PAIRS_H
#define SIFT_AGG_V6_UNNEST_PAIRS_H

#include "postgres.h"

#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_unnest.h"

typedef struct SiftUnPairs SiftUnPairs;

/* Pairs for pipe, whose fact slots are the ncols columns of the row (types type[0 .. ncols)) and the element (type[ncols]) */
extern SiftUnPairs *sift_un_pairs_new(MemoryContext cx, SiftV6Pipe *pipe, int ncols, const SiftVecType *type);
/* The index lists of the next pairs, SIFT_VEC_LEN of them at most: what sift_unnest_expand fills */
extern uint32 *sift_un_pairs_pidx(SiftUnPairs *p);
extern uint32 *sift_un_pairs_eidx(SiftUnPairs *p);
/* The m pairs of pidx and eidx (SIFT_GATHER_NONE: a NULL element) into the pipe: fact the columns of the rows, elems the vector of the elements */
extern void sift_un_pairs_emit(SiftUnPairs *p, const SiftVec *fact, const SiftVec *elems, uint32 m);

#endif							/* SIFT_AGG_V6_UNNEST_PAIRS_H */
