/*
 * agg/v6/v6_exists_plan.c - an EXISTS over the elements of an array as elements, dimensions and conditions of the join of the fact table (agg/v6/v6_exists_plan.h).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"

#include "agg/agg.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_exists_plan.h"
#include "agg/v6/v6_lateral_plan.h"

typedef struct Exists
{
	PlannerInfo *root;
	PlannerInfo *sub;
	const SubPlan *sp;
	Index	   *map;			/* the relation of the statement each relation of the sub-select becomes (0: none) */
	bool		ok;
} Exists;

bool
sift_v6_exists_clause(const RestrictInfo *ri)
{
	return IsA(ri->clause, SubPlan) && ((const SubPlan *) ri->clause)->subLinkType == EXISTS_SUBLINK;
}

/* a sub-select of relations and conditions only: no grouping, limit, set operation or sub-select, no volatile call, and no equality of two of its columns that a class of equal columns leaves unsaid */
static bool
plain_sub(const PlannerInfo *sub)
{
	const Query *q = sub->parse;

	if (q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->hasForUpdate || q->groupClause != NIL || q->havingQual != NULL || q->limitCount != NULL ||
		q->limitOffset != NULL || q->setOperations != NULL || q->cteList != NIL || q->rowMarks != NIL || q->distinctClause != NIL || sub->placeholder_list != NIL)
		return false;
	foreach_node(EquivalenceClass, ec, sub->eq_classes)
		if (!ec->ec_has_const && list_length(ec->ec_members) > 1)
			return false;
	return !contain_volatile_functions((Node *) q);
}

/* the function scan of the sub-select over a column of the fact table, which the sub-plan passes it as its argument */
static bool
elem_function(const Exists *x, const RelOptInfo *elem, const RelOptInfo *fact, SiftV6ElemRte *out)
{
	const RangeTblEntry *rte = planner_rt_fetch(elem->relid, x->sub);
	const RangeTblFunction *rtf;
	const FuncExpr *fe;
	int			k = 0;

	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 || elem->lateral_relids != NULL)
		return false;
	rtf = linitial(rte->functions);
	fe = (const FuncExpr *) rtf->funcexpr;
	if (!IsA(rtf->funcexpr, FuncExpr) || list_length(fe->args) != 1 || !IsA(linitial(fe->args), Param))
		return false;
	foreach_int(id, x->sp->parParam)
	{
		const Var  *v = list_nth(x->sp->args, k++);
		const Param *pr = linitial(fe->args);

		if (pr->paramkind == PARAM_EXEC && pr->paramid == id)
			return IsA(v, Var) && v->varno == fact->relid && v->varlevelsup == 0 && sift_v6_unnest_rte_function(rtf, (const Expr *) v, out);
	}
	return false;
}

/* the relations of the sub-select: its one function scan, and the lateral sub-selects each a lookup of one row (lateral, in the order of their numbers) */
static bool
sub_rels(const PlannerInfo *sub, RelOptInfo **elem, List **lateral)
{
	for (int r = 1; r < sub->simple_rel_array_size; r++)
	{
		RelOptInfo *rel = sub->simple_rel_array[r];

		if (rel == NULL || rel->reloptkind != RELOPT_BASEREL)
			continue;
		if (rel->rtekind == RTE_FUNCTION && *elem == NULL)
			*elem = rel;
		else if (rel->rtekind == RTE_SUBQUERY && rel->lateral_relids != NULL)
			*lateral = lappend(*lateral, rel);
		else
			return false;
	}
	return *elem != NULL;
}

/* the left joins of the sub-select are those of its lateral sub-selects */
static bool
left_joins(const PlannerInfo *sub, bool *left)
{
	foreach_node(SpecialJoinInfo, sj, sub->join_info_list)
	{
		if (sj->jointype != JOIN_LEFT || bms_membership(sj->min_righthand) != BMS_SINGLETON || !bms_is_empty(sj->commute_above_l) || !bms_is_empty(sj->commute_above_r) ||
			!bms_is_empty(sj->commute_below_l) || !bms_is_empty(sj->commute_below_r))
			return false;
		left[bms_singleton_member(sj->min_righthand)] = true;
	}
	return true;
}

/* a new relation of the range table of the statement */
static Index
add_rte(PlannerInfo *root, RangeTblEntry *rte)
{
	expand_planner_arrays(root, 1);
	root->parse->rtable = lappend(root->parse->rtable, rte);
	root->simple_rte_array[list_length(root->parse->rtable)] = rte;
	return list_length(root->parse->rtable);
}

static RangeTblEntry *
dimension_rte(const RangeTblEntry *rte)
{
	RangeTblEntry *copy = makeNode(RangeTblEntry);

	*copy = *rte;
	copy->eref = copyObject(rte->eref);
	copy->subquery = NULL;
	copy->lateral = false;
	return copy;
}

static RelOptInfo *
element_rel(PlannerInfo *root, const char *alias)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RelOptInfo *rel = makeNode(RelOptInfo);

	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias(alias, NIL);
	rte->inFromCl = true;
	rel->reloptkind = RELOPT_BASEREL;
	rel->relid = add_rte(root, rte);
	rel->relids = bms_make_singleton(rel->relid);
	rel->rtekind = RTE_RESULT;
	rel->reltarget = create_empty_pathtarget();
	return rel;
}

/* an expression of the sub-select in the terms of the statement: its columns are the relations they became, the arguments its sub-plan passes are the expressions they stand for */
static Node *
to_root(Node *n, Exists *x)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		x->ok &= v->varlevelsup == 0 && v->varno < (Index) x->sub->simple_rel_array_size && x->map[v->varno] != 0;
		if (x->ok)
		{
			v->varno = v->varnosyn = x->map[v->varno];
			v->varnullingrels = NULL;
		}
		return (Node *) v;
	}
	if (IsA(n, Param))
	{
		const Param *pr = (const Param *) n;
		int			k = 0;

		foreach_int(id, x->sp->parParam)
		{
			if (pr->paramkind == PARAM_EXEC && pr->paramid == id)
				return copyObject(list_nth(x->sp->args, k));
			k++;
		}
		x->ok = false;
		return n;
	}
	if (IsA(n, SubPlan) || IsA(n, AlternativeSubPlan) || IsA(n, PlaceHolderVar))
	{
		x->ok = false;
		return n;
	}
	return expression_tree_mutator(n, to_root, x);
}

static RestrictInfo *
in_root(Exists *x, const RestrictInfo *ri)
{
	Node	   *clause = to_root((Node *) ri->clause, x);
	RestrictInfo *c;

	if (!x->ok || ri->has_clone || ri->is_clone)
		return NULL;
	c = makeNode(RestrictInfo);
	*c = *ri;
	c->clause = (Expr *) clause;
	return c;
}

/* the tests of the element alone are the quals of the elements; the others, with the conditions of the join of the sub-select, run on the pairs */
static bool
quals_of(Exists *x, SiftV6JoinShape *sh, const RelOptInfo *elem, List *lateral)
{
	List	   *seen = NIL;
	List	   *rels = lcons((void *) elem, list_copy(lateral));

	foreach_node(RestrictInfo, ri, elem->baserestrictinfo)
	{
		RestrictInfo *c = in_root(x, ri);

		if (c == NULL)
			return false;
		if (bms_is_subset(pull_varnos(x->root, (Node *) c->clause), sh->elem->relids))
			sh->elem->baserestrictinfo = lappend(sh->elem->baserestrictinfo, c);
		else
			sh->resid = lappend(sh->resid, c);
	}
	foreach_ptr(RelOptInfo, rel, rels)
		foreach_node(RestrictInfo, ri, rel->joininfo)
		{
			RestrictInfo *c;

			if (list_member_ptr(seen, ri))
				continue;
			seen = lappend(seen, ri);
			if (!ri->is_pushed_down || (c = in_root(x, ri)) == NULL)
				return false;
			sh->resid = lappend(sh->resid, c);
		}
	return true;
}

static bool
add_dims(Exists *x, SiftV6JoinShape *sh, List *lateral, const bool *left)
{
	const uint64 budget = Max((uint64) work_mem * 1024 / 2 / (sh->nd + list_length(lateral)), 4096);

	foreach_ptr(RelOptInfo, rel, lateral)
	{
		const Index relid = x->map[rel->relid];

		if (!sift_v6_join_add_dim(sh, bms_make_singleton(relid), left[rel->relid] ? SV6J_LEFT_EQUI : SV6J_INNER_EQUI) ||
			!sift_v6_lateral_join_dim(x->root, x->sub, x->map, rel, relid, sh, sh->nd - 1) || !sift_v6_dim_rel_ok(x->root, sh->dim[sh->nd - 1].rel, budget))
			return false;
	}
	return true;
}

static bool
extend_shape(PlannerInfo *root, SiftV6JoinShape *sh, RestrictInfo *ri)
{
	const SubPlan *sp = (const SubPlan *) ri->clause;
	Exists		x = {.root = root,.sp = sp,.ok = true};
	RelOptInfo *elem = NULL;
	List	   *lateral = NIL;
	bool	   *left;

	if (sh->elem != NULL || sp->useHashTable || sp->testexpr != NULL || list_length(sp->args) != list_length(sp->parParam) || sp->plan_id < 1)
		return false;
	x.sub = list_nth(root->glob->subroots, sp->plan_id - 1);
	left = palloc0(x.sub->simple_rel_array_size * sizeof(bool));
	x.map = palloc0(x.sub->simple_rel_array_size * sizeof(Index));
	if (!plain_sub(x.sub) || !sub_rels(x.sub, &elem, &lateral) || !elem_function(&x, elem, sh->fact, &sh->rte) || !left_joins(x.sub, left) || left[elem->relid])
		return false;
	x.map[elem->relid] = (sh->elem = element_rel(root, planner_rt_fetch(elem->relid, x.sub)->eref->aliasname))->relid;
	foreach_ptr(RelOptInfo, rel, lateral)
		x.map[rel->relid] = add_rte(root, dimension_rte(planner_rt_fetch(rel->relid, x.sub)));
	sh->semi = true;
	sh->nmain = sh->nd;
	return quals_of(&x, sh, elem, lateral) && add_dims(&x, sh, lateral, left);
}

bool
sift_v6_exists_join(PlannerInfo *root, SiftV6JoinShape *sh, RestrictInfo *ri)
{
	SiftV6JoinShape trial = *sh;

	trial.resid = list_copy(sh->resid);
	if (!extend_shape(root, &trial, ri))
		return false;
	*sh = trial;
	return true;
}

bool
sift_v6_exists_main_unique(PlannerInfo *root, const SiftV6JoinShape *sh)
{
	for (int d = 0; sh->semi && d < sh->nmain; d++)
	{
		const SiftV6JoinDim *jd = &sh->dim[d];
		int			atts[SIFT_STAR_MAX_KEYS];
		const RangeTblEntry *rte;

		if (jd->lateral || jd->mode == SV6J_SEMI_EQUI || jd->mode == SV6J_ANTI_EQUI)
			continue;
		if ((jd->mode != SV6J_INNER_EQUI && jd->mode != SV6J_LEFT_EQUI) || bms_membership(jd->rels) != BMS_SINGLETON)
			return false;
		rte = planner_rt_fetch(bms_singleton_member(jd->rels), root);
		for (int k = 0; k < jd->nkeys; k++)
		{
			if (jd->kexpr[k] != NULL)
				return false;
			atts[k] = jd->key[k]->varattno;
		}
		if (rte->rtekind != RTE_RELATION || !sift_agg_unique_key(rte->relid, jd->nkeys, 0, atts, NULL))
			return false;
	}
	return true;
}
