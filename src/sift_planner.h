/*
 * sift_planner.h - contract between the planner hooks (sift_planner.c) and
 * the custom scan executor (sift_cscan.c).
 *
 * CustomScan layout (both SiftScan and SiftCount):
 *   custom_private = list of Const/Integer nodes, see SiftPrivateField
 *   custom_exprs   = the query right-hand sides (Const or Param), ANDed
 *   custom_scan_tlist:
 *     SiftScan:  needed Vars of the rel, then every sift.score/max_score
 *                expression of the rel (setrefs maps them to INDEX_VAR;
 *                the executor finds them with sift_is_score_expr)
 *     SiftCount: the count(*) Aggrefs
 */
#ifndef SIFT_PLANNER_H
#define SIFT_PLANNER_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

typedef enum SiftScanMode
{
	SIFT_MODE_UNORDERED,		/* heap order, optional scores */
	SIFT_MODE_RANKED,			/* descending score, k hint */
	SIFT_MODE_COUNT				/* scanrelid 0, one row of counts */
} SiftScanMode;

typedef enum SiftPrivateField
{
	SIFT_PRIV_INDEX_OID,		/* Oid */
	SIFT_PRIV_MODE,				/* SiftScanMode */
	SIFT_PRIV_K,				/* int64 k hint, 0 = unbounded */
	SIFT_PRIV_HEAP_OID,			/* Oid of the scanned heap */
	SIFT_NPRIV
} SiftPrivateField;

extern PGDLLIMPORT const CustomPathMethods sift_scan_path_methods;
extern PGDLLIMPORT const CustomPathMethods sift_count_path_methods;
extern PGDLLIMPORT const CustomScanMethods sift_scan_plan_methods;

/* sift_cscan.c: CreateCustomScanState callback */
extern Node *sift_create_scan_state(CustomScan *cscan);

/* true if expr (a Var or an expression, RelabelTypes ignored) is the key of the sift index */
extern bool sift_index_matches_key(IndexOptInfo *index, Node *expr);

/*
 * Clause recognition shared by hooks, amcostestimate and support functions:
 * true if clause is `key @@> q` (or sift.match_idx(key, q, index)) where key
 * matches the index key and q is pseudo-constant; *query receives q.
 */
extern bool sift_match_clause(Node *clause, IndexOptInfo *index, Expr **query);

/* true if expr is sift.score(ctid) / sift.max_score(ctid) of rel relid */
extern bool sift_is_score_expr(Node *expr, Index relid, bool *is_max);

#endif							/* SIFT_PLANNER_H */
