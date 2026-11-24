/*
 * agg/v6/v6_star_plan.c - the join of a fact table to up to SIFT_STAR_MAX_DIMS small relations as part of the aggregate scan of SiftAggV6. The join is the one the query makes of the relations of
 * the group's input: each relation but the fact table is a dimension, an inner or left join of the fact on equalities of its columns (those the planner knows as equivalences of the query and the ON
 * clauses of the left joins), whose plan (any plan of the relation that takes no parameter) the node reads whole into a hash table before it scans the fact table. A join that is anything else
 * (an inequality, a condition on two dimensions, a full, right, semi or anti join, a lateral relation) is not the node's, and the query keeps the plan of PostgreSQL.
 *
 * The keys and aggregates of the statement may name the fact table's columns and the columns of the dimensions (their rows are looked up in the batch), nothing else: any other qual of the join
 * or the output has no place in the node.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/group/ht.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "col/am/format_ops.h"
#include "agg/plan/format_ops.h"

typedef struct StarDim
{
	RelOptInfo *rel;
	bool		left;			/* the right side of a left join of the query */
	int			nkeys;
	const Var  *fvar[SIFT_STAR_MAX_KEYS];
	const Var  *dvar[SIFT_STAR_MAX_KEYS];
} StarDim;

static const Var *
plain_var(const Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	return n != NULL && IsA(n, Var) && ((const Var *) n)->varlevelsup == 0 ? (const Var *) n : NULL;
}

static bool
int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool
text_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
}

/* the join of the keys is one of the bytes' equality of one class: the same type, or two integer types, or two texts of a collation whose equality is the bytes' */
static bool
keys_agree(const Var *f, const Var *d)
{
	if (text_type(f->vartype) && text_type(d->vartype))
		return sift_text_bytes_equal(f->varcollid) && sift_text_bytes_equal(d->varcollid);
	if (f->vartype == DATEOID || f->vartype == TIMESTAMPOID || f->vartype == TIMESTAMPTZOID)
		return f->vartype == d->vartype;
	return int_type(f->vartype) && int_type(d->vartype);
}

static int
dim_of(const StarDim *dims, int nd, Index rti)
{
	for (int i = 0; i < nd; i++)
		if (dims[i].rel->relid == rti)
			return i;
	return -1;
}

static bool
add_key(StarDim *d, const Var *f, const Var *v)
{
	for (int k = 0; k < d->nkeys; k++)
		if (d->fvar[k]->varattno == f->varattno && d->dvar[k]->varattno == v->varattno)
			return true;
	if (d->nkeys == SIFT_STAR_MAX_KEYS || !keys_agree(f, v))
		return false;
	d->fvar[d->nkeys] = f;
	d->dvar[d->nkeys++] = v;
	return true;
}

/* the equalities the planner made equivalences of the query: every class that has a member of the fact table and one of a dimension joins them; a class that joins dimensions without the fact table, or has a computed member, is not ours */
static bool
equivalence_keys(PlannerInfo *root, Relids base, Index fact, StarDim *dims, int nd)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		const Var  *fvar = NULL;
		Relids		rels = NULL;
		bool		computed = false;

		if (ec->ec_has_const || ec->ec_has_volatile || ec->ec_broken)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = plain_var((Node *) em->em_expr);
			Relids		vr = bms_intersect(pull_varnos(root, (Node *) em->em_expr), base);

			if (em->em_is_child)
				continue;
			rels = bms_add_members(rels, vr);
			computed |= v == NULL && !bms_is_empty(vr);
			if (v != NULL && v->varno == fact && fvar == NULL)
				fvar = v;
		}
		if (bms_membership(rels) != BMS_MULTIPLE)
			continue;
		if (computed || fvar == NULL)
			return false;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = plain_var((Node *) em->em_expr);
			const int	d = v != NULL && !em->em_is_child ? dim_of(dims, nd, v->varno) : -1;

			if (d >= 0 && !dims[d].left && !add_key(&dims[d], fvar, v))
				return false;
		}
	}
	return true;
}

/* an ON clause of a left join, or a clause of an inner one the planner did not make an equivalence of: the equality of a column of the fact table and one of a dimension */
static bool
clause_key(RestrictInfo *ri, Index fact, StarDim *dims, int nd)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const Var  *a,
			   *b;
	int			d;

	if (!IsA(ri->clause, OpExpr) || list_length(op->args) != 2 || ri->has_clone || ri->is_clone || !op_hashjoinable(op->opno, exprType(linitial(op->args))))
		return false;
	a = plain_var(linitial(op->args));
	b = plain_var(lsecond(op->args));
	if (a == NULL || b == NULL)
		return false;
	if (b->varno == fact)
	{
		const Var  *t = a;

		a = b;
		b = t;
	}
	d = a->varno == fact ? dim_of(dims, nd, b->varno) : -1;
	return d >= 0 && ri->is_pushed_down != dims[d].left && add_key(&dims[d], a, b);
}

#define SEEN_STACK	64

/* adds the clause to the open-addressed set of cap slots (a power of 2, more than the clauses added); false when it was in */
static bool
seen_add(const RestrictInfo **tab, uint32 cap, const RestrictInfo *ri)
{
	uint32		at = (uint32) murmurhash64((uint64) (uintptr_t) ri) & (cap - 1);

	for (; tab[at] != NULL; at = (at + 1) & (cap - 1))
		if (tab[at] == ri)
			return false;
	tab[at] = ri;
	return true;
}

static bool
join_clause_keys(PlannerInfo *root, Relids base, Index fact, StarDim *dims, int nd)
{
	const RestrictInfo *stack[SEEN_STACK] = {0};
	const RestrictInfo **seen = stack;
	uint32		cap = SEEN_STACK;
	int			total = 0,
				r = -1;
	bool		ok = true;

	while ((r = bms_next_member(base, r)) >= 0)
		total += list_length(root->simple_rel_array[r]->joininfo);
	if (total * 2 > SEEN_STACK)
	{
		cap = pg_nextpower2_32(total * 2);
		seen = palloc0(cap * sizeof(*seen));
	}
	for (r = -1; ok && (r = bms_next_member(base, r)) >= 0;)
		foreach_node(RestrictInfo, ri, root->simple_rel_array[r]->joininfo)
			if (seen_add(seen, cap, ri) && !clause_key(ri, fact, dims, nd))
			{
				ok = false;
				break;
			}
	if (seen != stack)
		pfree(seen);
	return ok;
}

/* every special join of the query is a left join of one dimension */
static bool
joins_ok(PlannerInfo *root, StarDim *dims, int nd)
{
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		int			d;

		if (sj->jointype != JOIN_LEFT || bms_membership(sj->min_righthand) != BMS_SINGLETON || !bms_is_empty(sj->commute_above_l) || !bms_is_empty(sj->commute_above_r) ||
			!bms_is_empty(sj->commute_below_l) || !bms_is_empty(sj->commute_below_r))
			return false;
		d = dim_of(dims, nd, bms_singleton_member(sj->min_righthand));
		if (d < 0)
			return false;
		dims[d].left = true;
	}
	return true;
}

static bool
dim_rte_ok(PlannerInfo *root, RelOptInfo *rel, uint64 budget)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	if (rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL || rel->cheapest_total_path == NULL || rel->cheapest_total_path->param_info != NULL)
		return false;
	if (rte->rtekind != RTE_RELATION && rte->rtekind != RTE_VALUES && rte->rtekind != RTE_CTE && rte->rtekind != RTE_SUBQUERY)
		return false;
	return rel->rows * (rel->reltarget->width + 48.0) <= (double) budget;
}

static bool
in_target(const RelOptInfo *rel, AttrNumber attno)
{
	foreach_ptr(Node, n, rel->reltarget->exprs)
		if (IsA(n, Var) && ((Var *) n)->varattno == attno)
			return true;
	return false;
}

static bool
seed_dims(V6Plan *p, StarDim *dims, int nd)
{
	SiftV6Ext  *x = sift_v6_ext_new();

	x->ndims = nd;
	for (int d = 0; d < nd; d++)
	{
		SiftV6XDim *xd = &x->dim[d];

		xd->mode = dims[d].left ? SV6J_LEFT_EQUI : SV6J_INNER_EQUI;
		xd->nkeys = dims[d].nkeys;
		for (int k = 0; k < xd->nkeys; k++)
		{
			SiftV6Col	col;

			if (!sift_v6_plan_column(p, (const Expr *) dims[d].fvar[k], false, &col) || !(sift_v6_class(col.typid) == SV6C_I32 || sift_v6_class(col.typid) == SV6C_I64 ||
				sift_v6_class(col.typid) == SV6C_TEXT) || !in_target(dims[d].rel, dims[d].dvar[k]->varattno))
				return false;
			xd->fatt[k] = col.attno;
			xd->ftype[k] = col.typid;
			xd->ktype[k] = dims[d].dvar[k]->vartype;
			xd->kcol[k] = dims[d].dvar[k]->varattno;
		}
		p->dim_rels = lappend(p->dim_rels, dims[d].rel);
	}
	p->xseed = x;
	return true;
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, Relids base, RelOptInfo *output_rel, RelOptInfo *fact, GroupPathExtraData *extra, List *group_exprs)
{
	StarDim		dims[SIFT_STAR_MAX_DIMS] = {0};
	const int	nd = bms_num_members(base) - 1;
	const uint64 budget = Max((uint64) work_mem * 1024 / 2 / nd, 4096);
	int			n = 0,
				r = -1;

	if (!sift_v6_plan_fact_ok(root, fact))
		return;
	while ((r = bms_next_member(base, r)) >= 0)
	{
		RelOptInfo *rel = root->simple_rel_array[r];

		if (rel == fact)
			continue;
		if (!dim_rte_ok(root, rel, budget))
			return;
		dims[n++].rel = rel;
	}
	if (!joins_ok(root, dims, nd) || !equivalence_keys(root, base, fact->relid, dims, nd) || !join_clause_keys(root, base, fact->relid, dims, nd))
		return;
	for (int d = 0; d < nd; d++)
		if (dims[d].nkeys == 0)
			return;
	foreach_ptr(IndexOptInfo, ix, fact->indexlist)
	{
		V6Plan		p = {.root = root,.rel = fact,.out = output_rel,.relid = ix->indexoid,.index = ix};
		Relation	irel;
		List	   *children = NIL;
		Cost		xs = 0,
					xt = 0;

		if (ix->hypothetical || !sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid))
			continue;
		irel = index_open(ix->indexoid, AccessShareLock);
		sift_v6_plan_index(&p, irel);
		if (p.ops != NULL && seed_dims(&p, dims, nd) && sift_v6_compile_on(&p, extra, group_exprs) && p.spec.nbins == 0)
		{
			for (int d = 0; d < nd; d++)
			{
				Path	   *child = dims[d].rel->cheapest_total_path;

				children = lappend(children, child);
				xs += child->total_cost;
				xt += child->total_cost + dims[d].rel->rows * cpu_operator_cost * 2.0 + input_rel->rows * cpu_operator_cost;
			}
			sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, children, xs, xt, input_rel->rows);
		}
		index_close(irel, NoLock);
	}
}

void
sift_v6_star_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs = NIL;
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	const int	nrels = bms_num_members(base);
	int			r = -1;

	if (!sift_v6_plan_query_ok(root, output_rel) || nrels < 2 || nrels > 1 + SIFT_STAR_MAX_DIMS || root->hasLateralRTEs || root->placeholder_list != NIL)
		return;
	foreach_node(SortGroupClause, sgc, root->processed_groupClause)
		group_exprs = lappend(group_exprs, get_sortgroupclause_expr(sgc, root->parse->targetList));
	while ((r = bms_next_member(base, r)) >= 0)
		try_fact(root, input_rel, base, output_rel, root->simple_rel_array[r], (GroupPathExtraData *) extra, group_exprs);
}

bool
sift_v6_star_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = plain_var((const Node *) e);
	SiftV6Ext  *x = p->spec.ext;
	int			d = -1,
				c;
	SiftV6XDim *xd;

	if (x == NULL || x->ndims == 0 || v == NULL || v->vartype == NUMERICOID || sift_v6_class(v->vartype) == SV6C_NONE)
		return false;
	for (int i = 0; i < x->ndims; i++)
		if (((RelOptInfo *) list_nth(p->dim_rels, i))->relid == v->varno)
			d = i;
	if (d < 0)
		return false;
	xd = &x->dim[d];
	if (!in_target(list_nth(p->dim_rels, d), v->varattno))
		return false;
	for (c = 0; c < xd->npay && xd->pcol[c] != v->varattno; c++)
		;
	if (c == xd->npay)
	{
		if (c == SIFT_STAR_MAX_PAY)
			return false;
		xd->pcol[c] = v->varattno;
		xd->ptype[c] = v->vartype;
		xd->npay++;
	}
	*out = (SiftV6Col) {.typid = v->vartype,.src = SV6SRC_DIM,.dim = d,.slot = c};
	return true;
}
