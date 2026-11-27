/*
 * agg/star/plan.c - the join of a fact table to up to SIFT_STAR_MAX_DIMS small relations as part of the aggregate scan of SiftAggV6. The join is the one the query makes of the relations of
 * the group's input: each relation but the fact table is a dimension, an inner or left join of the fact on equalities of its columns (those the planner knows as equivalences of the query and the ON
 * clauses of the left joins), whose plan (any plan of the relation that takes no parameter) the node reads whole into a hash table before it scans the fact table. A join that is anything else
 * (an inequality, a condition on two dimensions, a full, right, semi or anti join, a lateral relation) is not the node's, and the query keeps the plan of PostgreSQL.
 *
 * The keys and aggregates of the statement may name the fact table's columns and the columns of the dimensions (their rows are looked up in the batch), nothing else: any other qual of the join
 * or the output has no place in the node.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "agg/ht/ht.h"
#include "agg/plan/int.h"
#include "agg/refuse.h"
#include "agg/cost/card.h"
#include "agg/datum.h"
#include "agg/dim_keys.h"
#include "agg/ext.h"
#include "agg/join/found.h"
#include "agg/source_rel.h"
#include "agg/star/run.h"

typedef struct StarDim
{
	RelOptInfo *rel;
	Path	   *path;			/* the plan the node reads it with */
	bool		left;			/* the right side of a left join of the query */
	bool		limited;		/* a sub-select with LIMIT or OFFSET: it must fit twice over (dim_rel_readable) */
	double		span;			/* SiftV6XDim.span */
	int			nkeys;
	const Var  *fvar[SIFT_STAR_MAX_KEYS];
	const Var  *dvar[SIFT_STAR_MAX_KEYS];
} StarDim;

const Var *
sift_v6_plain_var(const Node *n)
{
	while (n != NULL && (IsA(n, RelabelType) || (IsA(n, PlaceHolderVar) && bms_is_empty(((const PlaceHolderVar *) n)->phnullingrels))))
		n = IsA(n, RelabelType) ? (const Node *) ((const RelabelType *) n)->arg : (const Node *) ((const PlaceHolderVar *) n)->phexpr;
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

/* the join of the keys is one of the bytes' equality of one class: the same type, or two integer types, or two texts the equality compares by a collation whose equality is the bytes' (inputcollid of the operator) */
bool
sift_v6_key_types_agree(Oid ftype, Oid dtype, Oid inputcollid)
{
	if (text_type(ftype) && text_type(dtype))
		return sift_text_bytes_equal(inputcollid);
	if (ftype == DATEOID || ftype == TIMESTAMPOID || ftype == TIMESTAMPTZOID || ftype == BOOLOID)
		return ftype == dtype;
	return int_type(ftype) && int_type(dtype);
}

/* a key the tables hold: an integer, a bool or a text */
bool
sift_v6_key_type_ok(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_BOOL || cls == SV6C_TEXT;
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
add_key(StarDim *d, const Var *f, const Var *v, Oid inputcollid)
{
	for (int k = 0; k < d->nkeys; k++)
		if (d->fvar[k]->varattno == f->varattno && d->dvar[k]->varattno == v->varattno)
			return true;
	if (d->nkeys == SIFT_STAR_MAX_KEYS || !sift_v6_key_types_agree(f->vartype, v->vartype, inputcollid))
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
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
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
			const Var  *v = sift_v6_plain_var((Node *) em->em_expr);
			const int	d = v != NULL && !em->em_is_child ? dim_of(dims, nd, v->varno) : -1;

			if (d >= 0 && !dims[d].left && !add_key(&dims[d], fvar, v, ec->ec_collation))
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
	a = sift_v6_plain_var(linitial(op->args));
	b = sift_v6_plain_var(lsecond(op->args));
	if (a == NULL || b == NULL)
		return false;
	if (b->varno == fact)
	{
		const Var  *t = a;

		a = b;
		b = t;
	}
	d = a->varno == fact ? dim_of(dims, nd, b->varno) : -1;
	return d >= 0 && ri->is_pushed_down != dims[d].left && add_key(&dims[d], a, b, op->inputcollid);
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

/*
 * the rows of the base relation b change from one read to the next (a volatile function), or may (a sub-select that keeps some of its rows with LIMIT or OFFSET: *limited); a sub-select is the query
 * its subroot planned (none: it was proven empty)
 */
static bool
reads_volatile(PlannerInfo *root, const RelOptInfo *b, bool *limited)
{
	const Query *q;

	switch (b->rtekind)
	{
		case RTE_FUNCTION:
			return contain_volatile_functions((Node *) planner_rt_fetch(b->relid, root)->functions);
		case RTE_VALUES:
			return contain_volatile_functions((Node *) planner_rt_fetch(b->relid, root)->values_lists);
		case RTE_SUBQUERY:
			if (b->subroot == NULL)
				return false;
			q = b->subroot->parse;
			*limited |= q->limitCount != NULL || q->limitOffset != NULL;
			return contain_volatile_functions((Node *) q);
		default:
			return false;
	}
}

/*
 * a relation whose plan (any plan of it that takes no parameter) the node reads whole: a table, a VALUES list, a CTE, a sub-select or a function, or the result of a join of such relations (a join
 * tree under a semi or anti join). The node reads it again for each part of its keys when it does not fit the budget, so its rows must be the same at every read: nothing of it is volatile, and
 * a sub-select with LIMIT or OFFSET fits twice over, so that it is read once
 */
static bool
dim_rel_readable(PlannerInfo *root, RelOptInfo *rel, bool *limited)
{
	int			r = -1;

	if (rel->lateral_relids != NULL || rel->cheapest_total_path == NULL || rel->cheapest_total_path->param_info != NULL)
		return false;
	if (rel->reloptkind == RELOPT_BASEREL)
	{
		if ((rel->rtekind != RTE_RELATION && rel->rtekind != RTE_VALUES && rel->rtekind != RTE_CTE && rel->rtekind != RTE_SUBQUERY && rel->rtekind != RTE_FUNCTION) ||
			reads_volatile(root, rel, limited))
			return false;
	}
	else if (rel->reloptkind != RELOPT_JOINREL)
		return false;
	while (rel->reloptkind == RELOPT_JOINREL && (r = bms_next_member(rel->relids, r)) >= 0)
		if (root->simple_rel_array[r] != NULL && reads_volatile(root, root->simple_rel_array[r], limited))
			return false;
	return true;
}

/* the bytes the hash form of the table of the relation takes */
static double
dim_hash_bytes(const RelOptInfo *rel)
{
	return rel->rows * (rel->reltarget->width + 48.0);
}

bool
sift_v6_dim_rel_ok(PlannerInfo *root, RelOptInfo *rel, uint64 budget)
{
	bool		limited = false;

	return dim_rel_readable(root, rel, &limited) && dim_hash_bytes(rel) * (limited ? 2 : 1) <= (double) budget;
}

static int64
key_int(Oid typid, Datum v)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(v);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(v);
		default:
			return DatumGetInt64(v);
	}
}

/* the values the one integer key of the dimension spans when its statistics say the rows of the table have different keys: those of its histogram and most common values (0: not known) */
static double
key_span(PlannerInfo *root, const StarDim *d)
{
	const Var  *v = d->dvar[0];
	const int	kinds[] = {STATISTIC_KIND_HISTOGRAM, STATISTIC_KIND_MCV};
	VariableStatData vd;
	bool		isdefault = false;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	if (d->nkeys != 1 || d->rel->reloptkind != RELOPT_BASEREL || d->rel->rtekind != RTE_RELATION || (v->vartype != INT2OID && v->vartype != INT4OID && v->vartype != INT8OID && v->vartype != DATEOID))
		return 0;
	examine_variable(root, (Node *) v, 0, &vd);
	if (HeapTupleIsValid(vd.statsTuple) && vd.rel != NULL && (vd.isunique || (get_variable_numdistinct(&vd, &isdefault) >= vd.rel->tuples && !isdefault)))
		for (int i = 0; i < lengthof(kinds); i++)
		{
			AttStatsSlot slot;

			if (!get_attstatsslot(&slot, vd.statsTuple, kinds[i], InvalidOid, ATTSTATSSLOT_VALUES))
				continue;
			for (int j = 0; j < slot.nvalues; j++)
			{
				lo = Min(lo, key_int(v->vartype, slot.values[j]));
				hi = Max(hi, key_int(v->vartype, slot.values[j]));
			}
			free_attstatsslot(&slot);
		}
	ReleaseVariableStats(vd);
	return lo <= hi ? Max((double) hi - (double) lo + 1, d->rel->rows) : 0;
}

/* the table of the dimension fits the budget: in the dense form when its key allows it (agg/star/dense.h), else in the hash form */
static bool
dim_fits(PlannerInfo *root, StarDim *d, uint64 budget)
{
	Oid			ptypes[SIFT_STAR_MAX_PAY];
	int			npay = 0;
	double		bytes = dim_hash_bytes(d->rel);

	d->span = key_span(root, d);
	foreach_ptr(Node, n, d->rel->reltarget->exprs)
		if (npay < SIFT_STAR_MAX_PAY && !(IsA(n, Var) && ((const Var *) n)->varattno == d->dvar[0]->varattno))
			ptypes[npay++] = exprType(n);
	if (d->span > 0)
	{
		const double dense = sift_v6_star_dim_bytes(d->left ? SV6J_LEFT_EQUI : SV6J_INNER_EQUI, d->dvar[0]->vartype, ptypes, npay, d->rel->rows, d->rel->reltarget->width, d->span, budget);

		if (dense >= 0)
			bytes = Min(bytes, dense);
		else
			d->span = 0;
	}
	return bytes * (d->limited ? 2 : 1) <= (double) budget;
}

bool
sift_v6_in_target(const RelOptInfo *rel, const Var *v)
{
	foreach_ptr(Node, n, rel->reltarget->exprs)
	{
		const Var  *t = sift_v6_plain_var(n);

		if (t != NULL && t->varattno == v->varattno && t->varno == v->varno)
			return true;
	}
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

			if (!sift_v6_plan_column(p, (const Expr *) dims[d].fvar[k], false, &col) || !sift_v6_key_type_ok(col.typid) ||
				!sift_v6_in_target(dims[d].rel, dims[d].dvar[k]))
				return false;
			xd->fatt[k] = col.attno;
			xd->ftype[k] = col.typid;
			xd->ktype[k] = dims[d].dvar[k]->vartype;
			xd->kcol[k] = dims[d].dvar[k]->varattno;
		}
		xd->span = dims[d].span;
		p->dim_rels = lappend(p->dim_rels, dims[d].rel);
	}
	p->xseed = x;
	return true;
}

/* what the join is for: the groups of an aggregate, or the best K rows of the join (topk > 0) */
typedef struct StarGoal
{
	GroupPathExtraData *extra;
	List	   *group_exprs;
	uint32		topk;
} StarGoal;

/* the statement compiled against the fact index and the dimensions seeded in it */
static bool
star_compile(V6Plan *p, const StarGoal *goal)
{
	if (goal->topk > 0)
		return sift_v6_rows_compile(p, goal->topk);
	return sift_v6_compile_on(p, goal->extra, goal->group_exprs);
}

static void
star_add(V6Plan *p, const StarGoal *goal, double input_rows, List *children, Cost startup, Cost total)
{
	if (goal->topk > 0)
	{
		Cost		open,
					scan,
					dims = input_rows * cpu_operator_cost * list_length(children);

		sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, &scan);
		foreach_ptr(Path, child, children)
			dims += Max(child->parent->tuples, child->rows) * (cpu_tuple_cost + cpu_operator_cost * 2.0);
		sift_v6_rows_add(p, goal->topk, scan, children, dims, dims);
	}
	else
		sift_v6_add_join_path(p, goal->group_exprs, sift_v6_having(goal->extra), children, startup, total, input_rows);
}

/* the statement, its dimensions and what the join of them to the fact makes of its input */
typedef struct StarStmt
{
	StarDim    *dims;
	int			nd;
	const StarGoal *goal;
	double		input_rows;
} StarStmt;

static bool
star_compile_fn(V6Plan *p, const void *arg)
{
	const StarStmt *s = arg;

	return seed_dims(p, s->dims, s->nd) && star_compile(p, s->goal);
}

static void
star_add_fn(V6Plan *p, const void *arg)
{
	const StarStmt *s = arg;
	List	   *children = NIL;
	Cost		xs = 0,
				xt = 0;

	for (int d = 0; d < s->nd; d++)
	{
		Path	   *child = s->dims[d].path;
		const bool	found = !p->is_table && sift_v6_found_planned(p->root, p->ops, p->relid, p->spec.ext, d, child);

		children = lappend(children, child);
		xs += found ? 0 : sift_v6_dim_cost(child);
		xt += (found ? 0 : sift_v6_dim_cost(child) + child->rows * cpu_operator_cost * 2.0) + s->input_rows * cpu_operator_cost;
	}
	star_add(p, s->goal, s->input_rows, children, xs, xt);
}

/* the plan each dimension is read with: its cheapest, or the one that probes it by the constant keys of the fact table's filter */
static void
plan_dims(PlannerInfo *root, const RelOptInfo *fact, StarDim *dims, int nd)
{
	for (int d = 0; d < nd; d++)
	{
		AttrNumber	probe_attno[SIFT_STAR_MAX_KEYS];

		for (int k = 0; k < dims[d].nkeys; k++)
			probe_attno[k] = dims[d].fvar[k]->varattno;
		dims[d].path = sift_v6_dim_path(root, fact, dims[d].rel, dims[d].left ? SV6J_LEFT_EQUI : SV6J_INNER_EQUI, dims[d].nkeys, probe_attno, dims[d].dvar);
	}
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, Relids base, RelOptInfo *output_rel, RelOptInfo *fact, const StarGoal *goal)
{
	StarDim		dims[SIFT_STAR_MAX_DIMS] = {0};
	const int	nd = bms_num_members(base) - 1;
	const uint64 budget = sift_v6_star_budget(nd);
	const StarStmt stmt = {dims, nd, goal, input_rel->rows};
	int			n = 0,
				r = -1;

	if (!(goal->topk > 0 ? sift_v6_plan_fact_ok(root, fact) : sift_v6_join_fact_ok(root, fact)))
		return;
	while ((r = bms_next_member(base, r)) >= 0)
	{
		RelOptInfo *rel = root->simple_rel_array[r];

		if (rel == fact)
			continue;
		if (!dim_rel_readable(root, rel, &dims[n].limited))
			return;
		dims[n++].rel = rel;
	}
	if (!joins_ok(root, dims, nd) || !equivalence_keys(root, base, fact->relid, dims, nd) || !join_clause_keys(root, base, fact->relid, dims, nd))
		return;
	for (int d = 0; d < nd; d++)
		if (dims[d].nkeys == 0 || !dim_fits(root, &dims[d], budget))
			return;
	plan_dims(root, fact, dims, nd);
	sift_v6_fact_paths(root, fact, output_rel, star_compile_fn, star_add_fn, &stmt);
}

/* a placeholder the join can null, or of an expression, is evaluated where the star does not read it; one of a bare column no join nulls (an output of a sub-select pulled up under grouping sets)
 * is that column */
static bool
placeholders_ok(const PlannerInfo *root)
{
	foreach_node(PlaceHolderInfo, phi, root->placeholder_list)
		if (sift_v6_plain_var((const Node *) phi->ph_var) == NULL)
			return false;
	return true;
}

static void
try_facts(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, const StarGoal *goal)
{
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	const int	nrels = bms_num_members(base);
	int			r = -1;

	if (nrels < 2 || nrels > 1 + SIFT_STAR_MAX_DIMS || root->hasLateralRTEs || !placeholders_ok(root))
		return;
	while ((r = bms_next_member(base, r)) >= 0)
		try_fact(root, input_rel, base, output_rel, root->simple_rel_array[r], goal);
}

void
sift_v6_star_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	const StarGoal goal = {.extra = extra,.group_exprs = group_exprs};
	const int	nrels = bms_num_members(sift_v6_plan_base_relids(root, input_rel));

	if (!sift_v6_plan_query_ok(root, output_rel))
		return;
	if (nrels > 1 + SIFT_STAR_MAX_DIMS + root->hasLateralRTEs)
		sift_agg_arm_refused_by("single", AGG_REFUSE_JOIN, psprintf("%d tables, the fact table and up to %d it looks up", nrels, SIFT_STAR_MAX_DIMS));
	try_facts(root, input_rel, output_rel, &goal);
}

void
sift_v6_star_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k)
{
	const StarGoal goal = {.topk = k};

	try_facts(root, input_rel, output_rel, &goal);
}

bool
sift_v6_star_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = sift_v6_plain_var((const Node *) e);
	SiftV6Ext  *x = p->spec.ext;
	int			d = -1,
				c;
	SiftV6XDim *xd;
	Index		varno;

	if (x == NULL || x->ndims == 0 || v == NULL || get_typlen(v->vartype) == -2)
		return false;
	for (int i = 0; i < x->ndims; i++)
		if (bms_is_member(v->varno, ((RelOptInfo *) list_nth(p->dim_rels, i))->relids))
			d = i;
	if (d < 0 || !sift_v6_in_target(list_nth(p->dim_rels, d), v))
		return false;
	xd = &x->dim[d];
	varno = ((RelOptInfo *) list_nth(p->dim_rels, d))->relid == 0 ? v->varno : 0;
	for (c = 0; c < xd->npay && (xd->pcol[c] != v->varattno || xd->pvarno[c] != varno); c++)
		;
	if (c == xd->npay)
	{
		if (c == SIFT_STAR_MAX_PAY)
			return false;
		xd->pcol[c] = v->varattno;
		xd->ptype[c] = v->vartype;
		xd->pvarno[c] = varno;
		xd->npay++;
	}
	*out = (SiftV6Col) {.typid = v->vartype,.scale = sift_v6_type_native(v->vartype) ? 0 : SIFT_V6_SCALE_DATUM,.src = SV6SRC_DIM,.dim = d,.slot = c};
	return true;
}
