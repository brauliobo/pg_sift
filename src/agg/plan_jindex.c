/*
 * agg/plan_jindex.c - the conditions of a joined table that narrow the rows its load reads through a btree
 * `column = value`, `column IN (list)` and the bounds `<`, `<=`, `>`, `>=` of a column (the executor takes them as the keys of an index scan, lookup/map.c)
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/spec.h"
#include "agg/plan_int.h"
#include "agg/plan_join_int.h"

/* `<`, `<=`, `>` or `>=` of the default btree operator class of the column's type: a bound of the index scan on the column, as `=` is a point of it */
static bool
btree_bound_op(Oid opno, Oid typid)
{
	Oid			family = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY)->btree_opf;

	return OidIsValid(family) && op_in_opfamily(opno, family) && get_op_opfamily_strategy(opno, family) != BTEqualStrategyNumber;
}

/*
 * `column = value`, `column = ANY (array)` (IN (list)) or `column <, <=, >, >= value` among the table's own conditions (the value reads no column, is not volatile and takes no
 * executor parameter): the rows of the table that can match are those of an index scan on the column, when the table has a btree on it (the executor
 * looks, map_load). The conjunct stays in the filter; this only narrows what is read. A condition that keeps a large share of the table is read faster
 * by the heap scan than through the index (a random heap fetch for each row), so it is only for a condition the statistics of the table expect to keep
 * a small share (the bounds are judged together, join_index_quals).
 */
static bool
index_qual(AggCtx *c, JoinCtx *jc, Node *q, Oid *opno, Oid *coll, Node **col, Node **val, bool *any)
{
	Node	   *l,
			   *r;

	if (IsA(q, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) q)->useOr)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) q;

		*opno = sa->opno;
		*coll = sa->inputcollid;
		l = strip_relabel(linitial(sa->args));
		r = lsecond(sa->args);
		*any = true;
		if (join_of_var(c, l) != jc || clause_selectivity(c->root, q, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
			return false;
	}
	else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) q;

		*opno = op->opno;
		*coll = op->inputcollid;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		*any = false;
		if (join_of_var(c, l) != jc)
		{
			if (join_of_var(c, r) != jc)
				return false;
			*opno = get_commutator(*opno);
			l = r;
			r = strip_relabel(linitial(op->args));
		}
		if (op_mergejoinable(*opno, exprType(l)) && clause_selectivity(c->root, q, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
			return false;
	}
	else
		return false;
	*col = l;
	*val = r;
	return OidIsValid(*opno) && (op_mergejoinable(*opno, exprType(l)) || (!*any && btree_bound_op(*opno, exprType(l)))) && !unsupported_clause(c, r) && !contain_var_clause(r);
}

void
join_index_quals(AggCtx *c, JoinCtx *jc, List *conds)
{
	SiftAggJoin *j = &jc->j;

	if (jc->cpath != NULL || sift_aggjoin_preserves(jc->kind))
		return;
	List	   *taken = NIL;
	bool		bound[SIFT_AGG_MAX_JIDX];
	bool		bounded = false;

	foreach_ptr(Node, q, conds)
	{
		Node	   *col,
				   *val;
		Oid			opno,
					coll;
		bool		any;

		if (j->nidx == SIFT_AGG_MAX_JIDX || !index_qual(c, jc, q, &opno, &coll, &col, &val, &any))
			continue;
		j->idxatt[j->nidx] = ((Var *) col)->varattno;
		j->idxop[j->nidx] = opno;
		j->idxcoll[j->nidx] = coll;
		j->idxany[j->nidx] = any;
		j->idxval[j->nidx++] = val;
		taken = lappend(taken, q);
		bound[j->nidx - 1] = !op_mergejoinable(opno, exprType(col));
		bounded |= bound[j->nidx - 1];
	}
	/* the two bounds of a range keep a small share together, not each: they are judged as a whole, with the other conditions of the index */
	if (bounded && clauselist_selectivity(c->root, taken, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
	{
		int			n = 0;

		for (int k = 0; k < j->nidx; k++)
			if (!bound[k])
			{
				j->idxatt[n] = j->idxatt[k];
				j->idxop[n] = j->idxop[k];
				j->idxcoll[n] = j->idxcoll[k];
				j->idxany[n] = j->idxany[k];
				j->idxval[n++] = j->idxval[k];
			}
		j->nidx = n;
	}
}
