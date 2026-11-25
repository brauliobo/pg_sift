/*
 * agg/v6/filter/pivot.h - up to 64 different FILTER (WHERE ..) clauses and the aggregates they filter, answered in one pass over the batches of a scan: the pivot of a BI query
 * (sum(x) FILTER (WHERE month = 1), sum(x) FILTER (WHERE month = 2), ..) reads the table once instead of once for each clause.
 *
 * Each distinct clause is evaluated once for a batch (SiftPivotEval gives its rows as a bitmap, the form agg/v6/vexpr.h produces), however many aggregates carry it. An ungrouped pivot folds
 * each aggregate over the set bits of (clause AND active rows AND argument valid); a grouped one transposes the clause bitmaps of each 64 rows into a 64-bit mask for each row (bit f: the row
 * passes clause f), so that the row's one load tells every aggregate whether the row counts. The aggregates keep the semantics of the SQL ones: an aggregate ignores a row whose argument is NULL,
 * sum, avg, min and max of no row are NULL (sift_pivot_get returns false), a float8 sum adds in the order of the rows, and NaN is above every number for max and min. COUNT(DISTINCT) takes the
 * exact set of v6_distinct.h, one for each such aggregate.
 */
#ifndef SIFT_AGG_V6_FILTER_PIVOT_H
#define SIFT_AGG_V6_FILTER_PIVOT_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/v6/batch.h"

#define SIFT_PIVOT_MAX_FILTERS	64

typedef enum SiftPivotKind
{
	SFP_COUNT_STAR,
	SFP_COUNT,
	SFP_SUM,
	SFP_AVG,
	SFP_MIN,
	SFP_MAX,
	SFP_COUNT_DISTINCT
} SiftPivotKind;

typedef struct SiftPivotAgg
{
	SiftPivotKind kind;
	int			filter;			/* the clause 0 .. nfilters - 1, or -1 for none */
	int			arg;			/* the index of its vector in the args of sift_pivot_update; -1 for COUNT(*) */
} SiftPivotAgg;

/* Writes into out (SIFT_VEC_WORDS words) the rows of the batch for which the clause is true (false and NULL are not); the bits past the batch are ignored */
typedef void (*SiftPivotEval) (void *ctx, int filter, uint64 *out);

typedef struct SiftPivotStats
{
	uint64		batches;
	uint64		filter_evals;	/* calls of the evaluator: nfilters for each batch, not one for each aggregate */
	uint64		row_masks;		/* batches that took the 64-bit mask of each row */
} SiftPivotStats;

/* The accumulated answer of one aggregate of one group */
typedef struct SiftPivotResult
{
	uint64		n;				/* the rows counted: the non-NULL arguments, every row for COUNT(*) */
	SiftI128	w;				/* sum of an integer, int2, int4, int8, a scaled numeric of up to 18 digits (its scale is the column's) or of 38; avg: the same sum */
	double		f;				/* sum, min or max of a float8 */
	int64		i;				/* min or max of an integer */
	SiftI128	w2;				/* min or max of a numeric of 38 digits */
} SiftPivotResult;

typedef struct SiftPivot SiftPivot;

/* distinct_budget: the bytes all the COUNT(DISTINCT) sets may hold (v6_distinct.h spills past it) */
extern SiftPivot *sift_pivot_create(MemoryContext cx, int nfilters, int naggs, const SiftPivotAgg *aggs, uint64 distinct_budget);
/*
 * The active rows of the batch. gid: the group of each row (NULL: one group, 0). args[a.arg] is the vector of the argument a of the aggregates; its type decides the arithmetic: SVEC_I32, SVEC_I64
 * and SVEC_NUM64 as integers, SVEC_NUM128, SVEC_F64, and for COUNT(DISTINCT) also SVEC_UUID and SVEC_STRVIEW (slab: the base of the offsets of long strings, NULL when pointers).
 */
extern void sift_pivot_update(SiftPivot *p, const SiftBatchSel *bs, const uint32 *gid, SiftVec *const *args, const char *slab, SiftPivotEval eval, void *eval_ctx);
/* Ends the scan: the distinct sets resolve their spilled partitions */
extern void sift_pivot_finish(SiftPivot *p);
/* false: the aggregate of the group is NULL (sum, avg, min, max of no row; the group has no rows at all) */
extern bool sift_pivot_get(const SiftPivot *p, uint32 group, int agg, SiftPivotResult *out);
/* COUNT(*), COUNT, COUNT(DISTINCT): never NULL */
extern uint64 sift_pivot_count(const SiftPivot *p, uint32 group, int agg);
extern bool sift_pivot_float_overflow(const SiftPivot *p);
extern const SiftPivotStats *sift_pivot_stats(const SiftPivot *p);
extern void sift_pivot_free(SiftPivot *p);

/*
 * The clause bitmaps of a batch without the aggregates: what a caller that has its own accumulators (the groups of agg/v6/group/group.c) takes from the pivot. sift_fmasks_eval asks eval for each clause once
 * and keeps its rows AND the active rows of the batch; sift_fmasks_rows is the transposed form, bit f of rows[r] set when row r passes clause f, valid until the next sift_fmasks_eval.
 */
typedef struct SiftFilterMasks SiftFilterMasks;

extern SiftFilterMasks *sift_fmasks_create(MemoryContext cx, int nfilters);
extern void sift_fmasks_eval(SiftFilterMasks *m, const SiftBatchSel *bs, SiftPivotEval eval, void *ctx);
extern const uint64 *sift_fmasks_active(const SiftFilterMasks *m);
extern const uint64 *sift_fmasks_bits(const SiftFilterMasks *m, int f);
extern const uint64 *sift_fmasks_rows(SiftFilterMasks *m);
extern void sift_fmasks_free(SiftFilterMasks *m);

/* out[r] bit f = in[f] bit r, for 64 filters and 64 rows (the transposition of a 64 x 64 bit matrix) */
extern void sift_pivot_transpose64(uint64 *a);

#endif							/* SIFT_AGG_V6_FILTER_PIVOT_H */
