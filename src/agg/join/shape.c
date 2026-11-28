/*
 * agg/join/shape.c - the dimensions of a join the planner of the other joins (agg/join/plan.c) reads: the relations each inner, left, RIGHT, FULL, semi or anti join adds (agg/join/shape.h).
 */
#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/cost/card.h"
#include "agg/join/shape.h"
#include "agg/plan/int.h"

bool
sift_v6_join_add_dim(SiftV6JoinShape *sh, Relids rels, SiftV6JoinMode mode)
{
	if (sh->nd == SIFT_STAR_MAX_DIMS)
		return false;
	for (int i = 0; i < sh->nd; i++)
		if (bms_overlap(sh->dim[i].rels, rels))
			return false;
	sh->dim[sh->nd++] = (SiftV6JoinDim) {.rels = rels,.mode = mode};
	return true;
}

static bool
single_in(Relids r, Relids rest)
{
	return bms_membership(r) == BMS_SINGLETON && bms_is_subset(r, rest);
}

/* the relations a RIGHT or FULL join keeps for the fact table: one of them, or all of them (a join of them is then the dimension) */
static bool
kept_in(Relids l, Relids rest)
{
	return single_in(l, rest) || bms_equal(l, rest);
}

/* a dimension a RIGHT or FULL join keeps; closing: of the join of all the other relations, the last join of the statement */
static bool
preserved_dim(SiftV6JoinShape *sh, Relids rels, SiftV6JoinMode mode, const SpecialJoinInfo *sj, bool closing)
{
	if (!sift_v6_join_add_dim(sh, rels, mode))
		return false;
	sh->dim[sh->nd - 1].closing = closing;
	sh->dim[sh->nd - 1].ojrelid = sj->ojrelid;
	return true;
}

/* the special join of the statement as a dimension: a left join to a relation, the relation a RIGHT or FULL join keeps, the relations under a semi or anti join; false for any other */
static bool
special_dim(PlannerInfo *root, SiftV6JoinShape *sh, const SpecialJoinInfo *sj, Relids rest)
{
	Relids		fact = bms_make_singleton(sh->fact->relid);
	Relids		l = sj->min_lefthand,
				r = sj->min_righthand;
	Relids		before = bms_add_members(bms_copy(rest), fact);	/* the relations of the statement: a FULL join of a dimension is the last join, of all the others */

	switch (sj->jointype)
	{
		case JOIN_LEFT:
			if (single_in(r, rest))
				return sift_v6_join_add_dim(sh, r, SV6J_LEFT_EQUI);
			if (single_in(l, rest) && bms_equal(bms_intersect(sj->syn_righthand, before), bms_del_members(bms_copy(before), l)))
				return preserved_dim(sh, l, SV6J_RIGHT_EQUI, sj, true);
			return bms_equal(r, fact) && kept_in(l, rest) && preserved_dim(sh, l, SV6J_RIGHT_EQUI, sj, false);
		case JOIN_FULL:
			if (single_in(r, rest) && bms_equal(bms_intersect(sj->syn_lefthand, before), bms_del_members(bms_copy(before), r)))
				return preserved_dim(sh, r, SV6J_FULL_EQUI, sj, true);
			return bms_equal(r, fact) && kept_in(l, rest) && preserved_dim(sh, l, SV6J_FULL_EQUI, sj, false);
		case JOIN_SEMI:
		case JOIN_ANTI:
			return bms_is_subset(bms_difference(r, root->outer_join_rels), rest) && sift_v6_join_add_dim(sh, r, sj->jointype == JOIN_SEMI ? SV6J_SEMI_EQUI : SV6J_ANTI_EQUI);
		default:
			return false;
	}
}

/* a join under the right side of a semi or anti join: the plan of that dimension makes it */
static bool
under_semi(const SpecialJoinInfo *sj, const List *joins)
{
	Relids		rels = bms_union(sj->min_lefthand, sj->min_righthand);

	foreach_node(SpecialJoinInfo, o, joins)
		if (o != sj && (o->jointype == JOIN_SEMI || o->jointype == JOIN_ANTI) && bms_is_subset(rels, o->min_righthand))
			return true;
	return false;
}

bool
sift_v6_join_make_dims(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	Relids		rest = bms_del_member(bms_copy(base), sh->fact->relid);
	SiftV6JoinDim		inner[SIFT_STAR_MAX_DIMS];
	int			ni = 0,
				r = -1;

	if (sh->elem != NULL)
		rest = bms_del_member(rest, sh->elem->relid);
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		if (!bms_is_empty(sj->commute_above_r) || !bms_is_empty(sj->commute_below_r))
			return false;
		if (sh->elem != NULL && sj->jointype == JOIN_LEFT && bms_equal(sj->min_righthand, bms_make_singleton(sh->elem->relid)))
		{
			if (sh->elem_left)
				return false;
			sh->elem_left = true;
		}
		else if (!under_semi(sj, root->join_info_list) && !special_dim(root, sh, sj, rest))
			return false;
	}
	for (int i = 0; i < sh->nd; i++)
		rest = bms_del_members(rest, sh->dim[i].rels);
	while ((r = bms_next_member(rest, r)) >= 0)
	{
		if (sh->nd + ni >= SIFT_STAR_MAX_DIMS)
			return false;
		inner[ni++] = (SiftV6JoinDim) {.rels = bms_make_singleton(r),.mode = SV6J_INNER_EQUI};
	}
	memmove(&sh->dim[ni], &sh->dim[0], sh->nd * sizeof(SiftV6JoinDim));
	memcpy(&sh->dim[0], inner, ni * sizeof(SiftV6JoinDim));
	sh->nd += ni;
	return sh->nd > 0 || sh->elem != NULL;
}

double
sift_v6_join_guessed_keys(PlannerInfo *root, const SiftV6JoinShape *sh, double input_rows)
{
	AttrNumber	atts[SIFT_STAR_MAX_DIMS];
	RelOptInfo *rels[SIFT_STAR_MAX_DIMS];

	for (int d = 0; d < sh->nd; d++)
	{
		const SiftV6JoinDim *jd = &sh->dim[d];
		const Var  *v = jd->nkeys == 1 && jd->kexpr[0] == NULL ? sift_v6_plain_var(jd->probe[0]) : NULL;

		atts[d] = (jd->mode == SV6J_INNER_EQUI || jd->mode == SV6J_SEMI_EQUI) && bms_membership(jd->rels) == BMS_SINGLETON && v != NULL && v->varno == sh->fact->relid ? v->varattno : 0;
		rels[d] = jd->rel;
	}
	return sift_v6_lookups_guess(root, sh->fact, atts, rels, sh->nd, input_rows);
}
