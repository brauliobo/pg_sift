/*
 * col/f2/f2_v6_scope_topk.c - the parts of the plan of the newest rows of a scope key (f2_v6_scope.c): the ring leaf the statement orders by, the tie leaf and the columns of the rows.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/f2/f2_v6_scope_int.h"
#include "core/store_ops.h"

/* ORDER BY leaf DESC of a leaf of the index that can be a ring, and whether the ring (NULLS LAST) serves the way the statement puts the NULLs */
bool
sift_v6_scope_topk_order(ScopePlan *p, SortGroupClause *sgc)
{
	const Var  *v = (const Var *) get_sortgroupclause_expr(sgc, p->root->parse->targetList);
	int			col = IsA(v, Var) ? sift_v6_scope_index_col(p, v) : -1;

	if (col < 1 || !sift_f2_scope_ring_eligible(p->irel, col) || get_op_opfamily_strategy(sgc->sortop, p->index->opfamily[col - 1]) != BTGreaterStrategyNumber)
		return false;
	p->spec.ring_col = col;
	p->spec.nulls_first = sgc->nulls_first;
	p->spec.leaf_not_null = bms_is_member(v->varattno, p->fact->notnullattnums);
	return true;
}

/* The second ORDER BY leaf: an integer or a time that is never NULL, in either direction: the rows the ring leaf does not tell apart are ordered by it */
bool
sift_v6_scope_topk_tie(ScopePlan *p, SortGroupClause *sgc)
{
	const Var  *v = (const Var *) get_sortgroupclause_expr(sgc, p->root->parse->targetList);
	const int	col = IsA(v, Var) ? sift_v6_scope_index_col(p, v) : -1;
	const int	strat = col > 0 ? get_op_opfamily_strategy(sgc->sortop, p->index->opfamily[col - 1]) : 0;

	if (col < 1 || !bms_is_member(v->varattno, p->fact->notnullattnums) || (strat != BTLessStrategyNumber && strat != BTGreaterStrategyNumber))
		return false;
	switch (v->vartype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			break;
		default:
			return false;
	}
	p->spec.tie_col = col;
	p->spec.tie_desc = strat == BTGreaterStrategyNumber;
	return true;
}

/* The columns of the rows are columns of the index the scan decodes as they are stored (SiftV6Spec.key of the spec that follows the scope spec in the plan) */
bool
sift_v6_scope_topk_output(ScopePlan *p, const PathTarget *target)
{
	p->v6 = (SiftV6Spec) {.relid = RelationGetRelid(p->irel),.is_table = p->index == NULL};
	p->v6.key = palloc(Max(list_length(target->exprs), 1) * sizeof(SiftV6Col));
	foreach_ptr(Expr, e, target->exprs)
	{
		const Var  *v = (const Var *) e;
		const int	col = IsA(e, Var) ? sift_v6_scope_index_col(p, v) : -1;

		if (col < 1 || p->v6.nkeys == SIFT_V6_MAX_KEYS || !sift_store_ops_for_oid(RelationGetRelid(p->irel), false)->leaf_holds(RelationGetRelid(p->irel), false, col, sift_v6_class(v->vartype)))
			return false;
		p->v6.key[p->v6.nkeys++] = (SiftV6Col) {.attno = col,.typid = v->vartype};
		p->v6_tlist = lappend(p->v6_tlist, makeTargetEntry(copyObject(e), list_length(p->v6_tlist) + 1, NULL, false));
	}
	return true;
}
