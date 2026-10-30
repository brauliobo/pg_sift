/*
 * agg/plan_range_join.c - the rows of a join of a column with the ranges of a list: `event_date <@ periods.range`.
 *
 * PostgreSQL estimates `elem <@ range` and `range @> elem` between two relations with contjoinsel, a constant 0.001 whatever the ranges hold, so a join of a table with
 * the two ranges of a VALUES list that cover a quarter of its dates is estimated at 60 rows where it returns thousands, and everything above it (nested loops that
 * rescan an aggregate, the price of the normal plan the scan is compared with) is priced for those rows. When the ranges are constants the statistics of the column say
 * how many rows each of them takes: the selectivity of the join clause, which the planner caches in the clause (norm_selec, outer_selec), is the mean over the ranges
 * of the share of the table's rows (those its own conditions keep) that the range takes.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"

/* the constants of column `resno` of the VALUES list a CTE or a VALUES entry reads, NIL when it is anything else (a filter, a join, an expression that is not a constant) */
static List *
list_column(PlannerInfo *root, RangeTblEntry *rte, int resno)
{
	RangeTblEntry *values = rte;
	List	   *out = NIL;

	if (rte->rtekind == RTE_CTE)
	{
		PlannerInfo *body = cte_root(root, rte);
		Query	   *q;
		TargetEntry *te = NULL;
		Var		   *v;

		if (body == NULL)
			return NIL;
		q = body->parse;
		if (q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->setOperations || q->limitCount ||
			q->jointree == NULL || q->jointree->quals != NULL || list_length(q->jointree->fromlist) != 1)
			return NIL;
		foreach_node(TargetEntry, t, q->targetList)
			if (t->resno == resno && !t->resjunk)
				te = t;
		if (te == NULL || !IsA(te->expr, Var))
			return NIL;
		v = (Var *) te->expr;
		values = rt_fetch(v->varno, q->rtable);
		resno = v->varattno;
	}
	if (values->rtekind != RTE_VALUES || values->values_lists == NIL)
		return NIL;
	foreach_ptr(List, row, values->values_lists)
	{
		Node	   *e = list_nth(row, resno - 1);

		if (!IsA(e, Const) || ((Const *) e)->constisnull)
			return NIL;
		out = lappend(out, e);
	}
	return out;
}

/* the btree operator of the strategy for the type, 0 when it has none */
static Oid
elem_operator(Oid typid, int strategy)
{
	TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);

	if (!OidIsValid(tc->btree_opf))
		return InvalidOid;
	return get_opfamily_member(tc->btree_opf, tc->btree_opintype, tc->btree_opintype, strategy);
}

/* the conditions of the range on the column, as the planner would see them as restrictions of the table; false when the range is not one of the type the column has */
static bool
range_conds(Var *col, Const *range, List **conds, bool *empty)
{
	TypeCacheEntry *rt = lookup_type_cache(range->consttype, TYPECACHE_RANGE_INFO);
	RangeBound	lower,
				upper;

	if (rt->rngelemtype == NULL || rt->rngelemtype->type_id != exprType((Node *) col))
		return false;
	range_deserialize(rt, DatumGetRangeTypeP(range->constvalue), &lower, &upper, empty);
	if (*empty)
		return true;
	for (int side = 0; side < 2; side++)
	{
		RangeBound *b = side == 0 ? &lower : &upper;
		int			strategy = side == 0 ? (b->inclusive ? BTGreaterEqualStrategyNumber : BTGreaterStrategyNumber) :
			(b->inclusive ? BTLessEqualStrategyNumber : BTLessStrategyNumber);
		Oid			op;
		Const	   *k;

		if (b->infinite)
			continue;
		if (!OidIsValid(op = elem_operator(rt->rngelemtype->type_id, strategy)))
			return false;
		k = makeConst(rt->rngelemtype->type_id, exprTypmod((Node *) col), exprCollation((Node *) col), rt->rngelemtype->typlen, b->val, false, rt->rngelemtype->typbyval);
		*conds = lappend(*conds, make_opclause(op, BOOLOID, false, (Expr *) copyObject(col), (Expr *) k, InvalidOid, exprCollation((Node *) col)));
	}
	return true;
}

/*
 * The rows a join clause `col <@ range` (or `range @> col`) between the table `rel` and a list of constant ranges keeps: the selectivity of the clause is the mean over
 * the ranges of the share of the table's rows that its own conditions keep and the range takes. Set in the clause for the planner to read when it counts the join.
 */
static void
range_join_clause(PlannerInfo *root, RelOptInfo *rel, RestrictInfo *rinfo)
{
	OpExpr	   *op;
	Node	   *a,
			   *b;
	Var		   *col,
			   *rng;
	RangeTblEntry *other;
	List	   *ranges;
	List	   *restr = rel->baserestrictinfo;
	double		sum,
				alone = 0,
				together = 0,
				base;
	List	   *union_conds = NIL;

	if (rinfo->norm_selec >= 0 || !IsA(rinfo->clause, OpExpr))
		return;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || (op->opfuncid != F_ELEM_CONTAINED_BY_RANGE && op->opfuncid != F_RANGE_CONTAINS_ELEM))
		return;
	a = linitial(op->args);
	b = lsecond(op->args);
	if (op->opfuncid == F_RANGE_CONTAINS_ELEM)
	{
		Node	   *t = a;

		a = b;
		b = t;
	}
	if (!IsA(a, Var) || !IsA(b, Var))
		return;
	col = (Var *) a;
	rng = (Var *) b;
	if (col->varno != rel->relid || col->varlevelsup != 0 || rng->varlevelsup != 0 || rng->varno == rel->relid || col->varattno <= 0 || rng->varattno <= 0)
		return;
	other = planner_rt_fetch(rng->varno, root);
	if ((ranges = list_column(root, other, rng->varattno)) == NIL)
		return;
	base = restr == NIL ? 1.0 : clauselist_selectivity(root, restr, rel->relid, JOIN_INNER, NULL);
	if (base <= 0)
		return;
	foreach_node(Const, r, ranges)
	{
		List	   *conds = NIL;
		bool		empty = false;

		if (r->consttype != exprType((Node *) rng) || !range_conds(col, r, &conds, &empty))
			return;
		if (empty)
			continue;
		alone += clauselist_selectivity(root, conds, rel->relid, JOIN_INNER, NULL);
		together += Min(clauselist_selectivity(root, list_concat(list_copy(restr), conds), rel->relid, JOIN_INNER, NULL) / base, 1.0);
		union_conds = lappend(union_conds, conds == NIL ? (Node *) makeBoolConst(true, false) : (Node *) make_andclause(conds));
	}
	/*
	 * The conditions of the table are no independent evidence when they already keep the rows of the ranges (the statement says the same ranges twice, as a filter
	 * and as the join): every row it keeps is in a range, and the join returns the rows it keeps times the ranges each is in, the sum of the shares of the ranges
	 * over the share of their union.
	 */
	if (restr != NIL && union_conds != NIL && predicate_implied_by(list_make1(make_orclause(union_conds)), restr, false))
	{
		double		one = clauselist_selectivity(root, list_make1(make_orclause(union_conds)), rel->relid, JOIN_INNER, NULL);

		if (one > 0)
			together = Max(alone / one, 1.0);
	}
	sum = together;
	rinfo->norm_selec = rinfo->outer_selec = Max(sum / list_length(ranges), 0.0);
	elog(DEBUG1, "sift: join on a range of %d constant ranges kept %.4f of the rows of the table (the planner's default: 0.001)", list_length(ranges), rinfo->norm_selec);
}

/* called for each base relation before the joins are counted */
void
agg_range_join_rows(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (!sift_enable_agg || rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || !has_column_source(rte->relid))
		return;
	foreach_node(RestrictInfo, rinfo, rel->joininfo)
		range_join_clause(root, rel, rinfo);
}
