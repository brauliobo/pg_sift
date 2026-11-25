/*
 * agg/v6/topn/rows.h - ORDER BY .. LIMIT K of the rows of one table in SiftAggV6: the rows the scan keeps are offered to a heap of the best K by their sort columns (a row is built only as far as the
 * first column that tells it from the worst row held), and the heap keeps each winner's sort columns and its heap address; the node fetches the winners from the heap in order, so a statement that reads
 * 105 columns reads them for K rows.
 */
#ifndef SIFT_AGG_V6_ROWS_TOPN_H
#define SIFT_AGG_V6_ROWS_TOPN_H

#include "postgres.h"

#include "agg/v6/v6.h"


#define SIFT_V6_ROWS_ADDR_BITS	48

/* EXPLAIN of a row Top-N: its sort keys and the rows its heap holds */
extern void sift_v6_rows_explain(ExplainState *es, const SiftV6Spec *spec);
/* spec.topk rows, the sort columns being the keys of the spec in the order of spec.topk_item */
extern SiftV6Rows *sift_v6_rows_new(MemoryContext cx, const SiftV6Spec *spec);
/* the rows of bs, whose keys are keyvec and whose heap addresses are tid[row] */
extern void sift_v6_rows_update(SiftV6Rows *r, const SiftBatchSel *bs, SiftVec *const *keyvec, const uint64 *tid);
/* One row by the Datums of its sort columns (spec.key order) and its heap address: kept if it is among the best */
extern void sift_v6_rows_put(SiftV6Rows *r, const Datum *values, const bool *isnull, uint64 tid);
/* A row the compute threads of the backend (agg/v6/par/run.h) found among the best: its sort columns (isnull; ival for an integer, str for a text) and its heap address */
extern void sift_v6_rows_put_raw(SiftV6Rows *r, const int64 *ival, const char *const *str, const uint32 *slen, const bool *isnull, uint64 tid);
/* What a row must beat to stay, as the scan takes it (col/f2/sink/sink.h): kept up to date by sift_v6_rows_update; NULL when the first sort column is not an integer the zones of a granule bound */
extern const SiftF2Bound *sift_v6_rows_bound(const SiftV6Rows *r);
extern void sift_v6_rows_finish(SiftV6Rows *r);
/* the rows offered from now on are those of partition source: their addresses are told from those of the others */
extern void sift_v6_rows_source(SiftV6Rows *r, uint32 source);
/* the partition and the heap address of the next best row; false after the last */
extern bool sift_v6_rows_next(SiftV6Rows *r, uint32 *source, uint64 *tid);
/* the key of the row sift_v6_rows_next gave: a sort column, or a column of a dimension the row carries */
extern Datum sift_v6_rows_key(const SiftV6Rows *r, int key, bool *isnull);
/* The aggregates of the whole set a row Top-N carries: a group of no keys, which the scan feeds every row it keeps */
extern SiftV6Group *sift_v6_rows_totals_new(MemoryContext cx, const SiftV6Spec *spec);

#endif							/* SIFT_AGG_V6_ROWS_TOPN_H */
