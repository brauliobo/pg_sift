/*
 * bm25/planner_int.h - shared between the planner files (planner.c, planner_match.c, planner_path.c, planner_plan.c).
 */
#ifndef SIFT_PLANNER_INT_H
#define SIFT_PLANNER_INT_H

#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/pushdown.h"

/* What reading a query's postings costs; est.docs and est.ndocs are the index's view of the matches */
typedef struct IndexWork
{
	Cost		cost;
	Cost		dict_cost;			/* the part of cost that every parallel participant pays: the dictionary lookups */
	double		pages;
	SiftEstimate est;
	bool		known;
} IndexWork;

typedef struct ScanCand
{
	IndexOptInfo *index;
	List	   *queries;			/* operands of the sift quals */
	List	   *residual;			/* RestrictInfos the scan evaluates itself */
} ScanCand;

typedef struct ScoreRefs
{
	Index		relid;
	List	   *found;
} ScoreRefs;

/*
 * The query's leading sort keys that its rel can sort by, and the order a scan that sorts by them delivers:
 * the keys, then the score that breaks their ties when it is the next sort key. keyops are what the executor
 * sorts with (operator, collation, NULLS FIRST of each key); exprs read the rel's columns.
 */
typedef struct KeyedOrder
{
	List	   *pathkeys;
	List	   *exprs;
	List	   *keyops;
} KeyedOrder;

/* Matching documents, the index work, and what verifying a candidate costs per heap tuple */
typedef struct ScanCost
{
	IndexWork	idx;
	double		docs;			/* tuples the scan fetches from the heap */
	Cost		qual;			/* residual quals per tuple */
	Cost		tuple;			/* heap access and residual quals per fetched tuple */
	Cost		page;			/* per page of a fetch in heap order */
	double		pass;			/* fraction of the fetched tuples that pass the scan's own quals */
} ScanCost;

/* The estimates the paths of one index share: the matches and their cost, and what the filter bitmap keeps and where */
typedef struct ScanEst
{
	ScanCost	sc;
	SiftFilterEst fe;
} ScanEst;

/* bm25/planner_match.c */
extern bool scan_match_clause(PlannerInfo *root, Node *clause, IndexOptInfo *index, bool ranked, Expr **query);
extern bool score_walker(Node *node, ScoreRefs *refs);
extern bool score_cutoff(Node *clause, Index relid, bool *lower);
extern bool cuts_from_above(List *cuts, Index relid);
extern bool is_partition(RelOptInfo *rel);
extern List *score_exprs(PlannerInfo *root, RelOptInfo *rel);
extern bool sorted_by_score(PlannerInfo *root, RelOptInfo *rel, PathKey **pk);
extern int64 query_limit(PlannerInfo *root);
extern int64 scan_limit(PlannerInfo *root, bool ranked);
extern List *param_limits(PlannerInfo *root);
extern bool keyed_order(PlannerInfo *root, RelOptInfo *rel, KeyedOrder *ko);

/* bm25/planner_path.c */
extern bool any_list_too_long(PlannerInfo *root, ScanCand *c, const ScanCost *sc);
extern void scan_est(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, SiftScanMode mode, int64 k, BitmapHeapPath *filter,
					 ScanEst *est);
extern bool cheaper(CustomPath *a, CustomPath *b);
extern void add_scan_paths(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, SiftScanMode mode, int64 k,
						   List *pathkeys, BitmapHeapPath *filter, bool push, bool parallel, bool only_best, CustomPath **best,
						   CustomPath **best_partial);
extern CustomPath *keyed_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, const KeyedOrder *ko,
							  int64 k);
extern void unknown_query_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, int64 k, List *pathkeys,
							   BitmapHeapPath *filter, bool parallel, CustomPath **best, CustomPath **best_partial);
extern CustomPath *count_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid,
							  int nworkers);
extern void add_parallel_count(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid);

#endif							/* SIFT_PLANNER_INT_H */
