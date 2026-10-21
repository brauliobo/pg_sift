/*
 * agg/search.h - a BM25 match set as the row selection of an aggregate scan (docs/COL-SEARCH.md).
 *
 * `col @@> query` conjuncts of the WHERE of a query that SiftAgg answers are not evaluated per row: the BM25 index (`sift`) evaluates the
 * queries to the keys (blk << 16 | off) of the matching rows, superblock by superblock, without visiting the heap, and the source of the
 * column data keeps only those rows of its units (sift_colsrc_select). Visibility is the one rule of every SiftAgg scan: the rows that
 * survive the value filters are vetted at the end (all-visible pages pass, the others are read in the heap), so a match set needs no
 * visibility check of its own. The rows a phrase leaves undecided are rechecked on their text, as the scan of the index does.
 */
#ifndef SIFT_SEARCH_H
#define SIFT_SEARCH_H

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

#include "core/filter.h"

/* ---- planner ---- */

typedef struct SiftSearchPlan
{
	Node	   *spec;			/* List: the Oid (OIDOID Const) of the BM25 index, then the arms, each a List of query operands (ANDed); the arms are ORed. What SiftAggSpec.search carries */
	double		sel;			/* selectivity of the conjuncts taken out of the WHERE */
	double		docs;			/* matches expected (rows of the table times sel) */
	double		postings;		/* postings the index reads for them (0: not known, a query that is not a constant) */
	double		terms;
	char	   *desc;			/* EXPLAIN */
} SiftSearchPlan;

/*
 * The conjuncts of *quals that are `key @@> q` of one BM25 index of rel (q free of Vars, volatile functions and sub-plans) leave *quals,
 * and the plan to evaluate them in the index is returned. NULL when there is none, or when the column source (the sift_col index
 * indexoid, or the sift_table heapoid when there is no index) cannot be restricted to a match set.
 * allow_or: with no such conjunct, one conjunct `key @@> a OR key @@> b ...` of one index (every branch a match of it) leaves *quals too
 * and the plan is the union of the arms: the match set of the OR, as a set.
 */
extern SiftSearchPlan *sift_search_extract(PlannerInfo *root, RelOptInfo *rel, List **quals, Oid indexoid, Oid heapoid, bool allow_or);

/* Is the operand (a query text, a limit) the same at every execution of the plan: no sub-plan, no executor parameter */
extern bool sift_operand_ok(Node *e);

/* EXPLAIN: "orders_title @@> 'wireless headphones'" */
extern char *sift_search_describe(Node *spec);

/* The scan reads `*rows` of the table's `rows` values per leaf in `*units` of `units` units, and the match set costs the returned operator costs */
extern double sift_search_open_cost(const SiftSearchPlan *sp, double nrows);
extern double sift_search_cost(const SiftSearchPlan *sp, double nrows, double nunits, double *rows, double *units);

/* ---- executor ---- */

/* Superblocks whose matches are held at a time (a page of matches takes 64 bytes); sift.test_search_chunk_sbs */
extern PGDLLIMPORT int sift_search_chunk_sbs;
extern void sift_search_init(void);

typedef struct SiftSearch SiftSearch;

/* spec: SiftAggSpec.search. econtext evaluates the query operands (parent: their plan state, NULL in a worker) */
extern SiftSearch *sift_search_open(Node *spec, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt);
extern void sift_search_close(SiftSearch *s);

/* The matches in the superblocks [lo, hi), valid until the next call. Calls ascend, or the iteration starts again */
extern const SiftFilter *sift_search_range(SiftSearch *s, uint32 lo, uint32 hi);

extern int64 sift_search_matches(const SiftSearch *s);
extern const char *sift_search_name(const SiftSearch *s);

#endif							/* SIFT_SEARCH_H */
