/*
 * agg/two/spine.c - the input of the outer aggregate of a two-level aggregate (agg/two/two.h): the FROM item of the arms alone, or joined to a spine (a VALUES list, a series, a
 * small table) by an inner join or a LEFT JOIN that keeps the spine's rows, on equalities of an expression of the arms and one of the spine (=, IS NOT DISTINCT FROM). The node
 * hashes the spine's rows on their side of the equalities and adds each cell to the groups of every spine row it equals and the other conditions of the join hold for
 * (agg/two/hash.h); the conditions of the WHERE above a LEFT JOIN are tested on each joined row, the rows of the spine no cell matched included.
 */
/* check-memory: allow-allocs the lists follow the equalities of one join */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/plan/int.h"
#include "agg/two/plan_int.h"

int
sift_two_side(const TwoPlan *t, Node *e)
{
	int			side = -1;

	foreach_node(Var, v, pull_var_clause(e, PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS))
	{
		const int	s = v->varlevelsup != 0 || v->varattno <= 0 ? -2 : v->varno == t->srti ? 0 : t->spine.rel != NULL && v->varno == t->spine.rel->relid ? 1 : -2;

		if (s == -2 || (side >= 0 && s != side))
			return -1;
		side = s;
	}
	return side < 0 ? 0 : side;
}

/* a sub-select the outer statement reads alone: the FROM item of arms */
static bool
arms_rel(PlannerInfo *root, RelOptInfo *rel)
{
	return rel->reloptkind == RELOPT_BASEREL && planner_rt_fetch(rel->relid, root)->rtekind == RTE_SUBQUERY && rel->lateral_relids == NULL && !IS_DUMMY_REL(rel);
}

/* an equality of the join: `cell = spine` or `cell IS NOT DISTINCT FROM spine` (either way round) of an operator that hashes, no volatile function */
static bool
spine_pair(TwoPlan *t, const RestrictInfo *ri)
{
	Node	   *c = (Node *) ri->clause;
	bool		null_safe = is_notclause(c) && IsA(get_notclausearg((Expr *) c), DistinctExpr);
	const OpExpr *op = null_safe ? (const OpExpr *) get_notclausearg((Expr *) c) : IsA(c, OpExpr) ? (const OpExpr *) c : NULL;
	Node	   *l,
			   *r;
	int			ls;

	if (op == NULL || list_length(op->args) != 2 || contain_volatile_functions(c) || contain_subplans(c) || !op_hashjoinable(op->opno, exprType(linitial(op->args))))
		return false;
	l = sift_two_phvs(linitial(op->args), t);
	r = sift_two_phvs(lsecond(op->args), t);
	if (sift_two_has_phv(l, NULL) || sift_two_has_phv(r, NULL))
		return false;
	ls = sift_two_side(t, l);
	if (ls < 0 || ls == sift_two_side(t, r) || sift_two_side(t, r) < 0 || pull_var_clause(l, PVC_RECURSE_PLACEHOLDERS) == NIL || pull_var_clause(r, PVC_RECURSE_PLACEHOLDERS) == NIL)
		return false;
	t->spine.cell_keys = lappend(t->spine.cell_keys, ls == 0 ? l : r);
	t->spine.spine_keys = lappend(t->spine.spine_keys, ls == 0 ? r : l);
	t->spine.ops = lappend_oid(t->spine.ops, ls == 0 ? op->opno : get_commutator(op->opno));
	t->spine.colls = lappend_oid(t->spine.colls, op->inputcollid);
	t->spine.null_safe = lappend_int(t->spine.null_safe, null_safe);
	return OidIsValid(llast_oid(t->spine.ops));
}

/* a join path of the rel, under the projection of the scan/join target */
static const JoinPath *
join_path(const RelOptInfo *rel)
{
	foreach_ptr(Path, p, rel->pathlist)
	{
		const Path *q = p;

		while (IsA(q, ProjectionPath))
			q = ((const ProjectionPath *) q)->subpath;
		if (IsA(q, NestPath) || IsA(q, MergePath) || IsA(q, HashPath))
			return (const JoinPath *) q;
	}
	return NULL;
}

/* spine JOIN arms, or spine LEFT JOIN arms (the spine on the preserved side of a left or of a right join) */
static bool
spine_join(TwoPlan *t, const JoinPath *jp)
{
	const bool	spine_outer = bms_equal(jp->outerjoinpath->parent->relids, t->spine.rel->relids);

	t->spine.left = jp->jointype != JOIN_INNER;
	if (!(jp->jointype == JOIN_INNER || (jp->jointype == JOIN_LEFT && spine_outer) || (jp->jointype == JOIN_RIGHT && !spine_outer)))
		return false;
	foreach_node(RestrictInfo, ri, jp->joinrestrictinfo)
	{
		const bool	post = t->spine.left && ri->is_pushed_down;
		Node	   *e;

		if (!post && spine_pair(t, ri))
			continue;
		e = sift_two_phvs((Node *) ri->clause, t);
		if (!sift_two_cell_expr(t, e))
			return false;
		if (post)
			t->spine.post_quals = lappend(t->spine.post_quals, e);
		else
			t->spine.match_quals = lappend(t->spine.match_quals, e);
	}
	return t->spine.cell_keys != NIL;
}

bool
sift_two_from(TwoPlan *t)
{
	PlannerInfo *root = t->root;
	const JoinPath *jp = NULL;
	Relids		base;
	int			i = -1;

	if (t->input->reloptkind == RELOPT_BASEREL)
		t->rel = arms_rel(root, t->input) ? t->input : NULL;
	else if (t->input->reloptkind == RELOPT_JOINREL && (jp = join_path(t->input)) != NULL && bms_num_members(base = sift_v6_plan_base_relids(root, t->input)) == 2)
		while ((i = bms_next_member(base, i)) >= 0)
		{
			RelOptInfo *rel = find_base_rel(root, i);

			if (t->rel == NULL && arms_rel(root, rel) && (planner_rt_fetch(i, root)->inh || planner_rt_fetch(i, root)->subquery->hasAggs))
				t->rel = rel;
			else
				t->spine.rel = rel;
		}
	if (t->rel == NULL)
		return false;
	t->srti = t->rel->relid;
	if (t->spine.rel == NULL)
		return t->input->reloptkind == RELOPT_BASEREL;
	t->spine.path = t->spine.rel->cheapest_total_path;
	return t->spine.path != NULL && t->spine.path->param_info == NULL && t->spine.rel->lateral_relids == NULL &&
		t->spine.path->rows * (t->spine.path->pathtarget->width + 64) <= (double) work_mem * 1024 && spine_join(t, jp);
}
