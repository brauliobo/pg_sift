/*
 * col/leafshm.h - the statistics of a sift_col index that its leaves, zones and pre-aggregates give the planner, kept in shared memory
 * (sift.col_leaf_cache_kb) for the backends of a server.
 */
#ifndef SIFT_COL_LEAFSHM_H
#define SIFT_COL_LEAFSHM_H

#include "col/pg.h"

extern int	sift_col_leaf_cache_kb;

/* the two records an index has in the ring: its leaf statistics with the zone sample, and its pre-aggregate counts (only the planner of an aggregate reads those) */
typedef enum LshmKind
{
	LSHM_LEAF,
	LSHM_PRE
}			LshmKind;

/* st[ncols] and the arrays of *zs (as many as the leaves with a sample) are filled from what another backend kept for the index under the stamp; false: none, or none that is whole */
extern bool sift_col_leafshm_read(RelFileNumber rel, uint64 stamp, int ncols, SiftColLeafStats *st, SiftColZones *zs);
extern bool sift_col_leafshm_pre_read(RelFileNumber rel, uint64 stamp, SiftColPreStats *pre);
/* the stamp of the record of the kind the ring holds for the index (the newest: a write drops the older), false: none */
extern bool sift_col_leafshm_peek(RelFileNumber rel, LshmKind kind, uint64 *stamp);
extern void sift_col_leafshm_write(RelFileNumber rel, uint64 stamp, int ncols, const SiftColLeafStats *st, const SiftColZones *zs);
extern void sift_col_leafshm_pre_write(RelFileNumber rel, uint64 stamp, const SiftColPreStats *pre);

#endif
