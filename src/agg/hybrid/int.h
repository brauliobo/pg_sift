/*
 * agg/hybrid/int.h - shared between hybrid_plan.c and hybrid_exec.c: the layout of the SiftHybridScan plan and the entry points of its path and executor.
 *
 * SiftHybridAgg lays its plan out the same way (custom_private = ints, qinfo, estimate, then the packed SiftV6Spec; custom_exprs = the query, the pushed quals, then the clauses the heap
 * decides, whose qinfo kinds are SHQ_RESIDUAL and SHQ_EXISTS), with scanrelid 0 and a custom_scan_tlist of the keys and aggregates of the groups followed by the Vars the quals read.
 *
 * CustomScan layout (scanrelid is the heap, custom_scan_tlist the Vars of the rel the query reads, then every sift.score of it):
 *   custom_private = list_make3(ints, qinfo, estimate)
 *     ints         = SiftHybridPriv fields as an integer list
 *     qinfo        = of each pushed qual three integers: its SiftHybQualKind, the column of the store's index it is on, whether its pattern folds (agg/hybrid/quals.h)
 *     estimate     = SIFT_NHYBE float8 Consts, the numbers of the planner that the executor takes the direction of SIFT_HYB_ADAPTIVE from
 *   custom_exprs   = the query (an expression: a Const, a Param, a stable function of them), then the pushed quals with their column on the left: the Params of both are what makes
 *                    the scan rescan when one changes, and what setrefs.c keeps true
 *   plan.qual      = every qual of the scan (the pushed and the others), which the executor's scan keeps for the rows it returns
 */
#ifndef SIFT_HYBRID_INT_H
#define SIFT_HYBRID_INT_H

#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/hybrid/hybrid.h"
#include "bm25/eval/eval.h"

typedef enum SiftHybridPriv
{
	SIFT_HYBP_DIR,				/* SiftHybridDir */
	SIFT_HYBP_HOST,				/* SiftHybridHost */
	SIFT_HYBP_TEXT_INDEX,		/* Oid of the sift index of the query */
	SIFT_HYBP_STORE_OID,		/* Oid of the sift_col index (a sift_table is its own store: its Oid) */
	SIFT_HYBP_K,				/* LIMIT of a ranked scan, 0 without */
	SIFT_HYBP_RANKED,			/* the scan delivers rows by descending score */
	SIFT_HYBP_TIES,				/* the consumer is an incremental sort under a LIMIT of K rows (set once the plan is finished, planner_ties.c): A hands out the group tied with the K-th */
	SIFT_HYBP_COVERED,			/* the store holds every column the query reads */
	SIFT_HYBP_RESIDUAL,			/* a qual the store does not decide: no LIMIT is pushed into the ranked search */
	SIFT_NHYBP
} SiftHybridPriv;

/* The matches of an unranked iteration of a text plan as keys, in heap order: a SiftHybridHitFn (hybrid.h) over a SiftMatchKeys that sift_iter_begin filled in */
typedef struct SiftMatchKeys
{
	SiftMatchIter *it;
	SiftPageMatch pm;
	int			word;
} SiftMatchKeys;

extern bool sift_match_keys_next(void *arg, SiftKey *key, float4 *score);
/* The next match of a page in heap order, taken off the page from *word on (the next page starts at word 0) */
extern bool sift_match_page_next(SiftPageMatch *pm, int *word, SiftKey *key);

extern PGDLLIMPORT const CustomScanMethods sift_hybrid_plan_methods;
extern PGDLLIMPORT const CustomScanMethods sift_hybrid_agg_plan_methods;
extern PGDLLIMPORT const CustomPathMethods sift_hybrid_path_methods;
extern void sift_hybrid_semi_init(void);

/* The numbers of the estimate that go to the executor, in order: rows, granules, col_granules, heap_fetch, sel_col, limit, allvisfrac */
#define SIFT_NHYBE		7

#endif							/* SIFT_HYBRID_INT_H */
