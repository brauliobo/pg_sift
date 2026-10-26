/*
 * col/leafshm.h - the statistics of a sift_col index that its leaves, zones and pre-aggregates give the planner, kept in shared memory
 * (sift.col_leaf_cache_kb) for the backends of a server.
 */
#ifndef SIFT_COL_LEAFSHM_H
#define SIFT_COL_LEAFSHM_H

#include "col/pg.h"

extern int	sift_col_leaf_cache_kb;

/* st[ncols], *pre and the arrays of *zs (as many as the leaves with a sample) are filled from what another backend kept for the index under the stamp; false: none, or none that is whole */
extern bool sift_col_leafshm_read(RelFileNumber rel, uint64 stamp, int ncols, SiftColLeafStats *st, SiftColPreStats *pre, SiftColZones *zs);
extern void sift_col_leafshm_write(RelFileNumber rel, uint64 stamp, int ncols, const SiftColLeafStats *st, const SiftColPreStats *pre, const SiftColZones *zs);

#endif
