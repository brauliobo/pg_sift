/*
 * col/v1/src_zone.h - the zone tests of the prune entries of a scan (SiftSrcPrune) as the column engine's predicates, shared by the providers of the seam:
 * the sift_col index and the sift_table table both skip a unit whose zones exclude the entries.
 */
#ifndef SIFT_COL_V1_SRC_ZONE_H
#define SIFT_COL_V1_SRC_ZONE_H

#include "col/col.h"
#include "col/v1/src_exec.h"

/* The most predicates the zone tests of one begin hold (two for each entry that is no junction) */
#define SIFT_SRCZONE_MAX	(2 * SIFT_COLSRC_MAX_LEAVES)

/* What a source keeps between the begins of one scan: the zone trees and the tests of the last entries */
typedef struct SiftSrcZoneTests
{
	const SiftSrcPrune *zfor;	/* the prune entries of the last begin, and the zone trees made of those that are junctions */
	SiftColPrune *zone;
	int			nzone;
	SiftColPred *zpred;			/* the zone tests of the other entries, kept while the entries are the same */
	int			nzpred;
	const int64 *zvals[SIFT_SRCZONE_MAX];	/* the set of values zpred[i] was made from: the same array is the same values (the entries are made once), so a begin does not compare them */
} SiftSrcZoneTests;

/* The value every row of a source holds on a leaf it lacks, in the leaf's vector type: what the catalog gives rows written before ADD COLUMN */
typedef struct SiftSrcConst
{
	bool		isnull;
	int64		v;
} SiftSrcConst;

/*
 * The predicates of the entries that are no junction into out (SIFT_SRCZONE_MAX at most), and z->zone / z->nzone the trees of the junctions, for
 * sift_col_begin_zones. col: the leaf of the source that holds leaf i of the entries (NULL: the same numbers); absent[i]: the value of leaf i in a source
 * that lacks it (col[i] < 0), which decides the entries on it: a source none of whose rows pass an entry keeps no unit.
 */
extern int	sift_srczone_preds(SiftSrcZoneTests *z, MemoryContext cxt, const int *col, const SiftSrcConst *absent, const SiftSrcPrune *prune, int nprune,
							   SiftColPred *out);

#endif							/* SIFT_COL_V1_SRC_ZONE_H */
