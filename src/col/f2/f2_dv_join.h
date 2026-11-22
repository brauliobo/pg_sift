/*
 * col/f2/f2_dv_join.h - the deletion vectors of parts joined as they are (spec 49 6): parts with a few dead rows are concatenated granule by granule, and their vectors are carried over to
 * the joined part with the granule numbers moved by the granules of the parts before.
 */
#ifndef SIFT_COL_F2_DV_JOIN_H
#define SIFT_COL_F2_DV_JOIN_H

#include "postgres.h"

#include "access/transam.h"

#include "col/f2/delvec.h"

/* One input of the join: its vector (NULL when it has no dead row) and its granules; the inputs ascend by key */
typedef struct SiftDvJoinIn
{
	const SiftDv *dv;
	uint32		ngranules;
} SiftDvJoinIn;

/*
 * The vector of the joined part: the dirty granules of each input with granule_id + the granules of the inputs before it, so that the result ascends with no sort. Each input is settled
 * against strip_horizon first (sift_dv_settle): a delete that committed before it becomes a plain dead row, one that aborted before it is dropped, and a delete after it keeps its
 * (xmax, cmax), so a join never waits for a deleter and strips nothing a standby's snapshot still needs. *stripped_out is the greatest deleter made plain (invalid when none).
 * NULL when no granule of any input is dirty.
 */
extern SiftDv *sift_dv_concat_shifted(const SiftDvJoinIn *in, int n, uint64 out_gen, uint64 out_dv_gen, FullTransactionId strip_horizon, FullTransactionId *stripped_out);

/* Whether parts of nrows[i] rows and ndead[i] dead rows may be joined with their vectors carried over: the dead rows are less than dead_pct of all and no part is one a rewrite is for */
extern bool sift_dv_join_sparse(const uint64 *nrows, const uint64 *ndead, int n, int dead_pct);

/* Whether the quantile sketches of the parts still hold once their dead rows are carried over (at most 1% dead): a sketch of more is left out and the quantiles are read from the rows */
extern bool sift_dv_join_keeps_sketch(const uint64 *nrows, const uint64 *ndead, int n);

#endif							/* SIFT_COL_F2_DV_JOIN_H */
