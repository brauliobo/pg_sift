/*
 * col/f2/scan/ord_stream.h - the ordered scan of an index of format major 2 (col/f2/scan/ord_stream.c): the rows of the index in the order of its key columns, forward or backward, read one granule at a time
 * from the parts of one partition at a time and merged, with no sort and no collection of the addresses. Only an index whose key_order is the order of its key columns (sift_f2_native_order_matches)
 * gives it: the planner is told no other index has an order (sift_f2_ord_plan_init), so a plan that needs one never asks it of an index of format major 1.
 */
#ifndef SIFT_F2_ORD_STREAM_H
#define SIFT_F2_ORD_STREAM_H

#include "access/relscan.h"
#include "utils/rel.h"

#include "col/f2/key/enc.h"
#include "core/f2/manifest/manifest.h"

struct SiftPlanSnapshot;

/* A row comparison on the leading key columns, in the order of the values: (key 0, .., n - 1) op (vals), op one of SIFT_KOP_LT, LE, GT, GE (col/f2/key/plan.h) */
typedef struct SiftRowBound
{
	int			op;
	int			n;
	SiftKeyVal	vals[SIFT_KEY_COLS_MAX];
} SiftRowBound;

#define SIFT_ROW_BOUNDS_MAX	4

/* The rows of the index come in the order PostgreSQL derives from its key columns and their options: format major 2, key_order the key columns in that order and as they are ordered, each of a type whose values are integers */
extern bool sift_f2_native_order_matches(Relation index);

/* amgettuple / ammarkpos / amrestrpos of the index; an index that is not ordered natively is read as before (addresses in order) and cannot mark */
extern bool sift_f2_ord_gettuple(IndexScanDesc scan, ScanDirection dir);
extern void sift_f2_ord_markpos(IndexScanDesc scan);
extern void sift_f2_ord_restrpos(IndexScanDesc scan);

/* The first partition of the plan that reaches the bound lo (NULL: the first) */
extern int	sift_f2_first_reaching(const struct SiftPlanSnapshot *plan, const uint8 *lo);

/* The last partition from `from` on that reaches the bound hi (NULL: the last), -1 + from when none does */
extern int	sift_f2_last_overlapping(const struct SiftPlanSnapshot *plan, int from, const uint8 *hi);

/* A leaf of one integer column whose stored values are the values of the column */
struct SiftColDef;
extern bool sift_f2_integer_leaf(const struct SiftColDef *d);

/* The planner of sift_col: no index keeps the order of its columns for a plan unless sift_f2_native_order_matches (f2_cost.c) */
extern void sift_f2_ord_plan_init(void);

#endif							/* SIFT_F2_ORD_STREAM_H */
