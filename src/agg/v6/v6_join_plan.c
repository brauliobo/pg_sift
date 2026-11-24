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

#include "agg/plan/format_ops.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_join_plan.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "col/am/format_ops.h"

#define ROLE_FACT	(-2)
#define ROLE_ELEM	(-1)
#define ROLE_NONE	(-3)

typedef struct JoinDim
{
	Relids		rels;
	RelOptInfo *rel;			/* its plan: the relation, or the join PostgreSQL made of the relations */
	SiftV6JoinMode mode;
	int			nkeys;
	const Var  *probe[SIFT_STAR_MAX_KEYS];
	const Var  *key[SIFT_STAR_MAX_KEYS];
} JoinDim;

typedef struct JoinShape
{
	RelOptInfo *fact;
	RelOptInfo *elem;
	bool		elem_left;
	int			nd;
	JoinDim		dim[SIFT_STAR_MAX_DIMS];
	List	   *resid;			/* the RestrictInfo of each condition no key says */
} JoinShape;

static bool
add_dim(JoinShape *sh, Relids rels, SiftV6JoinMode mode)
{
	if (sh->nd == SIFT_STAR_MAX_DIMS)
		return false;
	for (int i = 0; i < sh->nd; i++)
		if (bms_overlap(sh->dim[i].rels, rels))
			return false;
	sh->dim[sh->nd++] = (JoinDim) {.rels = rels,.mode = mode};
	return true;
}

static bool
single_in(Relids r, Relids rest)
{
	return bms_membership(r) == BMS_SINGLETON && bms_is_subset(r, rest);
}

/* the special join of the statement as a dimension: a left join to a relation, the relation a RIGHT or FULL join keeps, the relations under a semi or anti join; false for any other */
static bool
special_dim(JoinShape *sh, const SpecialJoinInfo *sj, Relids rest)
{
	Relids		fact = bms_make_singleton(sh->fact->relid);
	Relids		l = sj->min_lefthand,
				r = sj->min_righthand;

	switch (sj->jointype)
	{
		case JOIN_LEFT:
			if (single_in(r, rest))
				return add_dim(sh, r, SV6J_LEFT_EQUI);
			return bms_equal(r, fact) && single_in(l, rest) && add_dim(sh, l, SV6J_RIGHT_EQUI);
		case JOIN_FULL:
			if (bms_equal(l, fact))
				return single_in(r, rest) && add_dim(sh, r, SV6J_FULL_EQUI);
			return bms_equal(r, fact) && single_in(l, rest) && add_dim(sh, l, SV6J_FULL_EQUI);
		case JOIN_SEMI:
		case JOIN_ANTI:
			return bms_is_subset(r, rest) && add_dim(sh, r, sj->jointype == JOIN_SEMI ? SV6J_SEMI_EQUI : SV6J_ANTI_EQUI);
		default:
			return false;
	}
}

/* the dimensions in the order the pipe probes them: a join of inner relations, then the others in the order the statement joins them, each probing columns of the relations before it */
static bool
make_dims(PlannerInfo *root, Relids base, JoinShape *sh)
{
	Relids		rest = bms_del_member(bms_copy(base), sh->fact->relid);
	JoinDim		inner[SIFT_STAR_MAX_DIMS];
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
		inner[ni++] = (JoinDim) {.rels = bms_make_singleton(r),.mode = SV6J_INNER_EQUI};
	}
	memmove(&sh->dim[ni], &sh->dim[0], sh->nd * sizeof(JoinDim));
	memcpy(&sh->dim[0], inner, ni * sizeof(JoinDim));
	sh->nd += ni;
	return sh->nd > 0;
}

static bool
dim_rels_ok(PlannerInfo *root, JoinShape *sh)
{
	const uint64 budget = Max((uint64) work_mem * 1024 / 2 / sh->nd, 4096);

	for (int i = 0; i < sh->nd; i++)
	{
		JoinDim    *d = &sh->dim[i];

		RelOptInfo *rel;

		if (bms_membership(d->rels) == BMS_SINGLETON)
			rel = root->simple_rel_array[bms_singleton_member(d->rels)];
		else
			rel = find_join_rel(root, d->rels);
		if (rel == NULL || !sift_v6_dim_rel_ok(root, rel, budget))
			return false;
		d->rel = rel;
	}
	return true;
}

/* the part a relation has in the join: the fact table, the element, the index of its dimension, or none */
static int
role_of(const JoinShape *sh, const Var *v)
{
	if (v->varno == sh->fact->relid)
		return ROLE_FACT;
	if (sh->elem != NULL && v->varno == sh->elem->relid)
		return ROLE_ELEM;
	for (int i = 0; i < sh->nd; i++)
		if (bms_is_member(v->varno, sh->dim[i].rels))
			return i;
	return ROLE_NONE;
}

static bool
add_key(JoinDim *d, const Var *probe, const Var *key)
{
	for (int k = 0; k < d->nkeys; k++)
		if (d->probe[k]->varno == probe->varno && d->probe[k]->varattno == probe->varattno && d->key[k]->varno == key->varno && d->key[k]->varattno == key->varattno)
			return true;
	if (d->nkeys == SIFT_STAR_MAX_KEYS || !sift_v6_keys_agree(probe, key))
		return false;
	d->probe[d->nkeys] = probe;
	d->key[d->nkeys++] = key;
	return true;
}

/* the relations are all under one dimension: its plan applies what joins them */
static bool
within_one_dim(const JoinShape *sh, Relids rels)
{
	for (int i = 0; i < sh->nd; i++)
		if (bms_is_subset(rels, sh->dim[i].rels))
			return true;
	return false;
}

/* a class of equal columns that the planner made of the inner and semi joins: the members that are dimensions are joined to the member before them, the column of the fact table, of the element or of the first dimension */
static bool
equivalence_keys(PlannerInfo *root, Relids base, JoinShape *sh)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		const Var  *probe = NULL;
		Relids		rels = NULL;
		bool		computed = false;
		int			ncolumns = 0;

		if (ec->ec_has_const || ec->ec_has_volatile || ec->ec_broken)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			Relids		vr = bms_intersect(pull_varnos(root, (Node *) em->em_expr), base);

			if (em->em_is_child)
				continue;
			rels = bms_add_members(rels, vr);
			computed |= v == NULL && !bms_is_empty(vr);
			if (v == NULL || role_of(sh, v) == ROLE_NONE)
				continue;
			ncolumns += role_of(sh, v) < 0;
			if (probe == NULL || role_of(sh, v) < role_of(sh, probe))
				probe = v;
		}
		if (bms_membership(rels) != BMS_MULTIPLE || within_one_dim(sh, rels))
			continue;
		if (computed || probe == NULL || ncolumns > 1)
			return false;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			const int	d = v != NULL && !em->em_is_child ? role_of(sh, v) : ROLE_NONE;

			if (d < 0 || d == role_of(sh, probe))
				continue;
			if ((sh->dim[d].mode != SV6J_INNER_EQUI && sh->dim[d].mode != SV6J_SEMI_EQUI) || !add_key(&sh->dim[d], probe, v))
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
place_period(JoinShape *sh, const RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const Var  *col,
			   *range;
	JoinDim    *d;

	if (sh->elem != NULL || !IsA(ri->clause, OpExpr) || op->opno != OID_RANGE_ELEM_CONTAINED_OP || !ri->is_pushed_down)
		return false;
	col = sift_v6_plain_var(linitial(op->args));
	range = sift_v6_plain_var(lsecond(op->args));
	if (col == NULL || range == NULL || role_of(sh, col) != ROLE_FACT || role_of(sh, range) < 0 || !period_types(col->vartype, range->vartype))
		return false;
	d = &sh->dim[role_of(sh, range)];
	if (d->mode != SV6J_INNER_EQUI || d->nkeys != 0)
		return false;
	d->mode = SV6J_RANGE_SMALL;
	d->probe[0] = col;
	d->key[0] = range;
	d->nkeys = 1;
	return true;
}

/* where a condition of the join goes: a key of the dimension it joins to the relations before it, a condition of the plan of one dimension, or a condition on the pairs. false: the join has no place for it */
static bool
place_clause(JoinShape *sh, RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const Var  *a = NULL,
			   *b = NULL;

	if (ri->has_clone || ri->is_clone)
		return false;
	if (within_one_dim(sh, ri->required_relids))
		return true;
	if (place_period(sh, ri))
		return true;
	if (IsA(ri->clause, OpExpr) && list_length(op->args) == 2 && op_hashjoinable(op->opno, exprType(linitial(op->args))))
	{
		a = sift_v6_plain_var(linitial(op->args));
		b = sift_v6_plain_var(lsecond(op->args));
	}
	if (a != NULL && b != NULL && role_of(sh, a) != role_of(sh, b))
	{
		const Var  *probe = role_of(sh, a) < role_of(sh, b) ? a : b;
		const Var  *key = probe == a ? b : a;
		const int	d = role_of(sh, key);

		if (d >= 0 && role_of(sh, probe) != ROLE_NONE && clause_level_ok(ri, sh->dim[d].mode) && add_key(&sh->dim[d], probe, key))
			return true;
	}
	if (!ri->is_pushed_down)
		return false;
	sh->resid = lappend(sh->resid, ri);
	return true;
}

static bool
join_clauses(PlannerInfo *root, Relids base, JoinShape *sh)
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
			if (!place_clause(sh, ri))
				return false;
		}
	}
	return true;
}

/* the join needs the lookup of this node: the planner of the star join or of the unnest alone takes every other, and the push of the keys of a semi join into the scan (v6_join_push.h) the one of a table, not of a sub-select */
static bool
needs_lookup(const JoinShape *sh)
{
	if (sh->elem != NULL || sh->resid != NIL)
		return true;
	for (int i = 0; i < sh->nd; i++)
	{
		const JoinDim *d = &sh->dim[i];

		if (d->mode == SV6J_SEMI_EQUI && (sh->nd > 1 || bms_membership(d->rels) != BMS_SINGLETON || d->rel->rtekind != RTE_RELATION))
			return true;
		if (d->mode != SV6J_INNER_EQUI && d->mode != SV6J_LEFT_EQUI && d->mode != SV6J_SEMI_EQUI)
			return true;
		for (int k = 0; k < d->nkeys; k++)
			if (role_of(sh, d->probe[k]) != ROLE_FACT)
				return true;
	}
	return false;
}

/* a RIGHT or FULL join keeps the rows of its dimension that no row matched, and nothing else of the statement may change what matches */
static bool
preserved_ok(const JoinShape *sh)
{
	for (int i = 0; i < sh->nd; i++)
		if (sift_star_preserves_dim(sh->dim[i].mode))
			return sh->nd == 1 && sh->elem == NULL && sh->resid == NIL;
	return true;
}

static bool
shape_of(PlannerInfo *root, Relids base, JoinShape *sh)
{
	if (!make_dims(root, base, sh) || !dim_rels_ok(root, sh) || !equivalence_keys(root, base, sh) || !join_clauses(root, base, sh))
		return false;
	for (int i = 0; i < sh->nd; i++)
		if (sh->dim[i].nkeys == 0)
			return false;
	return needs_lookup(sh) && preserved_ok(sh);
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
probe_col(V6Plan *p, const JoinShape *sh, const Var *v, int dim, SiftV6Col *out)
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

static bool
seed_dims(V6Plan *p, const JoinShape *sh, const SiftV6ElemRte *rte)
{
	SiftV6Ext  *x;

	if (sh->elem != NULL)
	{
		if (!sift_v6_unnest_seed(p, sh->elem, rte, sh->elem_left))
			return false;
		x = p->xseed;
	}
	else
		p->xseed = x = sift_v6_ext_new();
	p->spec.ext = x;
	for (int d = 0; d < sh->nd; d++)
	{
		const JoinDim *jd = &sh->dim[d];
		SiftV6XDim *xd = &x->dim[d];

		xd->mode = jd->mode;
		xd->nkeys = jd->nkeys;
		for (int k = 0; k < jd->nkeys; k++)
		{
			SiftV6Col	col;

			if (!probe_col(p, sh, jd->probe[k], d, &col) || !(jd->mode == SV6J_RANGE_SMALL ? jd->rel->rows <= SIFT_STAR_RANGE_MAX : key_class(jd->key[k]->vartype)) || !rel_outputs(jd->rel, jd->key[k]))
				return false;
			xd->fatt[k] = col.attno;
			xd->ftype[k] = col.typid;
			xd->fsrc[k] = col.src;
			xd->fdim[k] = col.dim;
			xd->fslot[k] = col.slot;
			xd->ktype[k] = jd->key[k]->vartype;
			xd->kcol[k] = jd->key[k]->varattno;
			xd->kvarno[k] = bms_membership(jd->rels) == BMS_SINGLETON ? 0 : jd->key[k]->varno;
		}
		p->dim_rels = lappend(p->dim_rels, jd->rel);
		x->ndims = d + 1;
	}
	return true;
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

static bool
seed_resid(V6Plan *p, const JoinShape *sh)
{
	ResidCtx	c = {.p = p,.x = p->xseed,.ok = true};

	foreach_node(RestrictInfo, ri, sh->resid)
	{
		if (contain_volatile_functions((Node *) ri->clause))
			return false;
		c.x->resid = lappend(c.x->resid, resid_var((Node *) ri->clause, &c));
		if (!c.ok)
			return false;
	}
	return true;
}

static Cost
lookup_cost(const JoinShape *sh, const RelOptInfo *input_rel, Cost *startup)
{
	Cost		total = 0;

	*startup = 0;
	for (int d = 0; d < sh->nd; d++)
	{
		const Path *child = sh->dim[d].rel->cheapest_total_path;

		*startup += child->total_cost;
		total += child->total_cost + sh->dim[d].rel->rows * cpu_operator_cost * 2.0 + input_rel->rows * cpu_operator_cost;
	}
	return total;
}

static void
try_index(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs, const JoinShape *sh, const SiftV6ElemRte *rte, IndexOptInfo *ix)
{
	V6Plan		p = {.root = root,.rel = sh->fact,.out = output_rel,.relid = ix->indexoid,.index = ix};
	Relation	irel;
	Cost		xs,
				xt;
	List	   *children = NIL;

	if (ix->hypothetical || !sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid))
		return;
	irel = index_open(ix->indexoid, AccessShareLock);
	sift_v6_plan_index(&p, irel);
	if (p.ops != NULL && seed_dims(&p, sh, rte) && sift_v6_compile_on(&p, extra, group_exprs) && (sh->elem != NULL ? sift_v6_unnest_ready(&p) : p.spec.nbins == 0) && seed_resid(&p, sh))
	{
		xt = lookup_cost(sh, input_rel, &xs);
		if (sh->elem != NULL)
			xt += sift_v6_unnest_pairs_cost(&p, input_rel);
		for (int d = 0; d < sh->nd; d++)
			children = lappend(children, sh->dim[d].rel->cheapest_total_path);
		sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, children, xs, xt, input_rel->rows);
	}
	index_close(irel, NoLock);
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, Relids base, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem, GroupPathExtraData *extra, List *group_exprs)
{
	JoinShape	sh = {.fact = fact,.elem = elem};
	SiftV6ElemRte rte = {0};

	if (!sift_v6_plan_fact_ok(root, fact) || (elem != NULL && !sift_v6_unnest_rte_joined(root, fact, elem, &rte)) || !shape_of(root, base, &sh))
		return;
	foreach_ptr(IndexOptInfo, ix, fact->indexlist)
		try_index(root, input_rel, output_rel, extra, group_exprs, &sh, &rte, ix);
}

void
sift_v6_join_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs = NIL;
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	const int	nrels = bms_num_members(base);
	int			f = -1;

	if (!sift_v6_plan_query_ok(root, output_rel) || nrels < 2 || nrels > 2 + SIFT_STAR_MAX_DIMS || root->placeholder_list != NIL)
		return;
	group_exprs = sift_v6_group_exprs_of(root);
	while ((f = bms_next_member(base, f)) >= 0)
	{
		RelOptInfo *fact = root->simple_rel_array[f];
		int			e = -1;

		if (fact == NULL)
			continue;
		if (!root->hasLateralRTEs)
			try_fact(root, input_rel, base, output_rel, fact, NULL, (GroupPathExtraData *) extra, group_exprs);
		while (root->hasLateralRTEs && (e = bms_next_member(base, e)) >= 0)
			if (e != f && root->simple_rel_array[e] != NULL)
				try_fact(root, input_rel, base, output_rel, fact, root->simple_rel_array[e], (GroupPathExtraData *) extra, group_exprs);
	}
}
