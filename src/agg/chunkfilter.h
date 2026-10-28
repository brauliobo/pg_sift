/*
 * agg/chunkfilter.h - the integer filters of a scan (SiftSrcFilter) as the column engine's predicates on the encoded chunks of a unit, shared by the
 * providers of the seam: the sift_col index and the sift_table table both apply them in the source, so that the executor does not test the rows again.
 */
#ifndef SIFT_AGG_CHUNKFILTER_H
#define SIFT_AGG_CHUNKFILTER_H

#include "core/host.h"

#include "col/col.h"
#include "agg/colsrc.h"

/* The filters a source applies: the engine predicates, and the leaf of the source's layout each one tests */
typedef struct SiftAggRanges
{
	int			col;
	int			leaf;
	int			narms;
	const int64 *arm;			/* narms pairs (lo, hi), inclusive */
} SiftAggRanges;

typedef struct SiftAggFilters
{
	SiftColPred *pred;
	int		   *leaf;
	int			n;
	SiftAggRanges *rng;			/* the unions of ranges (SSF_RANGES): each arm is run as a range, with the sorted run window, and the arms are ORed */
	int			nrng;
} SiftAggFilters;

/* The text tests of one scan that join the zone pruning of a begin (the others are not used to skip units) */
#define COL_MAX_STR_PRUNE	16

/* A text test (prune_str, filter_str) as an engine predicate on the column col of the source, over the constants in vals (room for t->nvals) */
extern SiftColPred sift_aggfilt_str_pred(const SiftSrcStrTest *t, SiftColValue *vals, int col);

/* Is a filter on a column of this definition one the engine tests as integers on its chunks? */
extern bool sift_aggfilt_int(const SiftColDef *def);

/* Adds the filter f, on the column col of the source (whose layout leaf is f->leaf), to fl; allocates in cxt. The column must pass sift_aggfilt_int. */
extern void sift_aggfilt_add(SiftAggFilters *fl, MemoryContext cxt, int col, const SiftSrcFilter *f);

/* What a run of the filters over a unit reads: lay and tol are the source's (tol: the leaf reads the values the engine cannot hold as NULL) */
typedef struct SiftAggFiltRun
{
	const SiftAggFilters *fl;
	const SiftSrcLayout *lay;
	const bool *tol;
} SiftAggFiltRun;

/*
 * A SiftAggSelOp (agg/sel.h) that narrows the rows of sel in the unit by the filters of arg (a SiftAggFiltRun), the most selective first; the two bounds of
 * a range on one column are one filter, which a sorted run answers. A numeric column keeps the rows whose value overflowed (the NaNs), which only the
 * comparisons that a number greater than all others passes keep.
 */
extern void sift_aggfilt_op(const SiftColUnit *cu, SiftColSel *sel, void *arg);

#endif							/* SIFT_AGG_CHUNKFILTER_H */
