/*
 * agg/plan_ord.c - paths whose groups come in the order the parent sorts by
 *
 * PostgreSQL gives a path no hint of the order its parent wants: it takes the paths of the upper relation with their pathkeys and puts a Sort where one
 * is missing. So the node offers, besides its path, a copy that emits its groups sorted (agg/gorder.c) by the longest prefix of the pathkeys of the
 * query's ORDER BY or window that are keys of the group, and the parent uses it when it is cheaper than the Sort it would add.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_clause.h"
#include "optimizer/tlist.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"
#include "agg/spec.h"
#include "core/compat.h"
#include "core/limits.h"

/* what one integer compare of the rank tuples costs, in operator costs: no function call */
#define ORD_RANK_CMP	0.1

static Node *
key_relabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* the group key (position in keys) a pathkey is on, with its sort operator; -1: it is none of them */
static int
pathkey_key(PathKey *pk, List *keys, Oid *sortop)
{
	EquivalenceClass *ec = pk->pk_eclass;

	foreach_node(EquivalenceMember, em, ec->ec_members)
	{
		Node	   *e = key_relabel((Node *) em->em_expr);
		int			j = 0;

		if (em->em_is_child || em->em_is_const)
			continue;
		foreach_ptr(Node, k, keys)
		{
			if (equal(e, key_relabel(k)))
			{
				*sortop = sift_pathkey_sortop(pk, em->em_datatype);
				return OidIsValid(*sortop) ? j : -1;
			}
			j++;
		}
	}
	return -1;
}

/* the ranks of the distinct values of each key, and the sort of the groups by them */
static Cost
order_cost(AggCtx *c, const SiftAggSpec *s, double groups)
{
	double		n = Max(groups, 2.0);
	Cost		cost = n * log2(n) * ORD_RANK_CMP * cpu_operator_cost * s->nord;

	for (int o = 0; o < s->nord; o++)
	{
		const SiftAggKey *k = &s->key[s->ord[o].key];
		/* no statistics of the key: the distinct values grow slower than the groups */
		double		d = k->kind == SAK_LEAF && c->stats.ndistinct[k->leaf] > 0 ? Min(c->stats.ndistinct[k->leaf], n) : pow(n, 0.6);

		cost += Max(d, 2.0) * log2(Max(d, 2.0)) * 2.0 * cpu_operator_cost;
	}
	return cost;
}

/* the node's paths with the groups sorted by ord[0..n), promised as `pathkeys` (the pathkeys of this query level) */
static void
ordered_add(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist,
			SiftAggOrd *ord, int n, List *pathkeys, int nworkers, List **seen)
{
	if (n == 0 || list_member(*seen, pathkeys))
		return;
	*seen = lappend(*seen, pathkeys);
	s->nord = n;
	/* the paths keep the spec, which is packed when the plan is made: the array outlives this call (the caller's is on its stack) */
	s->ord = memcpy(palloc(n * sizeof(SiftAggOrd)), ord, n * sizeof(SiftAggOrd));
	add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, 0, pathkeys);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, nworkers, pathkeys);
	s->nord = 0;
	s->ord = NULL;
}

/* the longest prefix of `pathkeys` (of this query level) whose keys are keys of the group */
static void
ordered_prefix(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist,
			   List *keys, List *pathkeys, int nworkers, List **seen)
{
	List	   *prefix = NIL;
	SiftAggOrd	ord[SIFT_AGG_MAX_KEYS];
	int			n = 0;

	foreach_node(PathKey, pk, pathkeys)
	{
		Oid			sortop;
		int			j = pathkey_key(pk, keys, &sortop);

		if (j < 0 || n >= SIFT_AGG_MAX_KEYS)
			break;
		ord[n].key = j;
		ord[n].sortop = sortop;
		ord[n].collation = pk->pk_eclass->ec_collation;
		ord[n].nulls_first = pk->pk_nulls_first;
		ord[n].subst = NULL;
		n++;
		prefix = lappend(prefix, pk);
	}
	ordered_add(c, s, input_rel, output_rel, target, scan_tlist, ord, n, prefix, nworkers, seen);
}

/* the subquery entry of the parent query that this level is (the one not planned yet with this group clause); 0: none */
static Index
parent_entry(PlannerInfo *root)
{
	PlannerInfo *parent = root->parent_root;

	for (Index i = 1; i < (Index) parent->simple_rel_array_size; i++)
	{
		RangeTblEntry *rte = parent->simple_rte_array[i];
		RelOptInfo *rel = parent->simple_rel_array[i];

		if (rte != NULL && rel != NULL && rte->rtekind == RTE_SUBQUERY && rel->subroot == NULL &&
			rte->subquery->groupClause != NIL && equal(rte->subquery->groupClause, root->parse->groupClause) &&
			list_length(rte->subquery->targetList) == list_length(root->parse->targetList))
			return i;
	}
	return 0;
}

/*
 * The order the parent query asks for, when this level is a sub-select or an inlined CTE that it reads: its pathkeys name the columns of the sub-select as Vars of the
 * entry, which are target entries of this query and so group keys. The pathkeys the node promises are made from the group clause of this level.
 */
static void
ordered_parent(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist,
			   List *keys, int nworkers, List **seen)
{
	Query	   *parse = root->parse,
			   *parent;
	Index		rti;
	List	   *sgcs = NIL;
	SiftAggOrd	ord[SIFT_AGG_MAX_KEYS];
	int			n = 0,
				npk = 0;
	bool		truthful = true;

	if (root->parent_root == NULL || root->parent_root->query_pathkeys == NIL)
		return;
	/* a window or an ORDER BY of the parent: not its grouping or DISTINCT, whose plan the order would only turn from a hash to a sort */
	parent = root->parent_root->parse;
	if (parent->groupClause != NIL || parent->hasAggs || parent->groupingSets != NIL || parent->distinctClause != NIL || (rti = parent_entry(root)) == 0)
		return;
	foreach_node(PathKey, pk, root->parent_root->query_pathkeys)
	{
		AttrNumber	attno = 0;
		bool		constant = false;
		TargetEntry *tle;
		SortGroupClause *sgc = NULL;
		Node	   *e;
		int			j = 0;
		bool		found = false;
		Oid			sortop;
		Const	   *subst;

		/* the column of this sub-select the pathkey is on: the entry's Var, or for an arm of a UNION ALL the member the class got for the arm */
		foreach_node(EquivalenceMember, em, pk->pk_eclass->ec_members)
		{
			Node	   *x = key_relabel((Node *) em->em_expr);

			if (IsA(x, Var) && ((Var *) x)->varno == rti && ((Var *) x)->varlevelsup == 0)
				attno = ((Var *) x)->varattno;
			else if (em->em_is_child && IsA(x, Const) && bms_is_member(rti, em->em_relids))
				constant = true;
		}
		if (attno == 0 && constant)
		{
			npk++;
			continue;
		}
		if (attno <= 0 || attno > list_length(parse->targetList))
			break;
		tle = list_nth_node(TargetEntry, parse->targetList, attno - 1);
		/* a column the sub-select gives as a constant is in any order */
		if (!tle->resjunk && IsA(key_relabel((Node *) tle->expr), Const))
		{
			npk++;
			continue;
		}
		if (tle->resjunk)
			break;
		/* COALESCE(key, constant): the NULL group sorts as the constant */
		subst = NULL;
		if (IsA(key_relabel((Node *) tle->expr), CoalesceExpr))
		{
			CoalesceExpr *ce = (CoalesceExpr *) key_relabel((Node *) tle->expr);

			if (list_length(ce->args) == 2 && IsA(lsecond(ce->args), Const) && !((Const *) lsecond(ce->args))->constisnull)
			{
				subst = (Const *) lsecond(ce->args);
				e = key_relabel((Node *) linitial(ce->args));
			}
			else
				break;
		}
		else if (tle->ressortgroupref != 0 && (sgc = get_sortgroupref_clause_noerr(tle->ressortgroupref, parse->groupClause)) != NULL)
			e = key_relabel((Node *) get_sortgroupclause_expr(sgc, root->processed_tlist));
		else
			break;
		foreach_ptr(Node, k, keys)
		{
			if (equal(e, key_relabel(k)))
			{
				found = true;
				break;
			}
			j++;
		}
		sortop = sift_pathkey_sortop(pk, exprType((Node *) tle->expr));
		if (!found || !OidIsValid(sortop) || n >= SIFT_AGG_MAX_KEYS)
			break;
		ord[n].key = j;
		ord[n].sortop = sortop;
		ord[n].collation = pk->pk_eclass->ec_collation;
		ord[n].nulls_first = pk->pk_nulls_first;
		ord[n].subst = subst;
		n++;
		npk++;
		/* the pathkeys of this level are those of the keys themselves: up to the first key sorted as something else */
		if (subst != NULL)
			truthful = false;
		if (truthful)
		{
			SortGroupClause *copy = copyObject(sgc);

			copy->sortop = sortop;
			sift_sortclause_reverse(copy, pk);
			copy->nulls_first = pk->pk_nulls_first;
			sgcs = lappend(sgcs, copy);
		}
	}
	if (n > 0 && sgcs != NIL)
	{
		List	   *mine = make_pathkeys_for_sortclauses(root, sgcs, root->processed_tlist);

		/* the pathkeys of this level, each on its own key: a redundant one (a key equal to a constant) would shift the keys */
		if (mine != NIL && list_length(mine) == list_length(sgcs))
		{
			/* the parent's pathkeys, the constant columns among them: the node is ordered by them though this level can not say so */
			c->parent_pathkeys = list_copy_head(root->parent_root->query_pathkeys, npk);
			c->parent_rti = rti;
			ordered_add(c, s, input_rel, output_rel, target, scan_tlist, ord, n, mine, nworkers, seen);
			c->parent_pathkeys = NIL;
			c->parent_rti = 0;
		}
	}
}

Cost
agg_order_cost(AggCtx *c, const SiftAggSpec *s, double groups)
{
	return s->nord > 0 ? order_cost(c, s, groups) : 0;
}

/* called with the paths of the scan added: the same groups, sorted for the ORDER BY or the window of the query */
void
agg_ordered_paths(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist,
				  List *keys, double table, int nworkers)
{
	Query	   *parse = root->parse;
	List	   *seen = NIL;

	/* the groups come out of a table that stays in memory (a spilling scan emits them by partition), and a node that keeps only some of them has nothing to sort */
	if (s->nsets > 0 || s->topn != NULL || s->gtopn_k > 0 || parse->hasDistinctOn || s->nkeys == 0 || list_length(keys) != s->nkeys - s->nhidden ||
		table > (double) get_hash_memory_limit())
		return;
	if (root->sort_pathkeys != NIL)
		ordered_prefix(c, s, input_rel, output_rel, target, scan_tlist, keys, root->sort_pathkeys, nworkers, &seen);
	if (root->window_pathkeys != NIL)
		ordered_prefix(c, s, input_rel, output_rel, target, scan_tlist, keys, root->window_pathkeys, nworkers, &seen);
	ordered_parent(c, s, root, input_rel, output_rel, target, scan_tlist, keys, nworkers, &seen);
}

/*
 * A sub-select the node gives its groups in the order the parent asks for (the pathkeys the node promises are those of its own level, without the columns it gives as
 * constants, which PostgreSQL drops from them): the scan of the sub-select is offered with the parent's pathkeys.
 */
void
agg_subquery_ordered(PlannerInfo *root, RelOptInfo *rel, Index rti)
{
	List	   *more = NIL;

	foreach_ptr(Path, p, rel->pathlist)
	{
		SubqueryScanPath *sp;
		SubqueryScanPath *np;
		SiftAggPath *ap;

		if (!IsA(p, SubqueryScanPath))
			continue;
		sp = (SubqueryScanPath *) p;
		if (!IsA(sp->subpath, CustomPath) || ((CustomPath *) sp->subpath)->methods != &agg_path_methods)
			continue;
		ap = (SiftAggPath *) sp->subpath;
		if (ap->parent_pathkeys == NIL || ap->parent_rti != rti || pathkeys_contained_in(ap->parent_pathkeys, p->pathkeys))
			continue;
		np = makeNode(SubqueryScanPath);
		memcpy(np, sp, sizeof(SubqueryScanPath));
		np->path.pathkeys = ap->parent_pathkeys;
		more = lappend(more, np);
	}
	foreach_ptr(Path, p, more)
		add_path(rel, p);
}
