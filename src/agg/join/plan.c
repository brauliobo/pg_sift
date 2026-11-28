/*
 * agg/join/plan.c - the planner of the joins of a fact table that need a lookup of the node: a relation with a key more than once, an outer, semi or anti join, a RIGHT or FULL join, a
 * key that is the element of an unnest or a column of an earlier relation, a condition the keys do not say (agg/join/plan.h).
 *
 * The statement's relations are split into the fact table (the one of the index), the element of its unnest (a lateral function scan of one of its columns), and the dimensions: each relation
 * that an inner or left join adds, each side a RIGHT or FULL join preserves, and each set of relations under a semi or anti join (read as the join PostgreSQL planned of them). Every equality
 * that joins a dimension to the relations before it is a key; any other condition of the join runs on the pairs of a batch with PostgreSQL's evaluator, unless it belongs to an outer, semi or anti
 * join (its ON clause is part of the match, which no filter of the result says).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan/int.h"
#include "agg/cost/card.h"
#include "agg/dim_keys.h"
#include "agg/ext.h"
#include "agg/exists_plan.h"
#include "agg/join/pairs.h"
#include "agg/join/plan.h"
#include "agg/join/push.h"
#include "agg/source_rel.h"
#include "agg/join/shape.h"
#include "agg/lateral_plan.h"
#include "agg/expr/pg.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/unique_key.h"

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
	vars = pull_var_clause((Node *) e, PVC_RECURSE_PLACEHOLDERS);
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
sift_v6_join_add_expr_key(SiftV6JoinDim *d, const Node *probe, const Var *key, const Node *kexpr, Oid inputcollid)
{
	for (int k = 0; k < d->nkeys; k++)
		if (same_probe(d->probe[k], probe) && equal(d->kexpr[k], kexpr) && (kexpr != NULL || (d->key[k]->varno == key->varno && d->key[k]->varattno == key->varattno)))
			return true;
	if (d->nkeys == SIFT_STAR_MAX_KEYS || !sift_v6_key_types_agree(exprType(probe), key->vartype, inputcollid))
		return false;
	d->probe[d->nkeys] = probe;
	d->kexpr[d->nkeys] = kexpr;
	d->key[d->nkeys++] = key;
	return true;
}

bool
sift_v6_join_add_key(SiftV6JoinDim *d, const Node *probe, const Var *key, Oid inputcollid)
{
	return sift_v6_join_add_expr_key(d, probe, key, NULL, inputcollid);
}

/* the dimension e is a key of, and the key: a column of it, or an expression of its columns that the plan of the dimension computes (key is then a Var of the type of the expression, and kexpr the expression) */
static int
dim_key_of(const SiftV6JoinShape *sh, Node *e, const Var **key, const Node **kexpr)
{
	const int	d = sift_v6_join_expr_role(sh, e);

	*key = sift_v6_plain_var(e);
	*kexpr = NULL;
	if (*key != NULL)
		return sift_v6_join_role(sh, *key);
	if (d < 0 || !sift_v6_pgexpr_plan_ok(NULL, (const Expr *) e))
		return ROLE_NONE;
	foreach_node(Var, v, pull_var_clause(e, PVC_RECURSE_PLACEHOLDERS))
		if (sift_v6_join_role(sh, v) != d)
			return ROLE_NONE;
	*key = makeVar(bms_next_member(sh->dim[d].rels, -1), InvalidAttrNumber, exprType(e), exprTypmod(e), exprCollation(e), 0);
	*kexpr = e;
	return d;
}

static bool
dim_rels_ok(PlannerInfo *root, SiftV6JoinShape *sh)
{
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
			if (!sift_v6_lateral_join_dim(root, root, NULL, rel, rel->relid, sh, i) || !sift_v6_dim_rel_ok(root, d->rel, sh->nd))
				return false;
		}
		else if (rel == NULL || !sift_v6_dim_rel_ok(root, rel, sh->nd))
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

/* the one part every column of e is of; ROLE_NONE when they are of several */
static int
single_role(const SiftV6JoinShape *sh, Node *e)
{
	int			role = ROLE_NONE;

	foreach_node(Var, v, pull_var_clause(e, PVC_RECURSE_PLACEHOLDERS))
	{
		const int	r = v->varlevelsup == 0 ? sift_v6_join_role(sh, v) : ROLE_NONE;

		if (r == ROLE_NONE || (role != ROLE_NONE && r != role))
			return ROLE_NONE;
		role = r;
	}
	return role;
}

/* the member equivalence_keys took as the probe of the class (its plain column, or the expression itself) */
static bool
is_probe(const EquivalenceMember *em, const Node *probe)
{
	const Var  *v = sift_v6_plain_var((Node *) em->em_expr);

	return equal(v != NULL ? (const Node *) v : (const Node *) em->em_expr, probe);
}

/* every member of the class is the probe, a key of its dimension, or of a part the probe or a key reads already (the planner applies the equality of the members of one relation in its scan):
 * a member of several parts that is no key (coalesce(f.a, d.b)) would drop the equality */
static bool
class_keyed(PlannerInfo *root, Relids base, const SiftV6JoinShape *sh, const EquivalenceClass *ec, const Node *probe, int probe_role)
{
	Bitmapset  *keyed = bms_make_singleton(probe_role - ROLE_NONE),
			   *rest = NULL;

	foreach_node(EquivalenceMember, em, ec->ec_members)
	{
		const Var  *key;
		const Node *kexpr;
		int			d;

		if (em->em_is_child || !bms_overlap(pull_varnos(root, (Node *) em->em_expr), base) || is_probe(em, probe))
			continue;
		d = dim_key_of(sh, (Node *) em->em_expr, &key, &kexpr);
		if (d >= 0 && d != probe_role)
			keyed = bms_add_member(keyed, d - ROLE_NONE);
		else if ((d = single_role(sh, (Node *) em->em_expr)) == ROLE_NONE)
			return false;
		else
			rest = bms_add_member(rest, d - ROLE_NONE);
	}
	return bms_is_subset(rest, keyed);
}

/* every member of the class that is a key of a dimension has a type the hash of the probe takes */
static bool
class_key_types_ok(const SiftV6JoinShape *sh, const EquivalenceClass *ec, const Node *probe, int probe_role)
{
	foreach_node(EquivalenceMember, em, ec->ec_members)
	{
		const Var  *key;
		const Node *kexpr;
		const int	d = em->em_is_child ? ROLE_NONE : dim_key_of(sh, (Node *) em->em_expr, &key, &kexpr);

		if (d >= 0 && d != probe_role && !sift_v6_key_types_agree(exprType(probe), key->vartype, ec->ec_collation))
			return false;
	}
	return true;
}

/* a class of equal columns and expressions that the planner made of the inner and semi joins (a class of one member, the sort key of a side of a merge join, states no equality): the members that are dimensions are joined to the member before them, the column or expression of the fact table, of the element or of the first dimension */
static bool
equivalence_keys(PlannerInfo *root, Relids base, SiftV6JoinShape *sh)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		const Node *probe = NULL;
		int			probe_role = ROLE_NONE;
		Relids		rels = NULL;
		int			ncolumns = 0,
					nmembers = 0;

		if (ec->ec_has_const || ec->ec_has_volatile || ec->ec_broken || !sift_v6_ec_equates(ec))
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			Relids		vr = bms_intersect(pull_varnos(root, (Node *) em->em_expr), base);
			const int	role = bms_is_empty(vr) ? ROLE_NONE : sift_v6_join_expr_role(sh, (Node *) em->em_expr);

			if (em->em_is_child)
				continue;
			nmembers++;
			rels = bms_add_members(rels, vr);
			if (role == ROLE_NONE)
				continue;
			ncolumns += role < 0;
			if (probe == NULL || role < probe_role)
			{
				probe = v != NULL ? (const Node *) v : (const Node *) em->em_expr;
				probe_role = role;
			}
		}
		if (nmembers < 2 || bms_membership(rels) != BMS_MULTIPLE || within_one_dim(sh, rels))
			continue;
		if (probe == NULL || ncolumns > 1 || (!IsA(probe, Var) && (probe_role >= 0 || !sift_v6_pgexpr_plan_ok(NULL, (const Expr *) probe))) ||
			!class_keyed(root, base, sh, ec, probe, probe_role) || !class_key_types_ok(sh, ec, probe, probe_role))
		{
			if (!computed_class_ok(sh, rels))
				return false;
			sh->resid = list_concat_unique_ptr(sh->resid, ec->ec_sources);
			continue;
		}
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *key;
			const Node *kexpr;
			const int	d = em->em_is_child ? ROLE_NONE : dim_key_of(sh, (Node *) em->em_expr, &key, &kexpr);

			if (d < 0 || d == probe_role)
				continue;
			if ((sh->dim[d].mode != SV6J_INNER_EQUI && sh->dim[d].mode != SV6J_SEMI_EQUI) || !sift_v6_join_add_expr_key(&sh->dim[d], probe, key, kexpr, ec->ec_collation))
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

/* fact.col <@ dim.range: the dimension is the periods, probed by the column; an inner join multiplies a row by the periods that hold it before its elements are taken apart (SV6J_RANGE_SMALL),
 * a semi or anti join keeps it by whether one does; a LEFT join of it is the normal plan's */
static bool
place_period(SiftV6JoinShape *sh, const RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const Var  *col,
			   *range;
	SiftV6JoinDim    *d;

	if (!IsA(ri->clause, OpExpr) || op->opno != OID_RANGE_ELEM_CONTAINED_OP)
		return false;
	col = sift_v6_plain_var(linitial(op->args));
	range = sift_v6_plain_var(lsecond(op->args));
	if (col == NULL || range == NULL || sift_v6_join_role(sh, col) != ROLE_FACT || sift_v6_join_role(sh, range) < 0 || !period_types(col->vartype, range->vartype))
		return false;
	d = &sh->dim[sift_v6_join_role(sh, range)];
	if ((d->mode != SV6J_INNER_EQUI && d->mode != SV6J_SEMI_EQUI && d->mode != SV6J_ANTI_EQUI) || d->nkeys != 0 || !clause_level_ok(ri, d->mode))
		return false;
	d->mode = d->mode == SV6J_INNER_EQUI ? SV6J_RANGE_SMALL : d->mode == SV6J_SEMI_EQUI ? SV6J_RANGE_SEMI : SV6J_RANGE_ANTI;
	d->probe[0] = (const Node *) col;
	d->key[0] = range;
	d->nkeys = 1;
	return true;
}

/* the equality of a column of a dimension (other) and a column or an expression of the relations before it (arg), as a key of the dimension */
static bool
place_key(SiftV6JoinShape *sh, const RestrictInfo *ri, Node *arg, Node *other, Oid inputcollid)
{
	const Var  *key;
	const Node *kexpr;
	const Var  *column = sift_v6_plain_var(arg);
	const Node *probe = column != NULL ? (const Node *) column : arg;
	const int	d = dim_key_of(sh, other, &key, &kexpr);

	if (d < 0 || sift_v6_join_expr_role(sh, probe) == ROLE_NONE || sift_v6_join_expr_role(sh, probe) >= d)
		return false;
	return (column != NULL || sift_v6_pgexpr_plan_ok(NULL, (const Expr *) probe)) && clause_level_ok(ri, sh->dim[d].mode) && sift_v6_join_add_expr_key(&sh->dim[d], probe, key, kexpr, inputcollid);
}

static bool
place_key_either(SiftV6JoinShape *sh, const RestrictInfo *ri, Node *a, Node *b, Oid inputcollid)
{
	return place_key(sh, ri, a, b, inputcollid) || place_key(sh, ri, b, a, inputcollid);
}

/* jsonb_build_array(a1, .. an): its arguments, or NIL when it is not one (or takes a VARIADIC array) */
static List *
jsonb_array_args(const Node *e)
{
	const FuncExpr *f = (const FuncExpr *) e;

	return IsA(e, FuncExpr) && f->funcid == F_JSONB_BUILD_ARRAY_ANY && !f->funcvariadic ? f->args : NIL;
}

/* the text of the jsonb array e, compared by its bytes */
static Node *
jsonb_text(Node *e)
{
	CoerceViaIO *c = makeNode(CoerceViaIO);

	c->arg = (Expr *) e;
	c->resulttype = TEXTOID;
	c->resultcollid = C_COLLATION_OID;
	c->coerceformat = COERCE_IMPLICIT_CAST;
	c->location = -1;
	return (Node *) c;
}

/*
 * jsonb_build_array(a1, .. an) = jsonb_build_array(b1, .. bn), each ai and bi of key types that agree: the two arrays are equal exactly when their texts are (an integer is the same number,
 * a date, timestamp, bool or string the same text, a NULL element the same null), so the texts are a key of the dimension, probed by hash where the equality of the arrays tests every pair
 */
static bool
place_jsonb_array_key(SiftV6JoinShape *sh, const RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	List	   *a,
			   *b;

	if (!IsA(op, OpExpr) || get_opcode(op->opno) != F_JSONB_EQ || (a = jsonb_array_args(linitial(op->args))) == NIL || (b = jsonb_array_args(lsecond(op->args))) == NIL ||
		list_length(a) != list_length(b))
		return false;
	for (int i = 0; i < list_length(a); i++)
		if (!sift_v6_key_types_agree(exprType(list_nth(a, i)), exprType(list_nth(b, i)), C_COLLATION_OID))
			return false;
	return place_key_either(sh, ri, jsonb_text(linitial(op->args)), jsonb_text(lsecond(op->args)), C_COLLATION_OID);
}

/* a semi or anti join matches a row once, by its first dimension row: a condition on the pairs would decide on that one only */
static bool
matches_once(const SiftV6JoinShape *sh, Relids rels)
{
	for (int i = 0; i < sh->nd; i++)
		if (sift_star_filters(sh->dim[i].mode) && bms_overlap(sh->dim[i].rels, rels))
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

/* the RIGHT or FULL join a condition of the relations it nulls is under (it reads them, and its relations do not name the outer join): it drops their rows before the join; -1 for none */
static int
preserved_over(const SiftV6JoinShape *sh, const RestrictInfo *ri)
{
	for (int i = 0; i < sh->nd; i++)
		if (sift_star_preserves_dim(sh->dim[i].mode) && !bms_is_empty(ri->clause_relids) && !bms_overlap(ri->clause_relids, sh->dim[i].rels) &&
			!bms_is_member(sh->dim[i].ojrelid, ri->clause_relids))
			return i;
	return -1;
}

/*
 * where a condition of the join goes: a key of the dimension it joins to the relations before it, a condition of the plan of one dimension, or a condition on the pairs. false: the join has no place
 * for it. A clone (is_clone) is the form of the clause for an order of commuting outer joins other than the statement's, the order the pipe joins in: the clause it is a clone of is placed.
 * A condition under a RIGHT join decides which rows a row of the preserved dimension meets, as its ON clause does; the rows a FULL join keeps of the other side would pass no such filter
 */
static bool
place_clause(SiftV6JoinShape *sh, RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	int			d;

	if (ri->is_clone)
		return true;
	if (within_one_dim(sh, ri->required_relids))
		return true;
	if (place_period(sh, ri))
		return true;
	if (IsA(ri->clause, OpExpr) && list_length(op->args) == 2 && op_hashjoinable(op->opno, exprType(linitial(op->args))) &&
		(place_jsonb_array_key(sh, ri) || place_key_either(sh, ri, linitial(op->args), lsecond(op->args), op->inputcollid)))
		return true;
	if (!ri->is_pushed_down)
		return place_on(sh, ri);
	if (matches_once(sh, ri->required_relids))
		return false;
	d = preserved_over(sh, ri);
	if (d < 0)
		sh->resid = lappend(sh->resid, ri);
	else if (sh->dim[d].mode == SV6J_RIGHT_EQUI)
		sh->dim[d].on = lappend(sh->dim[d].on, ri);
	else
		return false;
	return true;
}

/* the conditions of the join in the shape. An EXISTS of the fact row alone is the semi unnest of the join when each main dimension matches a row once (the unnest counts the row once);
 * else it stays a qual of the table, which the tests of the scan decide (seed_dims) */
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
	{
		SiftV6JoinShape trial = *sh;

		if (sift_v6_exists_clause(ri) && sift_v6_exists_join(root, &trial, ri) && sift_v6_exists_main_unique(root, &trial))
		{
			*sh = trial;
			sh->sub_qual = ri;
		}
	}
	return true;
}

/* a key the push reads as integers */
static bool
int_key(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	return cls == SV6C_I32 || cls == SV6C_I64;
}

/* the join needs the lookup of this node: the planner of the star join or of the unnest alone takes every other, and the push of the keys into the scan (join/push.h) the aggregates of a semi join of a small table */
static bool
needs_lookup(PlannerInfo *root, const SiftV6JoinShape *sh, bool rows)
{
	if (sh->elem != NULL || sh->resid != NIL)
		return true;
	for (int i = 0; i < sh->nd; i++)
	{
		const SiftV6JoinDim *d = &sh->dim[i];

		if (d->lateral || d->nkeys == 0 || d->on != NIL)
			return true;
		if (d->mode == SV6J_SEMI_EQUI && (rows || sh->nd > 1 || bms_membership(d->rels) != BMS_SINGLETON || !sift_v6_push_small_side(root, d->rel)))
			return true;
		if (d->mode != SV6J_INNER_EQUI && d->mode != SV6J_LEFT_EQUI && d->mode != SV6J_SEMI_EQUI)
			return true;
		for (int k = 0; k < d->nkeys; k++)
			if (d->kexpr[k] != NULL || !IsA(d->probe[k], Var) || sift_v6_join_role(sh, (const Var *) d->probe[k]) != ROLE_FACT || !int_key(exprType(d->probe[k])))
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

/* a condition on the pairs reads the dimension: of an EXISTS, the pairs run it before they probe a RIGHT or FULL dimension (agg/pipe.h) */
static bool
resid_reads(PlannerInfo *root, const SiftV6JoinShape *sh, const SiftV6JoinDim *d)
{
	foreach_node(RestrictInfo, ri, sh->resid)
	{
		const Relids rels = pull_varnos(root, (Node *) ri->clause);

		if (bms_overlap(rels, d->rels) || bms_is_member(d->ojrelid, rels))
			return true;
	}
	return false;
}

/* of an EXISTS, each condition on the pairs is one of its sub-select (it reads the element or a relation of the sub-select): the error of the function of a row is raised before them, as
 * PostgreSQL runs the EXISTS after the other conditions of the join (agg/pipe.h, late_row) */
static bool
semi_resid_ok(PlannerInfo *root, const SiftV6JoinShape *sh)
{
	foreach_node(RestrictInfo, ri, sh->resid)
	{
		const Relids rels = pull_varnos(root, (Node *) ri->clause);
		bool		sub = bms_is_member(sh->elem->relid, rels);

		for (int d = sh->nmain; d < sh->nd; d++)
			sub |= bms_overlap(rels, sh->dim[d].rels);
		if (!sub)
			return false;
	}
	return true;
}

/* a RIGHT or FULL join keeps the rows of its dimension that no pair matched (a row, or a row and an element of its unnest), and nothing else of the statement may change what matches */
static bool
preserved_ok(PlannerInfo *root, const SiftV6JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
		if (sift_star_preserves_dim(sh->dim[i].mode))
			return (sh->nd == 1 || (sh->dim[i].closing && last_preserved(sh, i))) && !(sh->semi && resid_reads(root, sh, &sh->dim[i]));
	return true;
}

/* a dimension no key joins, or one with a condition on its pairs, is read by the node's own pairs of (row, dimension row): not with the rows of an unnest, and a dimension of a semi or anti join is matched by its keys */
static bool
pairs_ok(const SiftV6JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
	{
		const SiftV6JoinDim *d = &sh->dim[i];

		if ((d->nkeys == 0 || d->on != NIL) && ((sh->elem != NULL && d->nkeys == 0) || sift_star_filters(d->mode) || sift_star_period(d->mode)))
			return false;
	}
	return true;
}

/* the table of the dimension has a unique index on columns of its keys: a fact row matches one of its rows at most (a key an expression computes is no such column) */
static bool
unique_on_keys(PlannerInfo *root, const SiftV6JoinDim *d)
{
	int			atts[SIFT_STAR_MAX_KEYS];

	for (int k = 0; k < d->nkeys; k++)
		atts[k] = d->kexpr[k] == NULL ? d->key[k]->varattno : InvalidAttrNumber;
	return sift_v6_unique_key(planner_rt_fetch(d->rel->relid, root)->relid, d->nkeys, 0, atts, NULL);
}

/* a dimension of the best K rows keeps or drops a fact row and never repeats it: a semi or anti join, or an inner or left join of a table on a unique index */
static bool
rows_dim_ok(PlannerInfo *root, const SiftV6JoinDim *d)
{
	return sift_star_filters(d->mode) ||
		((d->mode == SV6J_INNER_EQUI || d->mode == SV6J_LEFT_EQUI) && d->rel->reloptkind == RELOPT_BASEREL && d->on == NIL && unique_on_keys(root, d));
}

/* the best K rows of the join are fact rows its dimensions keep, each at most once, with no condition on the pairs */
static bool
rows_shape_ok(PlannerInfo *root, const SiftV6JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
		if (!rows_dim_ok(root, &sh->dim[i]))
			return false;
	return sh->resid == NIL;
}

static bool
shape_of(PlannerInfo *root, Relids base, SiftV6JoinShape *sh, bool rows)
{
	if (!sift_v6_join_make_dims(root, base, sh) || !dim_rels_ok(root, sh) || !equivalence_keys(root, base, sh) || !join_clauses(root, base, sh) ||
		!sift_v6_exists_main_unique(root, sh))
		return false;
	return pairs_ok(sh) && needs_lookup(root, sh, rows) && preserved_ok(root, sh) && (!sh->semi || semi_resid_ok(root, sh)) && sift_v6_join_elem_nest_ok(sh);
}

static bool
rel_outputs(const RelOptInfo *rel, const Var *v)
{
	foreach_ptr(Node, n, rel->reltarget->exprs)
		if (IsA(n, Var) && ((Var *) n)->varattno == v->varattno && ((Var *) n)->varno == v->varno)
			return true;
	return false;
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
	return ok && sift_v6_key_type_ok(out->typid);
}

/* a key probed by an expression: the expression over the virtual attributes of the columns of the pairs, each one a column the join can give before the dimension is probed */
static bool
seed_key_expr(V6Plan *p, const SiftV6JoinShape *sh, int dim, const Node *probe, SiftV6XDim *xd, int k)
{
	Node	   *e = sift_v6_pair_expr(p, (Node *) probe);

	if (e == NULL)
		return false;
	foreach_node(Var, v, pull_var_clause(e, 0))
	{
		const SiftV6Col *col = &p->xseed->rcol[v->varattno - 1];

		if (sift_v6_class(col->typid) == SV6C_NONE || (col->src == SV6SRC_DIM && !(col->dim < dim && (sh->dim[col->dim].mode == SV6J_INNER_EQUI || sh->dim[col->dim].mode == SV6J_LEFT_EQUI))))
			return false;
	}
	p->xseed->kexpr = lappend(p->xseed->kexpr, e);
	xd->kx[k] = list_length(p->xseed->kexpr);
	xd->ftype[k] = exprType(probe);
	return true;
}

/* the unnest of the join, or the tests of the elements of the arrays of its rows, and its dimensions */
static bool
seed_dims(V6Plan *p, const SiftV6JoinShape *sh, GroupPathExtraData *extra)
{
	SiftV6Ext  *x;

	if (sh->elem != NULL)
	{
		if (!sift_v6_unnest_seed(p, sh->elem, &sh->rte, sh->elem_left))
			return false;
		if (extra != NULL)
			sift_v6_unnest_tests_beside(p, &sh->rte, p->out, extra);
		x = p->xseed;
		x->elem.semi = sh->semi;
	}
	else if (extra != NULL && !sift_v6_unnest_tests_seed(p, extra))
		return false;
	else if ((x = p->xseed) == NULL)
		p->xseed = x = sift_v6_ext_new();
	p->spec.ext = x;
	for (int d = 0; d < sh->nd; d++)
	{
		const SiftV6JoinDim *jd = &sh->dim[d];
		SiftV6XDim *xd = &x->dim[d];

		xd->mode = jd->mode;
		xd->first_row = jd->lateral;
		xd->nkeys = jd->nkeys;
		for (int k = 0; k < jd->nkeys; k++)
		{
			SiftV6Col	col;

			if (!(sift_star_period(jd->mode) ? jd->rel->rows <= SIFT_STAR_RANGE_MAX : sift_v6_key_type_ok(jd->key[k]->vartype)) || (jd->kexpr[k] == NULL && !rel_outputs(jd->rel, jd->key[k])))
				return false;
			if (!IsA(jd->probe[k], Var))
			{
				if (!seed_key_expr(p, sh, d, jd->probe[k], xd, k))
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
			xd->kres[k] = jd->kexpr[k] != NULL ? jd->kpos[k] : 0;
			xd->kcol[k] = jd->key[k]->varattno;
			xd->kvarno[k] = bms_membership(jd->rels) == BMS_SINGLETON ? 0 : jd->key[k]->varno;
		}
		p->dim_rels = lappend(p->dim_rels, jd->rel);
		x->ndims = d + 1;
	}
	return true;
}

static bool
seed_conditions(V6Plan *p, const List *quals, List **out)
{
	foreach_node(RestrictInfo, ri, quals)
	{
		Node	   *e;

		if (contain_volatile_functions((Node *) ri->clause) || (e = sift_v6_pair_expr(p, (Node *) ri->clause)) == NULL)
			return false;
		*out = lappend(*out, e);
	}
	return true;
}

static bool
seed_resid(V6Plan *p, const SiftV6JoinShape *sh)
{
	if (!seed_conditions(p, sh->resid, &p->xseed->resid))
		return false;
	for (int d = 0; d < sh->nd; d++)
		if (!seed_conditions(p, sh->dim[d].on, &p->xseed->on[d]))
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

/* the plan of a dimension with the keys it computes as columns after its own */
static Path *
computed_keys_path(PlannerInfo *root, SiftV6JoinDim *jd)
{
	PathTarget *target = copy_pathtarget(jd->path->pathtarget);

	for (int k = 0; k < jd->nkeys; k++)
		if (jd->kexpr[k] != NULL)
		{
			jd->kpos[k] = list_length(target->exprs) + 1;
			add_column_to_pathtarget(target, (Expr *) jd->kexpr[k], 0);
		}
	set_pathtarget_cost_width(root, target);
	return (Path *) create_projection_path(root, jd->rel, jd->path, target);
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

			probe_attno[k] = v != NULL && v->varno == sh->fact->relid && jd->kexpr[k] == NULL ? v->varattno : 0;
		}
		jd->path = bms_membership(jd->rels) == BMS_SINGLETON && !jd->lateral ? sift_v6_dim_path(root, sh->fact, jd->rel, jd->mode, jd->nkeys, probe_attno, jd->key) : jd->rel->cheapest_total_path;
		for (int k = 0; k < jd->nkeys; k++)
			if (jd->kexpr[k] != NULL)
			{
				jd->path = computed_keys_path(root, jd);
				break;
			}
	}
}

/* the statement, the shape of its join and what the planner of the node needs to make a path of a plan that compiled: the groups of an aggregate, or the best topk rows of the join */
typedef struct JoinStmt
{
	RelOptInfo *input_rel;
	GroupPathExtraData *extra;
	List	   *group_exprs;
	const SiftV6JoinShape *sh;
	uint32		topk;
	double	   *charge;			/* what each fact relation's probes are charged for the lookups PostgreSQL guesses (sift_v6_lookup_paths), by relid; NULL: nothing */
	double		lift_rows;		/* V6Plan.lift_rows */
} JoinStmt;

static bool
join_compile(V6Plan *p, const void *arg)
{
	const JoinStmt *js = arg;

	p->sub_qual = js->sh->sub_qual;
	return seed_dims(p, js->sh, js->extra) &&
		(js->topk > 0 ? sift_v6_rows_compile(p, js->topk) : sift_v6_compile_on(p, js->extra, js->group_exprs)) &&
		(!p->xseed->unnest || sift_v6_unnest_ready(p)) && seed_resid(p, js->sh);
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
	if (js->topk > 0)
	{
		Cost		open,
					scan;

		sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, &scan);
		foreach_ptr(Path, child, children)
			xt += child->total_cost;
		sift_v6_rows_add(p, js->topk, scan, children, xs, xt);
		return;
	}
	p->lift_rows = js->lift_rows;
	sift_v6_add_join_path(p, js->group_exprs, sift_v6_having(js->extra), children, xs, xt, js->input_rel->rows);
}

static void
try_fact(PlannerInfo *root, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem, const JoinStmt *stmt)
{
	SiftV6JoinShape sh = {.fact = fact,.elem = elem};
	JoinStmt	js = *stmt;
	const Relids base = sift_v6_plan_base_relids(root, js.input_rel);
	double		guess;

	js.sh = &sh;
	if (!(elem != NULL ? sift_v6_plan_fact_ok(root, fact) && sift_v6_unnest_rte_joined(root, fact, elem, &sh.rte) : sift_v6_join_fact_ok(root, fact)) || !shape_of(root, base, &sh, js.topk > 0) ||
		(js.topk > 0 && !rows_shape_ok(root, &sh)))
		return;
	plan_dims(root, &sh);
	guess = sift_v6_join_guessed_keys(root, &sh, js.input_rel->rows);
	if (js.charge != NULL)
		sift_v6_charge_probes(fact, guess, &js.charge[fact->relid], output_rel);
	js.lift_rows = guess > 1.0 ? js.input_rel->rows * guess : 0;
	sift_v6_fact_paths(root, fact, output_rel, join_compile, join_add, &js);
}

static void
try_facts(PlannerInfo *root, RelOptInfo *output_rel, const JoinStmt *js)
{
	Relids		base = sift_v6_plan_base_relids(root, js->input_rel);
	const int	nrels = bms_num_members(base);
	int			f = -1;

	if (nrels < 2 || nrels > 2 + SIFT_STAR_MAX_DIMS || sift_v6_join_placeholder_in_quals(root, base))
		return;
	while ((f = bms_next_member(base, f)) >= 0)
	{
		RelOptInfo *fact = root->simple_rel_array[f];
		int			e = -1;

		if (fact == NULL)
			continue;
		try_fact(root, output_rel, fact, NULL, js);
		while (js->topk == 0 && root->hasLateralRTEs && (e = bms_next_member(base, e)) >= 0)
			if (e != f && root->simple_rel_array[e] != NULL)
				try_fact(root, output_rel, fact, root->simple_rel_array[e], js);
	}
}

void
sift_v6_join_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs, double *charge)
{
	const JoinStmt js = {.input_rel = input_rel,.extra = extra,.group_exprs = group_exprs,.charge = charge};

	if (sift_v6_plan_query_ok(root, output_rel))
		try_facts(root, output_rel, &js);
}

void
sift_v6_join_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k)
{
	const JoinStmt js = {.input_rel = input_rel,.topk = k};

	try_facts(root, output_rel, &js);
}

void
sift_v6_lookup_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	double	   *charge = palloc(root->simple_rel_array_size * sizeof(double));

	for (int r = 0; r < root->simple_rel_array_size; r++)
		charge[r] = 1.0;
	sift_v6_star_upper_paths(root, input_rel, output_rel, extra, group_exprs, charge);
	sift_v6_join_upper_paths(root, input_rel, output_rel, extra, group_exprs, charge);
	pfree(charge);
}
