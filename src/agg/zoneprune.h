/*
 * agg/zoneprune.h - the zone tests of the prune entries of a scan (SiftSrcPrune) as the column engine's predicates, shared by the providers of the seam:
 * the sift_col index and the sift_table table both skip a unit whose zones exclude the entries.
 */
#ifndef SIFT_AGG_ZONEPRUNE_H
#define SIFT_AGG_ZONEPRUNE_H

#include "core/host.h"

#include "col/col.h"
#include "agg/colsrc.h"

/* The most predicates the zone tests of one begin hold (two for each entry that is no junction) */
#define SIFT_AGGZONE_MAX	(2 * SIFT_COLSRC_MAX_LEAVES)

/* What a source keeps between the begins of one scan: the zone trees and the tests of the last entries */
typedef struct SiftAggZones
{
	const SiftSrcPrune *zfor;	/* the prune entries of the last begin, and the zone trees made of those that are junctions */
	SiftColPrune *zone;
	int			nzone;
	SiftColPred *zpred;			/* the zone tests of the other entries, kept while the entries are the same */
	int			nzpred;
} SiftAggZones;

/*
 * The predicates of the entries that are no junction into out (SIFT_AGGZONE_MAX at most), and z->zone / z->nzone the trees of the junctions, for
 * sift_col_begin_zones. col: the leaf of the source that holds leaf i of the entries (NULL: the same numbers); an entry on a leaf the source lacks tests
 * nothing, and a junction with such an entry is left out whole (a unit of an older layout holds that column as NULLs the zone cannot tell).
 */
extern int	sift_aggzone_preds(SiftAggZones *z, MemoryContext cxt, const int *col, const SiftSrcPrune *prune, int nprune, SiftColPred *out);

#endif							/* SIFT_AGG_ZONEPRUNE_H */
