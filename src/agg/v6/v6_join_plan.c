/*
 * agg/v6/v6_join_plan.c - the planner of the joins of a fact table that need a lookup of the node: a relation with a key more than once, an outer, semi or anti join, a RIGHT or FULL join, a
 * key that is the element of an unnest or a column of an earlier relation, a condition the keys do not say (agg/v6/v6_join_plan.h).
 *
 * The statement's relations are split into the fact table (the one of the index), the element of its unnest (a lateral function scan of one of its columns), and the dimensions: each relation
 * that an inner or left join adds, each side a RIGHT or FULL join preserves, and each set of relations under a semi or anti join (read as the join PostgreSQL planned of them). Every equality
 * that joins a dimension to the relations before it is a key; any other condition of the join runs on the pairs of a batch with PostgreSQL's evaluator, unless it belongs to an outer, semi or anti
 * join (its ON clause is part of the match, which no filter of the result says).
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_dim_keys.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_exists_plan.h"
#include "agg/v6/v6_join_plan.h"
#include "agg/v6/v6_source_rel.h"
#include "agg/v6/v6_join_shape.h"
#include "agg/v6/v6_lateral_plan.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_unnest_jsonb.h"

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

/* the special join of the statement as a dimension: a left join to a relation, the relation a RIGHT or FULL join keeps, the relations under a semi or anti join; false for any other */
static bool
special_dim(SiftV6JoinShape *sh, const SpecialJoinInfo *sj, Relids rest)
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
			return bms_equal(r, fact) && single_in(l, rest) && sift_v6_join_add_dim(sh, l, SV6J_RIGHT_EQUI);
		case JOIN_FULL:
			if (single_in(r, rest) && bms_equal(bms_intersect(sj->syn_lefthand, before), bms_del_members(bms_copy(before), r)))
			{
				if (!sift_v6_join_add_dim(sh, r, SV6J_FULL_EQUI))
					return false;
				sh->dim[sh->nd - 1].closing = true;
				return true;
			}
			return bms_equal(r, fact) && single_in(l, rest) && sift_v6_join_add_dim(sh, l, SV6J_FULL_EQUI);
		case JOIN_SEMI:
		case JOIN_ANTI:
			return bms_is_subset(r, rest) && sift_v6_join_add_dim(sh, r, sj->jointype == JOIN_SEMI ? SV6J_SEMI_EQUI : SV6J_ANTI_EQUI);
		default:
			return false;
	}
}

/* the dimensions in the order the pipe probes them: a join of inner relations, then the others in the order the statement joins them, each probing columns of the relations before it */
static bool
make_dims(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	Relids		rest = bms_del_member(bms_copy(base), sh->fact->relid);
	SiftV6JoinDim		inner[SIFT_STAR_MAX_DIMS];
	int			ni = 0,
				r = -1;

	if (sh->elem != NULL)
		rest = bms_del_member(rest, sh->elem->relid);
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		if (!bms_is_empty(sj->commute_above_l) || !bms_is_empty(sj->commute_above_r) || !bms_is_empty(sj->commute_below_l) || !bms_is_empty(sj->commute_below_r))
			return false;
		if (sh->elem != NULL && sj->jointype == JOIN_LEFT && bms_equal(sj->min_righthand, bms_make_singleton(sh->elem->relid)))
		{
			if (sh->elem_left)
				return false;
			sh->elem_left = true;
		}
		else if (!special_dim(sh, sj, rest))
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
	return sh->nd > 0;
}

/* the part a relation has in the join: the fact table, the element, the index of its dimension, or none */
static int
role_of_rel(const SiftV6JoinShape *sh, Index relid)
{
	if (relid == sh->fact->relid)
		return ROLE_FACT;
	if (sh->elem != NULL && relid == sh->elem->relid)
		return ROLE_ELEM;
	for (int i = 0; i < sh->nd; i++)
		if (bms_is_member(relid, sh->dim[i].rels))
			return i;
	return ROLE_NONE;
}

int
sift_v6_join_role(const SiftV6JoinShape *sh, const Var *v)
{
	return role_of_rel(sh, v->varno);
}

/* the part of the latest relation the columns of e are of; ROLE_NONE when it reads a column the join does not know, or none */
int
sift_v6_join_expr_role(const SiftV6JoinShape *sh, const Node *e)
{
	List	   *vars;
	int			role = ROLE_FACT;

	if (contain_agg_clause((Node *) e) || contain_window_function((Node *) e))
		return ROLE_NONE;
	vars = pull_var_clause((Node *) e, 0);
	if (vars == NIL)
		return ROLE_NONE;
	foreach_node(Var, v, vars)
	{
		const int	r = v->varlevelsup == 0 ? sift_v6_join_role(sh, v) : ROLE_NONE;

		if (r == ROLE_NONE)
			return ROLE_NONE;
		role = Max(role, r);
	}
	return role;
}

/* the same probe: a column by its attribute, an expression by its tree */
static bool
same_probe(const Node *a, const Node *b)
{
	if (IsA(a, Var) && IsA(b, Var))
		return ((const Var *) a)->varno == ((const Var *) b)->varno && ((const Var *) a)->varattno == ((const Var *) b)->varattno;
	return equal(a, b);
}

bool
sift_v6_join_add_key(SiftV6JoinDim *d, const Node *probe, const Var *key)
{
	for (int k = 0; k < d->nkeys; k++)
		if (same_probe(d->probe[k], probe) && d->key[k]->varno == key->varno && d->key[k]->varattno == key->varattno)
			return true;
	if (d->nkeys == SIFT_STAR_MAX_KEYS || !sift_v6_key_types_agree(exprType(probe), exprCollation(probe), key->vartype, key->varcollid))
		return false;
	d->probe[d->nkeys] = probe;
	d->key[d->nkeys++] = key;
	return true;
}

static bool
dim_rels_ok(PlannerInfo *root, SiftV6JoinShape *sh)
{
	const uint64 budget = Max((uint64) work_mem * 1024 / 2 / sh->nd, 4096);

	for (int i = 0; i < sh->nd; i++)
	{
		SiftV6JoinDim *d = &sh->dim[i];
		RelOptInfo *rel;

		if (bms_membership(d->rels) == BMS_SINGLETON)
			rel = root->simple_rel_array[bms_singleton_member(d->rels)];
		else
			rel = find_join_rel(root, d->rels);
		if (rel != NULL && rel->lateral_relids != NULL)
		{
			if (!sift_v6_lateral_join_dim(root, root, NULL, rel, rel->relid, sh, i) || !sift_v6_dim_rel_ok(root, d->rel, budget))
				return false;
		}
		else if (rel == NULL || !sift_v6_dim_rel_ok(root, rel, budget))
			return false;
		else
			d->rel = rel;
	}
	return true;
}

/* the relations are all under one dimension: its plan applies what joins them */
static bool
within_one_dim(const SiftV6JoinShape *sh, Relids rels)
{
	for (int i = 0; i < sh->nd; i++)
		if (bms_is_subset(rels, sh->dim[i].rels))
			return true;
	return false;
}

/* a class with an expression of the columns of a dimension (lower(d.t) = f.g) is not a key: its equalities run on the pairs, which is what an inner join of the relations says */
static bool
computed_class_ok(const SiftV6JoinShape *sh, Relids rels)
{
	int			r = -1;

	while ((r = bms_next_member(rels, r)) >= 0)
	{
		const int	role = role_of_rel(sh, r);

		if (role == ROLE_NONE || (role >= 0 && sh->dim[role].mode != SV6J_INNER_EQUI))
			return false;
	}
	return true;
}

/* a class of equal columns and expressions that the planner made of the inner and semi joins: the members that are dimensions are joined to the member before them, the column or expression of the fact table, of the element or of the first dimension */
static bool
equivalence_keys(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		const Node *probe = NULL;
		int			probe_role = ROLE_NONE;
		Relids		rels = NULL;
		bool		computed_dim = false;
		int			ncolumns = 0;

		if (ec->ec_has_const || ec->ec_has_volatile || ec->ec_broken)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			Relids		vr = bms_intersect(pull_varnos(root, (Node *) em->em_expr), base);
			const int	role = bms_is_empty(vr) ? ROLE_NONE : sift_v6_join_expr_role(sh, (Node *) em->em_expr);

			if (em->em_is_child)
				continue;
			rels = bms_add_members(rels, vr);
			computed_dim |= v == NULL && role >= 0;
			if (role == ROLE_NONE)
				continue;
			ncolumns += role < 0;
			if (probe == NULL || role < probe_role)
			{
				probe = v != NULL ? (const Node *) v : (const Node *) em->em_expr;
				probe_role = role;
			}
		}
		if (bms_membership(rels) != BMS_MULTIPLE || within_one_dim(sh, rels))
			continue;
		if (computed_dim || probe == NULL || ncolumns > 1 || (!IsA(probe, Var) && !sift_v6_pgexpr_plan_ok(NULL, (const Expr *) probe)))
		{
			if (!computed_class_ok(sh, rels))
				return false;
			sh->resid = list_concat_unique_ptr(sh->resid, ec->ec_sources);
			continue;
		}
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			const int	d = v != NULL && !em->em_is_child ? sift_v6_join_role(sh, v) : ROLE_NONE;

			if (d < 0 || d == probe_role)
				continue;
			if ((sh->dim[d].mode != SV6J_INNER_EQUI && sh->dim[d].mode != SV6J_SEMI_EQUI) || !sift_v6_join_add_key(&sh->dim[d], probe, v))
				return false;
		}
	}
	return true;
}

/* an inner join's clause is a condition of the WHERE level; that of an outer or anti join is its own ON clause; a semi join's is either, by how PostgreSQL pulled the sub-select up */
static bool
clause_level_ok(const RestrictInfo *ri, SiftV6JoinMode mode)
{
	return mode == SV6J_SEMI_EQUI || ri->is_pushed_down == (mode == SV6J_INNER_EQUI);
}

/* the discrete range types, whose bounds are int64 values: a date or int4 element of the fact table in a range column of a dimension is a period of the range join */
static bool
period_types(Oid elem, Oid range)
{
	return (elem == DATEOID && range == DATERANGEOID) || (elem == INT4OID && range == INT4RANGEOID);
}

/* fact.col <@ dim.range of an inner join: the dimension is the periods, probed by the column (SV6J_RANGE_SMALL); a LEFT or semi join of it, and the rows of an unnest, are the normal plan's */
static bool
place_period(SiftV6JoinShape *sh, const RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const Var  *col,
			   *range;
	SiftV6JoinDim    *d;

	if (sh->elem != NULL || !IsA(ri->clause, OpExpr) || op->opno != OID_RANGE_ELEM_CONTAINED_OP || !ri->is_pushed_down)
		return false;
	col = sift_v6_plain_var(linitial(op->args));
	range = sift_v6_plain_var(lsecond(op->args));
	if (col == NULL || range == NULL || sift_v6_join_role(sh, col) != ROLE_FACT || sift_v6_join_role(sh, range) < 0 || !period_types(col->vartype, range->vartype))
		return false;
	d = &sh->dim[sift_v6_join_role(sh, range)];
	if (d->mode != SV6J_INNER_EQUI || d->nkeys != 0)
		return false;
	d->mode = SV6J_RANGE_SMALL;
	d->probe[0] = (const Node *) col;
	d->key[0] = range;
	d->nkeys = 1;
	return true;
}

/* the equality of a column of a dimension (other) and a column or an expression of the relations before it (arg), as a key of the dimension */
static bool
place_key(SiftV6JoinShape *sh, const RestrictInfo *ri, Node *arg, Node *other)
{
	const Var  *key = sift_v6_plain_var(other);
	const Var  *column = sift_v6_plain_var(arg);
	const Node *probe = column != NULL ? (const Node *) column : arg;
	int			d;

	if (key == NULL || (d = sift_v6_join_role(sh, key)) < 0 || sift_v6_join_expr_role(sh, probe) == ROLE_NONE || sift_v6_join_expr_role(sh, probe) >= d)
		return false;
	return (column != NULL || sift_v6_pgexpr_plan_ok(NULL, (const Expr *) probe)) && clause_level_ok(ri, sh->dim[d].mode) && sift_v6_join_add_key(&sh->dim[d], probe, key);
}

/* a semi or anti join matches a row once, by its first dimension row: a condition on the pairs would decide on that one only */
static bool
matches_once(const SiftV6JoinShape *sh, Relids rels)
{
	for (int i = 0; i < sh->nd; i++)
		if ((sh->dim[i].mode == SV6J_SEMI_EQUI || sh->dim[i].mode == SV6J_ANTI_EQUI) && bms_overlap(sh->dim[i].rels, rels))
			return true;
	return false;
}

/* the dimension an ON clause belongs to: the last of the dimensions it names, which is the outer join it is the clause of; -1 when no dimension of an outer join */
static int
on_dim(const SiftV6JoinShape *sh, Relids rels)
{
	int			last = -1;

	for (int i = 0; i < sh->nd; i++)
		if (bms_overlap(sh->dim[i].rels, rels))
			last = i;
	if (last < 0 || (sh->dim[last].mode != SV6J_LEFT_EQUI && !sift_star_preserves_dim(sh->dim[last].mode)))
		return -1;
	return last;
}

/* a condition of the ON clause of a LEFT, RIGHT or FULL join that no key says: it decides which pairs match, so it runs on them before the join keeps or completes the rows */
static bool
place_on(SiftV6JoinShape *sh, RestrictInfo *ri)
{
	const int	d = on_dim(sh, ri->required_relids);

	if (d < 0)
		return false;
	sh->dim[d].on = lappend(sh->dim[d].on, ri);
	return true;
}

/* where a condition of the join goes: a key of the dimension it joins to the relations before it, a condition of the plan of one dimension, or a condition on the pairs. false: the join has no place for it */
static bool
place_clause(SiftV6JoinShape *sh, RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;

	if (ri->has_clone || ri->is_clone)
		return false;
	if (within_one_dim(sh, ri->required_relids))
		return true;
	if (place_period(sh, ri))
		return true;
	if (IsA(ri->clause, OpExpr) && list_length(op->args) == 2 && op_hashjoinable(op->opno, exprType(linitial(op->args))) &&
		(place_key(sh, ri, linitial(op->args), lsecond(op->args)) || place_key(sh, ri, lsecond(op->args), linitial(op->args))))
		return true;
	if (!ri->is_pushed_down)
		return place_on(sh, ri);
	if (matches_once(sh, ri->required_relids))
		return false;
	sh->resid = lappend(sh->resid, ri);
	return true;
}

static bool
join_clauses(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	List	   *seen = NIL;
	int			r = -1;

	while ((r = bms_next_member(base, r)) >= 0)
	{
		const RelOptInfo *rel = root->simple_rel_array[r];

		if (rel == NULL)
			continue;
		foreach_node(RestrictInfo, ri, rel->joininfo)
		{
			if (list_member_ptr(seen, ri))
				continue;
			seen = lappend(seen, ri);
			if (sift_v6_exists_clause(ri) ? !sift_v6_exists_join(root, sh, ri) : !place_clause(sh, ri))
				return false;
		}
	}
	foreach_node(RestrictInfo, ri, sh->fact->baserestrictinfo)
		if (sift_v6_exists_clause(ri) && sift_v6_exists_join(root, sh, ri))
			sh->sub_qual = ri;
	return true;
}

/* the join needs the lookup of this node: the planner of the star join or of the unnest alone takes every other, and the push of the keys of a semi join into the scan (v6_join_push.h) the one of a table, not of a sub-select */
static bool
needs_lookup(const SiftV6JoinShape *sh)
{
	if (sh->elem != NULL || sh->resid != NIL)
		return true;
	for (int i = 0; i < sh->nd; i++)
	{
		const SiftV6JoinDim *d = &sh->dim[i];

		if (d->lateral || d->nkeys == 0 || d->on != NIL)
			return true;
		if (d->mode == SV6J_SEMI_EQUI && (sh->nd > 1 || bms_membership(d->rels) != BMS_SINGLETON || d->rel->rtekind != RTE_RELATION))
			return true;
		if (d->mode != SV6J_INNER_EQUI && d->mode != SV6J_LEFT_EQUI && d->mode != SV6J_SEMI_EQUI)
			return true;
		for (int k = 0; k < d->nkeys; k++)
			if (!IsA(d->probe[k], Var) || sift_v6_join_role(sh, (const Var *) d->probe[k]) != ROLE_FACT)
				return true;
	}
	return false;
}

/* the preserved dimension i is the last, and no other is preserved: the rows of it that nothing matched come out with a NULL side for the relations before it, and no later join may drop them */
static bool
last_preserved(const SiftV6JoinShape *sh, int i)
{
	for (int j = 0; j < sh->nd; j++)
		if (j != i && sift_star_preserves_dim(sh->dim[j].mode))
			return false;
	return i == sh->nd - 1;
}

/* a RIGHT or FULL join keeps the rows of its dimension that no row matched, and nothing else of the statement may change what matches */
static bool
preserved_ok(const SiftV6JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
		if (sift_star_preserves_dim(sh->dim[i].mode))
			return sh->elem == NULL && (sh->nd == 1 || (sh->dim[i].closing && last_preserved(sh, i)));
	return true;
}

/* a dimension no key joins, or one with a condition on its pairs, is read by the node's own pairs of (row, dimension row): not with the rows of an unnest, and a dimension of a semi or anti join is matched by its keys */
static bool
pairs_ok(const SiftV6JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
	{
		const SiftV6JoinDim *d = &sh->dim[i];

		if ((d->nkeys == 0 || d->on != NIL) && (sh->elem != NULL || d->mode == SV6J_SEMI_EQUI || d->mode == SV6J_ANTI_EQUI || d->mode == SV6J_RANGE_SMALL))
			return false;
	}
	return true;
}

static bool
shape_of(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	if (!make_dims(root, base, sh) || !dim_rels_ok(root, sh) || !equivalence_keys(root, base, sh) || !join_clauses(root, base, sh) ||
		!sift_v6_exists_main_unique(root, sh))
		return false;
	return pairs_ok(sh) && needs_lookup(sh) && preserved_ok(sh);
}

static bool
rel_outputs(const RelOptInfo *rel, const Var *v)
{
	foreach_ptr(Node, n, rel->reltarget->exprs)
		if (IsA(n, Var) && ((Var *) n)->varattno == v->varattno && ((Var *) n)->varno == v->varno)
			return true;
	return false;
}

static bool
key_class(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_TEXT;
}

/* the column of the probing side a key is: of the index, of the unnest's element, or a payload column of an earlier dimension */
static bool
probe_col(V6Plan *p, const SiftV6JoinShape *sh, const Var *v, int dim, SiftV6Col *out)
{
	bool		ok;

	if (v->varno == p->rel->relid)
		ok = sift_v6_plan_column(p, (const Expr *) v, false, out);
	else if (p->elem_rel != NULL && v->varno == p->elem_rel->relid)
		ok = sift_v6_unnest_column(p, (const Expr *) v, out);
	else
		ok = sift_v6_star_column(p, (const Expr *) v, out) && out->dim < dim && (sh->dim[out->dim].mode == SV6J_INNER_EQUI || sh->dim[out->dim].mode == SV6J_LEFT_EQUI);
	return ok && key_class(out->typid);
}

typedef struct ResidCtx
{
	V6Plan	   *p;
	SiftV6Ext  *x;
	Index		varno[SIFT_V6_RESID_COLS];
	AttrNumber	attno[SIFT_V6_RESID_COLS];
	bool		ok;
} ResidCtx;

/* the virtual attribute (1-based) of the column a Var of a condition is on the pairs: the Var read from the slot of its column */
static Node *
resid_var(Node *n, ResidCtx *c)
{
	Var		   *v,
			   *nv;
	int			i;

	if (n == NULL || !c->ok)
		return n;
	if (IsA(n, PlaceHolderVar) || IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		c->ok = false;
		return n;
	}
	if (!IsA(n, Var))
		return expression_tree_mutator(n, resid_var, c);
	v = (Var *) n;
	for (i = 0; i < c->x->nrcol && (c->varno[i] != v->varno || c->attno[i] != v->varattno); i++)
		;
	if (i == c->x->nrcol)
	{
		SiftV6Col  *col = &c->x->rcol[Min(i, SIFT_V6_RESID_COLS - 1)];

		if (v->varlevelsup != 0 || i == SIFT_V6_RESID_COLS)
			c->ok = false;
		else if (v->varno == c->p->rel->relid)
			c->ok = sift_v6_plan_column(c->p, (const Expr *) v, false, col);
		else if (c->p->elem_rel != NULL && v->varno == c->p->elem_rel->relid)
			c->ok = sift_v6_unnest_column(c->p, (const Expr *) v, col);
		else
			c->ok = sift_v6_star_column(c->p, (const Expr *) v, col);
		if (!c->ok)
			return n;
		c->varno[i] = v->varno;
		c->attno[i] = v->varattno;
		c->x->nrcol++;
	}
	nv = copyObject(v);
	nv->varno = nv->varnosyn = 1;
	nv->varattno = nv->varattnosyn = i + 1;
	nv->varnullingrels = NULL;
	return (Node *) nv;
}

/* a key probed by an expression: the expression over the virtual attributes of the columns of the pairs, each one a column the join can give before the dimension is probed */
static bool
seed_key_expr(ResidCtx *rc, const SiftV6JoinShape *sh, int dim, const Node *probe, SiftV6XDim *xd, int k)
{
	Node	   *e = resid_var(copyObject((Node *) probe), rc);

	if (!rc->ok)
		return false;
	foreach_node(Var, v, pull_var_clause(e, 0))
	{
		const SiftV6Col *col = &rc->x->rcol[v->varattno - 1];

		if (sift_v6_class(col->typid) == SV6C_NONE || (col->src == SV6SRC_DIM && !(col->dim < dim && (sh->dim[col->dim].mode == SV6J_INNER_EQUI || sh->dim[col->dim].mode == SV6J_LEFT_EQUI))))
			return false;
	}
	rc->x->kexpr = lappend(rc->x->kexpr, e);
	xd->kx[k] = list_length(rc->x->kexpr);
	xd->ftype[k] = exprType(probe);
	return true;
}

static bool
seed_dims(V6Plan *p, const SiftV6JoinShape *sh, ResidCtx *rc)
{
	SiftV6Ext  *x;

	if (sh->elem != NULL)
	{
		if (!sift_v6_unnest_seed(p, sh->elem, &sh->rte, sh->elem_left))
			return false;
		x = p->xseed;
		x->elem.semi = sh->semi;
	}
	else
		p->xseed = x = sift_v6_ext_new();
	p->spec.ext = x;
	rc->x = x;
	for (int d = 0; d < sh->nd; d++)
	{
		const SiftV6JoinDim *jd = &sh->dim[d];
		SiftV6XDim *xd = &x->dim[d];

		xd->mode = jd->mode;
		xd->nkeys = jd->nkeys;
		for (int k = 0; k < jd->nkeys; k++)
		{
			SiftV6Col	col;

			if (!(jd->mode == SV6J_RANGE_SMALL ? jd->rel->rows <= SIFT_STAR_RANGE_MAX : key_class(jd->key[k]->vartype)) || !rel_outputs(jd->rel, jd->key[k]))
				return false;
			if (!IsA(jd->probe[k], Var))
			{
				if (!seed_key_expr(rc, sh, d, jd->probe[k], xd, k))
					return false;
			}
			else if (probe_col(p, sh, (const Var *) jd->probe[k], d, &col))
			{
				xd->fatt[k] = col.attno;
				xd->ftype[k] = col.typid;
				xd->fsrc[k] = col.src;
				xd->fdim[k] = col.dim;
				xd->fslot[k] = col.slot;
			}
			else
				return false;
			xd->ktype[k] = jd->key[k]->vartype;
			xd->kcol[k] = jd->key[k]->varattno;
			xd->kvarno[k] = bms_membership(jd->rels) == BMS_SINGLETON ? 0 : jd->key[k]->varno;
		}
		p->dim_rels = lappend(p->dim_rels, jd->rel);
		x->ndims = d + 1;
	}
	return true;
}

static bool
seed_conditions(ResidCtx *rc, const List *quals, List **out)
{
	foreach_node(RestrictInfo, ri, quals)
	{
		if (contain_volatile_functions((Node *) ri->clause))
			return false;
		*out = lappend(*out, resid_var((Node *) ri->clause, rc));
		if (!rc->ok)
			return false;
	}
	return true;
}

static bool
seed_resid(const SiftV6JoinShape *sh, ResidCtx *rc)
{
	if (!seed_conditions(rc, sh->resid, &rc->x->resid))
		return false;
	for (int d = 0; d < sh->nd; d++)
		if (!seed_conditions(rc, sh->dim[d].on, &rc->x->on[d]))
			return false;
	return true;
}

static Cost
lookup_cost(const SiftV6JoinShape *sh, const RelOptInfo *input_rel, Cost *startup)
{
	Cost		total = 0;

	*startup = 0;
	for (int d = 0; d < sh->nd; d++)
	{
		const Cost	read = sift_v6_dim_cost(sh->dim[d].path);

		*startup += read;
		total += read + sh->dim[d].path->rows * cpu_operator_cost * 2.0 + input_rel->rows * cpu_operator_cost;
		if (sh->dim[d].nkeys == 0)
			total += sh->fact->rows * sh->dim[d].path->rows * cpu_operator_cost * 2.0;
		if (sh->dim[d].on != NIL || sh->resid != NIL)
			total += (sh->dim[d].nkeys == 0 ? sh->fact->rows * sh->dim[d].path->rows : input_rel->rows) * cpu_operator_cost * 4.0;
	}
	return total;
}

/* the plan each dimension is read with: its cheapest, or the one that probes it by the constant keys of the fact table's filter */
static void
plan_dims(PlannerInfo *root, SiftV6JoinShape *sh)
{
	for (int d = 0; d < sh->nd; d++)
	{
		SiftV6JoinDim *jd = &sh->dim[d];
		AttrNumber	probe_attno[SIFT_STAR_MAX_KEYS];

		for (int k = 0; k < jd->nkeys; k++)
		{
			const Var  *v = sift_v6_plain_var(jd->probe[k]);

			probe_attno[k] = v != NULL && v->varno == sh->fact->relid ? v->varattno : 0;
		}
		jd->path = bms_membership(jd->rels) == BMS_SINGLETON && !jd->lateral ? sift_v6_dim_path(root, sh->fact, jd->rel, jd->mode, jd->nkeys, probe_attno, jd->key) : jd->rel->cheapest_total_path;
	}
}

/* the statement, the shape of its join and what the planner of the node needs to make a path of a plan that compiled */
typedef struct JoinStmt
{
	RelOptInfo *input_rel;
	GroupPathExtraData *extra;
	List	   *group_exprs;
	const SiftV6JoinShape *sh;
} JoinStmt;

static bool
join_compile(V6Plan *p, const void *arg)
{
	const JoinStmt *js = arg;
	ResidCtx	rc = {.p = p,.ok = true};

	p->sub_qual = js->sh->sub_qual;
	return seed_dims(p, js->sh, &rc) && sift_v6_compile_on(p, js->extra, js->group_exprs) &&
		(js->sh->elem != NULL ? sift_v6_unnest_ready(p) : p->spec.nbins == 0) && seed_resid(js->sh, &rc);
}

static void
join_add(V6Plan *p, const void *arg)
{
	const JoinStmt *js = arg;
	Cost		xs,
				xt = lookup_cost(js->sh, js->input_rel, &xs);
	List	   *children = NIL;

	if (js->sh->elem != NULL)
		xt += sift_v6_unnest_pairs_cost(p, js->input_rel);
	for (int d = 0; d < js->sh->nd; d++)
		children = lappend(children, js->sh->dim[d].path);
	sift_v6_add_join_path(p, js->group_exprs, sift_v6_having_of(js->extra), children, xs, xt, js->input_rel->rows);
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, Relids base, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem, GroupPathExtraData *extra, List *group_exprs)
{
	SiftV6JoinShape sh = {.fact = fact,.elem = elem};
	const JoinStmt js = {input_rel, extra, group_exprs, &sh};

	if (!(elem != NULL ? sift_v6_plan_fact_ok(root, fact) && sift_v6_unnest_rte_joined(root, fact, elem, &sh.rte) : sift_v6_join_fact_ok(root, fact)) || !shape_of(root, base, &sh))
		return;
	plan_dims(root, &sh);
	sift_v6_fact_paths(root, fact, output_rel, join_compile, join_add, &js);
}

void
sift_v6_join_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	const int	nrels = bms_num_members(base);
	int			f = -1;

	if (!sift_v6_plan_query_ok(root, output_rel) || nrels < 2 || nrels > 2 + SIFT_STAR_MAX_DIMS || root->placeholder_list != NIL)
		return;
	while ((f = bms_next_member(base, f)) >= 0)
	{
		RelOptInfo *fact = root->simple_rel_array[f];
		int			e = -1;

		if (fact == NULL)
			continue;
		try_fact(root, input_rel, base, output_rel, fact, NULL, extra, group_exprs);
		while (root->hasLateralRTEs && (e = bms_next_member(base, e)) >= 0)
			if (e != f && root->simple_rel_array[e] != NULL)
				try_fact(root, input_rel, base, output_rel, fact, root->simple_rel_array[e], extra, group_exprs);
	}
}
