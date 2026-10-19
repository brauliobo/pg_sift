/*
 * sift_aggpre.h - the pre-aggregates of a column unit as the records of the seam (sift_colsrc.h, docs/COL-PREAGG.md), shared by the providers
 * of the column engine: the sift_col index and the sift_table table both read units written by the same segment writer.
 */
#ifndef SIFT_AGGPRE_H
#define SIFT_AGGPRE_H

#include "sift_host.h"

#include "sift_col.h"
#include "sift_col_preagg.h"
#include "sift_colsrc.h"

/* The histograms a spec writes, as the planner sees them (SiftSrcPreagg.aggs is always set: every unit has the sums of its integer leaves) */
extern void sift_aggpre_spec(const SiftCPaggSpec *spec, SiftSrcPreagg *pa);

/* The histogram of the last sift_aggpre_unit_hist and the dictionary of its keys */
typedef struct SiftAggPre
{
	MemoryContext cxt;
	MemoryContext pcx;
	SiftSrcDict hdict;
} SiftAggPre;

/* mcxt holds the memory of the histograms for the life of the source */
extern void sift_aggpre_init(SiftAggPre *pre, MemoryContext mcxt);

/* SiftSrcProvider.unit_agg of leaf col (definition def) of the unit; false when the unit may not answer (not exact, an overflow value) */
extern bool sift_aggpre_unit_agg(const SiftColUnit *unit, int col, const SiftColDef *def, SiftSrcUnitAgg *agg);

/*
 * SiftSrcProvider.unit_hist of the dimension col (definition def) of the unit. The measures of the histogram are columns of the unit's schema;
 * leaf_of_col, when set, gives the leaf of the source for each (a table keeps its leaves in the layout of the table, a segment of an older
 * layout has its own columns); NULL: the same numbers. Memory valid until the next call.
 */
extern bool sift_aggpre_unit_hist(SiftAggPre *pre, const SiftColUnit *unit, int col, const SiftColDef *def, const int *leaf_of_col, SiftSrcHist *h);

#endif							/* SIFT_AGGPRE_H */
