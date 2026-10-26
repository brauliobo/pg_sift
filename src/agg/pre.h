/*
 * agg/pre.h - the pre-aggregates of a column unit as the records of the seam (agg/colsrc.h, docs/COL-PREAGG.md), shared by the providers
 * of the column engine: the sift_col index and the sift_table table both read units written by the same segment writer.
 */
#ifndef SIFT_AGGPRE_H
#define SIFT_AGGPRE_H

#include "core/host.h"

#include "col/col.h"
#include "col/preagg.h"
#include "agg/colsrc.h"
#include "agg/sel.h"

/* The histograms a spec writes, as the planner sees them (SiftSrcPreagg.aggs is always set: every unit has the sums of its integer leaves) */
extern void sift_aggpre_spec(const SiftCPaggSpec *spec, SiftSrcPreagg *pa);

/* The histogram of the last sift_aggpre_unit_hist and the dictionary of its keys; the rollup of the last sift_aggpre_unit_rollup and the dictionaries of its dims */
typedef struct SiftAggPre
{
	MemoryContext cxt;
	MemoryContext pcx;
	SiftSrcDict hdict;
	SiftSrcDict rdict[SIFT_SRC_MAX_RDIMS];
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

/*
 * SiftSrcProvider.unit_rollup of the list list (a leaf of the source; negative: the rows of the unit) of the unit, whose columns are those of schema.
 * col_of_leaf and leaf_of_col, when set, translate the leaves of the source to the columns of the unit and back (as in sift_aggpre_unit_hist); NULL: the same numbers.
 * Memory valid until the next call.
 */
extern bool sift_aggpre_unit_rollup(SiftAggPre *pre, const SiftColUnit *unit, const SiftColSchema *schema, int list, const SiftSrcRollupNeed *need,
									const int *col_of_leaf, const int *leaf_of_col, SiftSrcRollup *h);

/* SiftSrcProvider.unit_presence of leaf col of the unit: the key list or bitmap of an exact unit, valid until the next call; false when it has none */
extern bool sift_aggpre_unit_presence(const SiftColUnit *unit, int col, SiftSrcPresence *p);

/*
 * The sorted run of leaf col in the unit, whose rows are in the stored numbering: not those of a unit narrowed by the match set sel or by
 * the filters (SiftSrcProvider.run_info); run_heads and run_read are of the run run_info found
 */
extern bool sift_aggpre_run_info(const SiftAggSel *sel, const SiftSrcUnit *unit, const SiftColUnit *cu, int col, uint32 *nvals, uint32 *nnull, uint64 *id);
extern int	sift_aggpre_run_heads(const SiftColUnit *unit, int col, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);

#endif							/* SIFT_AGGPRE_H */
