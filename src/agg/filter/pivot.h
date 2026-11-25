/*
 * agg/filter/pivot.h - up to 64 different FILTER (WHERE ..) clauses of the aggregates of one scan, evaluated once for each batch of a scan: the pivot of a BI query
 * (sum(x) FILTER (WHERE month = 1), sum(x) FILTER (WHERE month = 2), ..) reads the table once instead of once for each clause.
 *
 * SiftFilterMasks keeps the clause bitmaps of a batch for a caller that has its own accumulators (the groups of agg/group/group.c). Each distinct clause is evaluated once for a batch
 * (SiftPivotEval gives its rows as a bitmap, the form agg/vexpr.h produces), however many aggregates carry it; a grouped scan transposes the clause bitmaps of each 64 rows into a 64-bit mask
 * for each row (bit f: the row passes clause f), so that the row's one load tells every aggregate whether the row counts.
 */
#ifndef SIFT_AGG_V6_FILTER_PIVOT_H
#define SIFT_AGG_V6_FILTER_PIVOT_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/batch.h"

#define SIFT_PIVOT_MAX_FILTERS	64

/* Writes into out (SIFT_VEC_WORDS words) the rows of the batch for which the clause is true (false and NULL are not); the bits past the batch are ignored */
typedef void (*SiftPivotEval) (void *ctx, int filter, uint64 *out);

/*
 * sift_fmasks_eval asks eval for each clause once and keeps its rows AND the active rows of the batch; sift_fmasks_rows is the transposed form, bit f of rows[r] set when row r passes clause f,
 * valid until the next sift_fmasks_eval.
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
