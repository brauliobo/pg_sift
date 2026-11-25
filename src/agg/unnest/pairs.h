/*
 * agg/unnest/pairs.h - the pairs of (row, element) of a LATERAL unnest as the batches of a pipe (agg/pipe.h): the columns of the row beside the columns of the element, gathered by the pairs
 * that agg/unnest/unnest.h made. Pure C: the run of the leader and the compute threads (agg/par/unnest.h) make their pairs the same way.
 */
#ifndef SIFT_AGG_V6_UNNEST_PAIRS_H
#define SIFT_AGG_V6_UNNEST_PAIRS_H

#include "postgres.h"

#include "agg/gather.h"
#include "agg/pipe.h"
#include "agg/unnest/unnest.h"

#define SIFT_UN_ELEMS_MAX	16

typedef struct SiftUnPairs SiftUnPairs;

/* Pairs for pipe, whose fact slots are the ncols columns of the row and then the nel columns of the element (their types are those of the pipe); the columns of the element in the bits of eused are the ones the pairs gather */
extern SiftUnPairs *sift_un_pairs_new(MemoryContext cx, SiftV6Pipe *pipe, int ncols, int nel, uint32 eused);
/* The index lists of the next pairs, SIFT_VEC_LEN of them at most: what sift_unnest_expand fills */
extern uint32 *sift_un_pairs_pidx(SiftUnPairs *p);
extern uint32 *sift_un_pairs_eidx(SiftUnPairs *p);
/* The m pairs of pidx and eidx (SIFT_GATHER_NONE: a NULL element) into the pipe: fact the columns of the rows, elems the columns of the element */
extern void sift_un_pairs_emit(SiftUnPairs *p, const SiftVec *fact, const SiftVec *elems, uint32 m);

#endif							/* SIFT_AGG_V6_UNNEST_PAIRS_H */
