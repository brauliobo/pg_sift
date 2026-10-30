/*
 * agg/order/int.h - what the files of agg/order/ share
 * The cost constants of the sort and the heap, and the functions the parts call in one
 * another.
 */
#ifndef SIFT_AGG_ORDER_DIR_INT_H
#define SIFT_AGG_ORDER_DIR_INT_H

#include "agg/plan_int.h"

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */
/* The rows a plan assumes for a LIMIT it cannot know (a parameter of a generic plan): a page of a list */
#define TOPN_ASSUMED_ROWS	1000
/* Rows the heap keeps at most for a plan to be priced as a sort as well (the sort has no zones to skip units by, but no heap to keep) */
#define TOPN_SORT_ROWS	1024
/*
 * The sort reads the keys after the first at every row that passes (the scan reads the first anyway), by positions: a gather, dearer than
 * the decode of a whole column. Measured on 200,000 rows of small integer keys, sorted in memory: 11 ms for each key past the first, where
 * the plain plan's bounded sort of the same rows (cost 15,284) takes 70 ms in the same run, so the key is 0.157 * 15,284 = 2,400 units
 * for 200,000 rows, 0.012 per row, 4.8 operators at the default cpu_operator_cost. The ratio to the plain plan cancels the speed of the host.
 */
#define TOPN_KEY_READ	4.8
/*
 * A tuplesort compares through the tuple comparator, which PostgreSQL's cost_sort prices at 2 * cpu_operator_cost a step, and pays it twice,
 * forming the runs and merging them. Measured on the same rows, the sort that spills (4 keys, 4 MB) takes 172 ms more than the one in
 * memory: 37,600 units on the same scale, 32,000 of them past the writes of the runs (cost_sort's model), against the 8,800 of one operator
 * a step. 4 operators a step makes 35,200 units.
 */
#define TOPN_SPILL_STEP	4.0
/*
 * What a compare of two entries of a key of byte strings costs besides an integer's, in operators, as the scan does it: each row is compared with
 * the n-th best (a dictionary entry once, then it is remembered). Measured on 200,000 random strings of 8 to 15 bytes, LIMIT 10, serial, median of
 * 5: 14.8 ms under the C collation, 25.8 under en_US.utf8 and 22.1 under ICU: 11 ms more is 2,400 units on the scale of TOPN_KEY_READ (1 ms is about
 * 218 units), 0.012 a row, 4.8 operators; the byte order and the datum compares count as one.
 */
#define TOPN_CMP_BYTES	1.0
#define TOPN_CMP_LOCALE	5.0
/*
 * lower() or upper() of an entry, once per distinct entry of a unit (every row, when they are all distinct). Measured on the same 200,000 strings:
 * 38.8 ms against 13.8 for the key itself under C, 97.1 against 25.8 under en_US.utf8 (about three times that): 25 ms is 0.027 units a row, 11 operators
 */
#define TOPN_CMP_XFORM	11.0
/*
 * What a row costs the evaluator of a sort expression besides PostgreSQL's cost of the expression itself (cost_qual_eval_node: an operator is one operator),
 * in operators. Measured on 200,000 rows, LIMIT 10, serial, the scan against the plan's sort: g + 1 5.3 ms (24 ns a row, the call and the datum of the column),
 * numeric * 2 36.7 ms (the numeric of the leaf's scaled integer and the numeric product kept as a string: 157 ns a row more), g::text 15.9 ms. 1 ms is about 218 units.
 */
#define TOPN_EXPR_ROW	1.5
#define TOPN_EXPR_LEAF	0.5
#define TOPN_EXPR_NUMERIC	0.5
#define TOPN_EXPR_STRING	5.0
/*
 * The leaf of each column of the scan tuple when every one of them is a stored value the top-N keeps as an integer: the rows of a unit give the
 * output then, and no heap row is fetched (oleaf[] is -1 for all when one column is not). A column that is not a sort key is decoded from the units,
 * which pays when most of the final rows come from a few of them: the first key follows the heap order (TOPN_OUT_CORR); a random layout leaves those to the heap.
 */
#define TOPN_OUT_CORR	0.9
/*
 * ORDER BY keys after the first nlead, which the scan cannot order by (a column the index does not hold, an expression of other columns): the scan gives
 * the rows in the order of the first nlead, and the Sort above, which has them as its presorted prefix, puts the keys that follow in order. The rows
 * that tie with the last on the first keys must all be there (TOPN_TIES): the keys must be integers, dates or timestamps, whose equality the scan and the Sort
 * see the same way, and the heap is given room for the rows the statistics of the first key say tie with the one that counts. *slack: that room.
 */
#define TOPN_TIES_MAX	8192.0

extern bool topn_bounds(Query *parse, PlannerInfo *root, double *count, double *offset);
extern Cost topn_heap_cost(double read, double n, double cmpx);
extern Cost topn_sort_cost(double rows, int nkey, double keybytes, double cmpx);
extern double topn_key_width(AggCtx *c, Node *e, Oid typid);

extern Node * topn_xform(Node *e, SiftTopXform *xf, Node **konst);
extern bool topn_raw_key(AggCtx *c, int leaf, int xkind);
extern bool topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt, Oid *cmp, Oid *collid, SiftTopXform *xf, List **exprs);
extern double topn_fraction(AggCtx *c, Node *key, int leaf, double matches, int64 limit);
extern bool topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno, List **funcs);
extern void topn_out_leaves(AggCtx *c, List *scan_tlist, int nkey, const int *leaf, const SiftTopXform *xf, int *oleaf);

extern bool window_whole_set(WindowClause *wc);
extern bool topn_windows(AggCtx *c, SiftAggSpec *s, List *funcs, List **scan_tlist);
extern bool topn_joins_ok(AggCtx *c, SiftAggSpec *s);
extern bool topn_array_ok(AggCtx *c, SiftAggSpec *s, const int *leaf, const SiftTopXform *xf, int nkey, Oid collid);
extern bool topn_runs_ok(const AggCtx *c, const SiftAggSpec *s, const int *leaf, const SiftTopXform *xf);
extern bool topn_heads_win(const AggCtx *c, const SiftAggSpec *s, int leaf, double rows);

#endif							/* SIFT_AGG_ORDER_DIR_INT_H */
