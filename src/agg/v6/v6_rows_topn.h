/*
 * agg/v6/v6_rows_topn.h - ORDER BY .. LIMIT K of the rows of one table in SiftAggV6: the rows the scan keeps are offered to a heap of the best K by their sort columns (a row is built only as far as the
 * first column that tells it from the worst row held), and the heap keeps each winner's sort columns and its heap address; the node fetches the winners from the heap in order, so a statement that reads
 * 105 columns reads them for K rows.
 */
#ifndef SIFT_AGG_V6_ROWS_TOPN_H
#define SIFT_AGG_V6_ROWS_TOPN_H

#include "postgres.h"

#include "agg/v6/v6.h"


/* spec.topk rows, the sort columns being the keys of the spec in the order of spec.topk_item */
extern SiftV6Rows *sift_v6_rows_new(MemoryContext cx, const SiftV6Spec *spec);
/* the rows of bs, whose keys are keyvec and whose heap addresses are tid[row] */
extern void sift_v6_rows_update(SiftV6Rows *r, const SiftBatchSel *bs, SiftVec *const *keyvec, const uint64 *tid);
extern void sift_v6_rows_finish(SiftV6Rows *r);
/* the heap address of the next best row; false after the last */
extern bool sift_v6_rows_next(SiftV6Rows *r, uint64 *tid);

#endif							/* SIFT_AGG_V6_ROWS_TOPN_H */
