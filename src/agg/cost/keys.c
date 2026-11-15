/*
 * agg/cost/keys.c - the plan of a unique key that the prices alone do not rank
 * An IN list on a column with a unique b-tree: the b-tree plan probes each key, which the scan does by reading the units the keys spread over.
 */
#include "postgres.h"

#include "catalog/pg_am.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/cost/int.h"

/* The keys of an IN list on the first column of a unique b-tree: the elements of its array; 0 for any other test (an equality is priced right: 8 against the scan's 150) */
static double
restrict_keys(RelOptInfo *rel, IndexOptInfo *ix, Node *clause)
{
	Oid			opno;
	Node	   *left,
			   *right;
	ArrayType  *a;

	if (!IsA(clause, ScalarArrayOpExpr) || !((ScalarArrayOpExpr *) clause)->useOr)
		return 0;
	opno = ((ScalarArrayOpExpr *) clause)->opno;
	left = linitial(((ScalarArrayOpExpr *) clause)->args);
	right = lsecond(((ScalarArrayOpExpr *) clause)->args);
	if (!IsA(left, Var) || ((Var *) left)->varno != rel->relid || ((Var *) left)->varattno != ix->indexkeys[0] ||
		!IsA(right, Const) || ((Const *) right)->constisnull || !op_in_opfamily(opno, ix->opfamily[0]))
		return 0;
	a = DatumGetArrayTypeP(((Const *) right)->constvalue);
	return ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
}

/*
 * Does a unique b-tree answer the keys of the WHERE for less than the scan costs? One probe and one row a key, warm, which is 4 us (AGG_KEY_PROBE): 60 keys of
 * 100,000 rows run 0.36 ms in the b-tree plan and 0.82 in the scan, and PostgreSQL prices its plan by cold pages (60 keys: 428 against the scan's 335), so
 * the prices alone chose the scan. 500 keys cost the probes more than the scan reads (1.34 ms against 0.84 at 100,000 rows), and the scan stays.
 */
bool
agg_unique_keys_first(AggCtx *c, const Path *scan)
{
	double		keys = 0;

	/* with the index scans off there is no plan of the b-tree to prefer */
	if (sift_agg_force || !enable_indexscan || c->joins != NIL || c->nparts != 0)
		return false;
	foreach_ptr(IndexOptInfo, ix, c->rel->indexlist)
	{
		if (!ix->unique || ix->nkeycolumns != 1 || ix->relam != BTREE_AM_OID || ix->indpred != NIL || ix->hypothetical || ix->indexkeys[0] <= 0)
			continue;
		foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
			keys = Max(keys, restrict_keys(c->rel, ix, (Node *) ri->clause));
	}
	if (keys == 0 || keys * AGG_KEY_PROBE * cpu_operator_cost >= scan->total_cost)
		return false;
	agg_refused_f(COST, "an index of the unique key probes its %.0f keys for about %.2f, the scan reads the units they spread over for %.2f", keys,
				  keys * AGG_KEY_PROBE * cpu_operator_cost, scan->total_cost);
	return true;
}
