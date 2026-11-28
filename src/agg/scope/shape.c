/*
 * agg/scope/shape.c - why a join of more than two relations that holds a scope relation of an index of a table it aggregates reads no scope summary (sift.explain_why, arm scope): the
 * planner of the scope takes the facts and the pairs alone, joined once.
 */
#include "postgres.h"

#include "access/genam.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"

#include "agg/scope/scope.h"
#include "core/catalog.h"

const char *const sift_v6_scope_why_subselect =
	"a sub-select reads the scope relation and is no select of it alone: the relation, a view or a sub-select that projects its columns, and the union of the two paths of a scope, are read "
	"from the units";

/* The paths of the scopes of the sift_col indexes of the relations */
static List *
scope_legs(PlannerInfo *root, Relids rels)
{
	const Oid	col_am = sift_catalog()->col_am;
	List	   *legs = NIL;
	int			r = -1;

	while ((r = bms_next_member(rels, r)) >= 0)
		foreach_node(IndexOptInfo, ix, root->simple_rel_array[r]->indexlist)
		{
			Relation	index;
			const SiftScopeSpecs *specs;

			if (ix->relam != col_am)
				continue;
			index = index_open(ix->indexoid, AccessShareLock);
			specs = sift_scope_specs(index);
			for (int ord = 0; specs != NULL && ord < specs->n; ord++)
			{
				SiftScopeLeg l[2];
				const int	n = sift_scope_legs(index, ord, l);

				for (int i = 0; i < n; i++)
					legs = lappend(legs, memcpy(palloc(sizeof(SiftScopeLeg)), &l[i], sizeof(SiftScopeLeg)));
			}
			index_close(index, AccessShareLock);
		}
	return legs;
}

/* The path whose relation (or joined relation) base relation r is; NULL: r is no scope relation */
static const SiftScopeLeg *
leg_of(PlannerInfo *root, List *legs, int r)
{
	const RangeTblEntry *rte = planner_rt_fetch(r, root);

	if (rte->rtekind != RTE_RELATION)
		return NULL;
	foreach_ptr(SiftScopeLeg, l, legs)
		if (rte->relid == l->rel || rte->relid == l->via_rel)
			return l;
	return NULL;
}

static Relids
scope_rels(PlannerInfo *root, List *legs, Relids rels)
{
	Relids		out = NULL;
	int			r = -1;

	while ((r = bms_next_member(rels, r)) >= 0)
		if (leg_of(root, legs, r) != NULL)
			out = bms_add_member(out, r);
	return out;
}

/* Is the scope key of relation s joined to a column of another relation: a table of keys, which may hold a key more than once */
static bool
key_joined(PlannerInfo *root, const SiftScopeLeg *leg, int s)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		bool		key = false,
					other = false;

		if (ec->ec_has_const || ec->ec_has_volatile)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = (const Var *) sift_v6_strip_relabel((Node *) em->em_expr);

			if (!IsA(v, Var) || v->varlevelsup != 0)
				continue;
			key |= v->varno == s && v->varattno == leg->key;
			other |= v->varno != s;
		}
		if (key && other)
			return true;
	}
	return false;
}

const char *
sift_v6_scope_shape_why(PlannerInfo *root, Relids rels)
{
	List	   *legs = scope_legs(root, rels);
	const Relids scoped = scope_rels(root, legs, rels);
	int			s = -1;

	if (bms_is_empty(scoped))
		return NULL;
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		const Relids rhs = bms_intersect(sj->syn_righthand, root->all_baserels);

		if (sj->jointype != JOIN_SEMI || bms_num_members(rhs) < 2 || !bms_overlap(rhs, scoped))
			continue;
		if (bms_is_subset(rhs, scoped))
			return sift_v6_scope_why_subselect;
		return "the scope relation is a table inside a join in the sub-select of a semi join: only a semi join to the scope relation itself is read from the units";
	}
	if (bms_num_members(scoped) > 1)
		return "a scan reads one scope relation: another join to a scope relation stays a join";
	s = bms_singleton_member(scoped);
	if (planner_rt_fetch(s, root)->relid == leg_of(root, legs, s)->rel && key_joined(root, leg_of(root, legs, s), s))
		return "the table of keys may hold a key more than once, and the join counts a row for each of them";
	return NULL;
}
