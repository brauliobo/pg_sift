/*
 * agg/v6/plan.c - the path of SiftAggV6: an aggregate of one table that has a sift_col index of format major 2 holding every column the query reads. Its WHERE clauses are the
 * quals of the index scan the scan of the index decides on the stored values; the groups are plain columns, the aggregates count, sum, avg, min, max and count(DISTINCT) of one column.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/timestamp.h"

#include "agg/agg.h"
#include "agg/group/ht.h"
#include "agg/plan/format_ops.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_par_pg.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_join_push.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/v6/v6_expr_num.h"
#include "agg/v6/v6_argext.h"
#include "agg/v6/v6_argext_sql.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_cost.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_gen.h"
#include "agg/v6/v6_pct.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_unnest_exists.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "agg/v6/v6_join_plan.h"
#include "agg/v6/v6_plan_shape.h"
#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_qual.h"
#include "col/am.h"
#include "col/am/format_ops.h"
#include "col/am/preds.h"
#include "col/col.h"
#include "table/dispatch.h"
#include "core/catname.h"

SiftV6Rel
sift_v6_plan_rel(const V6Plan *p)
{
	return (SiftV6Rel) {.relid = p->rel->relid,.index = p->index,.colrel = p->colrel,.ops = p->ops};
}

static const CustomPathMethods v6_path_methods = {"SiftAggV6", sift_agg_v6_plan_path, NULL};

/* The attribute of the host a column of the table is: the column of the index, or the attribute of the table; 0: the host does not hold it */
static int
host_attno(const V6Plan *p, AttrNumber attno)
{
	if (p->is_table)
		return attno > 0 && attno <= p->tsch.natts ? attno : 0;
	for (int i = 0; attno > 0 && i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == attno)
			return i + 1;
	return 0;
}

static const SiftColSchema *
host_schema(const V6Plan *p)
{
	return p->is_table ? p->tsch.leaves : p->colrel->schema;
}

static int
host_first(const V6Plan *p, int att)
{
	return p->is_table ? p->tsch.att_first[att] : p->colrel->first[att];
}

static int
host_nleaves(const V6Plan *p, int att)
{
	return p->is_table ? p->tsch.att_nleaves[att] : p->colrel->nleaves[att];
}

Oid
sift_v6_plan_opfamily(const V6Plan *p, int att)
{
	return p->is_table ? p->tsch.att_opfamily[att] : p->index->opfamily[att - 1];
}

static bool
leaf_class_ok(const SiftColDef *d, int nleaves, SiftV6Class cls)
{
	if (nleaves != 1)
		return false;
	if (d->lane != 0 || d->kind != SIFT_COLK_SCALAR || (d->flags & (SIFT_COLD_LOSSY | SIFT_COLD_DERIVED)) || d->path[0] != '\0')
		return false;
	switch (cls)
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			return d->store == SIFT_COLS_INT;
		case SV6C_F64:
			return d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;
		case SV6C_TEXT:
			return d->store == SIFT_COLS_STR;
		default:
			return false;
	}
}

bool
sift_v6_leaf_holds(const SiftColRel *r, int att, SiftV6Class cls)
{
	return leaf_class_ok(&r->schema->cols[r->first[att]], r->nleaves[att], cls);
}

bool
sift_v6_table_leaf_holds(const SiftStoreSchema *table, int att, SiftV6Class cls)
{
	return att > 0 && att <= table->natts && leaf_class_ok(&table->leaves->cols[table->att_first[att]], table->att_nleaves[att], cls);
}

bool
sift_v6_plan_table(V6Plan *p, Oid relid)
{
	p->ops = sift_store_ops_for_scan(relid, true);
	p->is_table = p->ops != NULL && p->ops->schema(relid, true, &p->tsch);
	return p->is_table;
}

void
sift_v6_plan_index(V6Plan *p, Relation irel)
{
	p->colrel = sift_col_rel(irel);
	p->ops = p->colrel->stale ? NULL : sift_store_ops_for_scan(RelationGetRelid(irel), false);
}

static bool
column_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || (v->vartype == NUMERICOID && !numeric_ok))
		return false;
	if (v->varattno >= SIFT_V6_JTYPE_VATT)
	{
		const SiftV6Rel rel = sift_v6_plan_rel(p);

		return !p->is_table && sift_v6_json_type_col(&rel, v, out);
	}
	att = host_attno(p, v->varattno);
	if (att == 0 || !leaf_class_ok(&host_schema(p)->cols[host_first(p, att)], host_nleaves(p, att), sift_v6_class(v->vartype)))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = v->vartype,.scale = v->vartype == NUMERICOID ? host_schema(p)->cols[host_first(p, att)].scale : 0};
	return true;
}

bool
sift_v6_plan_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(p, e, numeric_ok, out);
}

/* The host holds attribute att whole in leaves the scan decodes as a datum: a table keeps the datum of every attribute in one cell, an index the leaves of its type (sift_col_def_datum_ok) */
static bool
datum_leaf_ok(const V6Plan *p, int att)
{
	return p->is_table ? host_nleaves(p, att) == 1 : sift_col_def_datum_ok(&host_schema(p)->cols[host_first(p, att)], host_nleaves(p, att));
}

/* A column of a type no native vector holds, or a numeric of any form: its values are read as datums, so a NaN, an infinity or a numeric of any digits is the value it is */
static bool
datum_column_of(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || sift_v6_type_native(v->vartype) || v->vartype == JSONBOID)
		return false;
	att = host_attno(p, v->varattno);
	if (att == 0 || !datum_leaf_ok(p, att))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = v->vartype,.scale = SIFT_V6_SCALE_DATUM};
	return true;
}

/* A text column of a sift_table: its cells are the datums, so its bytes are read after the header; no scan key may compare them (the leaf is lossy) */
static bool
table_text_of(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (!p->is_table || e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || sift_v6_class(v->vartype) != SV6C_TEXT)
		return false;
	att = host_attno(p, v->varattno);
	if (att == 0 || host_nleaves(p, att) != 1)
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = v->vartype};
	return true;
}

/* A column the scan reads as a value of any type: the columns of the kernels, the text of a table, and the datums */
bool
sift_v6_plan_read_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(p, e, numeric_ok, out) || table_text_of(p, e, out) || datum_column_of(p, e, out);
}

Expr *
sift_v6_plan_type_vars(const V6Plan *p, const Expr *e)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return p->is_table || p->xseed != NULL ? (Expr *) e : sift_v6_json_type_vars(&rel, e);
}

/* An expression of the query that is a column of the index (its indexprs entry, which PostgreSQL has built over the same range table entry): the scan reads what the index stored of it */
static bool
index_expr_of(const V6Plan *p, const Expr *e, SiftV6Col *out)
{
	int			k = 0;
	const Oid	typid = exprType((const Node *) e);

	if (p->index == NULL || p->index->indexprs == NIL || typid == NUMERICOID || sift_v6_class(typid) == SV6C_NONE)
		return false;
	for (int i = 0; i < p->index->ncolumns; i++)
	{
		if (p->index->indexkeys[i] != 0)
			continue;
		if (equal(e, list_nth(p->index->indexprs, k++)))
		{
			if (!leaf_class_ok(&host_schema(p)->cols[host_first(p, i + 1)], host_nleaves(p, i + 1), sift_v6_class(typid)))
				return false;
			*out = (SiftV6Col) {.attno = i + 1,.typid = typid};
			return true;
		}
	}
	return false;
}

/* An expression of the query that is the generation expression of a stored generated column of the table: the scan reads the column, which holds what the expression gives */
static bool
generated_expr_of(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	Relation	rel = p->is_table && !IsA(e, Var) ? table_open(p->relid, NoLock) : NULL;
	bool		found = false;

	for (int a = 1; rel != NULL && a <= RelationGetNumberOfAttributes(rel) && !found; a++)
	{
		const Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), a - 1);
		Node	   *gen;

		if (att->attgenerated != ATTRIBUTE_GENERATED_STORED || att->attisdropped)
			continue;
		gen = (Node *) expression_planner((Expr *) build_column_default(rel, a));
		ChangeVarNodes(gen, 1, p->rel->relid, 0);
		found = equal(e, gen) &&
			column_of(p, (Expr *) makeVar(p->rel->relid, a, att->atttypid, att->atttypmod, att->attcollation, 0), false, out);
	}
	if (rel != NULL)
		table_close(rel, NoLock);
	return found;
}

/* A column the scan reads as a value: a column of the index, the text or the float8 of a key of a jsonb document, a payload column of a dimension or the element of the unnest */
static bool
read_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return column_of(p, e, numeric_ok, out) || index_expr_of(p, e, out) || generated_expr_of(p, e, out) ||
		(!p->is_table && (sift_v6_json_read(&rel, e, out) ||
							  (exprType((Node *) e) != NUMERICOID && (sift_v6_star_column(p, e, out) || sift_v6_unnest_column(p, e, out))))) ||
		table_text_of(p, e, out) || datum_column_of(p, e, out);
}

static bool
resolve_column(void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(ctx, e, numeric_ok, out) || sift_v6_unnest_column(ctx, e, out);
}

/* the columns of an expression PostgreSQL computes: a numeric and a type the native vectors do not hold are datums, so a NaN is the value it is */
static bool
resolve_datum_column(void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return datum_column_of(ctx, e, out) || sift_v6_plan_read_column(ctx, e, numeric_ok, out) || sift_v6_unnest_column(ctx, e, out);
}

static int
add_derive(V6Plan *p, const SiftV6Derive *d)
{
	for (int i = 0; i < p->spec.nderive; i++)
		if (memcmp(&p->spec.derive[i], d, sizeof(SiftV6Derive)) == 0)
			return i + 1;
	if (p->spec.nderive == SIFT_V6_MAX_DERIVE)
		return 0;
	p->spec.derive[p->spec.nderive] = *d;
	return ++p->spec.nderive;
}

/* e is a key or argument the scan computes (v6_plan_rewrite.h): col is its column, typed as e */
static bool
derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col)
{
	const SiftV6Resolver rs = {resolve_column, p, p->xseed != NULL && p->xseed->unnest};
	SiftV6Derive d;
	Oid			restype;
	int			at;

	if (!sift_v6_derive_of(&rs, e, key, &d, &restype) || (at = add_derive(p, &d)) == 0)
		return false;
	*col = (SiftV6Col) {.attno = d.src.attno,.typid = restype,.scale = d.kind == SVD_NUM_EXPR ? (int) d.aux : 0,.derive = at};
	return true;
}

/* e is a key or argument PostgreSQL's executor computes from columns of the scan (v6_pg_expr.h): col is its column, typed as e; an argument has a native vector, a key may be of any type that can be grouped */
bool
sift_v6_pg_derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col)
{
	const SiftV6Resolver rs = {resolve_datum_column, p, false, p->elem_rel != NULL ? p->elem_rel->relid : 0};
	SiftV6Derive d = {0};
	Oid			restype;
	int			at;

	if ((p->xseed != NULL && !p->xseed->unnest) || !sift_v6_pgexpr_derive_of(&rs, p->rel->relid, e, &d, &restype, &p->spec.pgexpr) ||
		!(sift_v6_type_native(restype) || (p->xseed == NULL && key && sift_v6_key_generic(restype) && sift_genkey_supported(restype))) || (at = add_derive(p, &d)) == 0)
		return false;
	*col = (SiftV6Col) {.attno = d.src.attno,.typid = restype,.derive = at,.collid = exprCollation((Node *) e)};
	return true;
}

static void
scan_cost(const V6Plan *p, double groups, Cost *startup, Cost *total)
{
	if (!p->is_table)
		sift_v6_agg_cost(p->root, p->rel, p->index, &p->spec, groups, startup, total);
	else
	{
		const SiftV6CostIn in = {.tuples = p->rel->tuples,.rows = p->rel->rows,.f_prune = sift_v6_clause_prune(p->root, p->rel->baserestrictinfo),
		.groups = groups,.ncols_read = sift_v6_spec_ncols_read(&p->spec)};

		sift_v6_scan_cost(&in, startup, total);
	}
	if (p->text != NULL)
	{
		double		touch = 1.0,
					text_cost = sift_v6_text_cost(p->root, p->rel, p->text, p->text_qual, p->text_query, &touch);

		/* a term the text index does not hold touches no granule, and the scan still pays for its opening */
		*total = Max(*total * touch + text_cost, *startup);
	}
}

/* A column of the scan's own relation as the member of scan_vars it is (-1: e is not one: the other entries are told apart by equal()) */
static int
scan_var_bit(const Expr *e)
{
	const Var  *v = (const Var *) e;

	return IsA(e, Var) && v->varlevelsup == 0 && bms_is_empty(v->varnullingrels) && v->varno < 1024 ? (int) v->varno * 2048 + v->varattno - FirstLowInvalidHeapAttributeNumber : -1;
}

static bool
scan_tlist_has(const V6Plan *p, const Expr *e)
{
	const int	bit = scan_var_bit(e);

	if (bit >= 0)
		return bms_is_member(bit, p->scan_vars);
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (equal(te->expr, e))
			return true;
	return false;
}

static void
add_scan_var(V6Plan *p, Expr *e)
{
	const int	bit = scan_var_bit(e);

	if (scan_tlist_has(p, e))
		return;
	if (bit >= 0)
		p->scan_vars = bms_add_member(p->scan_vars, bit);
	p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry(copyObject(e), list_length(p->scan_tlist) + 1, NULL, false));
}

/* Room for one more aggregate and function in the spec: the arrays double */
static void
spec_room(V6Plan *p, bool func)
{
	int		   *cap = func ? &p->cap_funcs : &p->cap_aggs;
	const int	n = func ? p->spec.nfuncs : p->spec.naggs;

	if (n < *cap)
		return;
	*cap = Max(*cap * 2, 8);
	if (func)
		SIFT_REGROW(p->spec.func, (Size) *cap * sizeof(SiftV6Func));
	else
		SIFT_REGROW(p->spec.agg, (Size) *cap * sizeof(SiftV6Agg));
}

/* COUNT(DISTINCT (a, b, ..)): the columns of the row, each one the exact set of v6_distinct.h can hold */
static bool
compile_distinct_row(V6Plan *p, const RowExpr *row, SiftV6Agg *g)
{
	int			n = 0;

	if (row->args == NIL || list_length(row->args) > SIFT_V6_MAX_DCOLS)
		return false;
	foreach_ptr(Expr, e, row->args)
	{
		SiftV6Col  *col = n == 0 ? &g->arg : &g->xcol[n - 1];

		if (!column_of(p, e, true, col) || !sift_v6_kind_takes(SV6_COUNT_DISTINCT, col->typid) ||
			(sift_v6_class(col->typid) == SV6C_TEXT && !sift_text_bytes_equal(exprCollation((Node *) e))))
			return false;
		n++;
	}
	g->nxcols = n - 1;
	return true;
}

/* the kind of an aggregate of the statement: percentile_cont only where the groups are those of an unnest, which keeps the values of a group in one arena (v6_pct.h) */
static bool
agg_kind_of(const V6Plan *p, const Aggref *unfiltered, SiftV6Agg *g)
{
	if (p->xseed != NULL && sift_v6_pct_cont_of(unfiltered, &g->frac))
	{
		g->kind = SV6_PCT_CONT;
		return true;
	}
	return sift_v6_agg_kind(unfiltered, &g->kind);
}

/* a is the aggregate the scan computes, and shape the one the plan's tlist names: the same unless the filter of shape is an expression of the scan's argument (compile_agg) */
static bool
compile_agg_native(V6Plan *p, Aggref *a, const Aggref *shape)
{
	SiftV6Agg	g = {.restype = a->aggtype};
	Aggref		unfiltered = *a;
	SiftV6Class cls;
	Oid			coll;

	unfiltered.aggfilter = NULL;
	if (!agg_kind_of(p, &unfiltered, &g) || (a->aggfilter != NULL && !sift_v6_compile_filter(p, a->aggfilter, &g.fclause)))
		return false;
	if (g.kind == SV6_COUNT_DISTINCT && list_length(a->args) == 1 && IsA(((TargetEntry *) linitial(a->args))->expr, RowExpr))
	{
		if (!compile_distinct_row(p, (const RowExpr *) ((TargetEntry *) linitial(a->args))->expr, &g))
			return false;
	}
	else if (g.kind != SV6_COUNT_STAR)
	{
		const bool	two = g.kind == SV6_ARG_MAX || g.kind == SV6_ARG_MIN;

		Expr	   *arg = list_length(a->args) > 0 ? sift_v6_plan_type_vars(p, ((TargetEntry *) linitial(a->args))->expr) : NULL;
		const bool	score = !two && !sift_v6_kind_distinct(g.kind) && sift_v6_score_col(p, arg, &g.arg);
		const bool	derived_ok = !two && !sift_v6_kind_valued(g.kind);

		if (list_length(a->args) != (two ? 2 : 1) ||
			!(score || ((read_of(p, arg, !two, &g.arg) || (derived_ok && (derived_col(p, arg, false, &g.arg) || sift_v6_pg_derived_col(p, arg, false, &g.arg)))) && sift_v6_kind_takes(g.kind, g.arg.typid))) ||
			sift_v6_col_class(&g.arg) == SV6C_DATUM)
			return false;
		if (g.arg.derive > 0 && p->spec.derive[g.arg.derive - 1].kind == SVD_PG_EXPR && g.kind >= SV6_STDDEV_POP && g.kind <= SV6_VAR_SAMP)
			return false;
		if (g.arg.derive > 0 && p->spec.derive[g.arg.derive - 1].kind == SVD_NUM_EXPR && g.kind != SV6_SUM && g.kind != SV6_AVG && g.kind != SV6_COUNT)
			return false;
		if (two && (!column_of(p, ((TargetEntry *) lsecond(a->args))->expr, false, &g.arg2) || sift_v6_class(g.arg2.typid) == SV6C_BOOL || sift_v6_class(g.arg2.typid) == SV6C_TEXT))
			return false;
		cls = sift_v6_class(g.arg.typid);
		coll = exprCollation((Node *) ((TargetEntry *) linitial(a->args))->expr);
		if (cls == SV6C_TEXT && !((g.kind == SV6_MIN || g.kind == SV6_MAX) ? sift_collate_is_c(coll) : sift_text_bytes_equal(coll)))
			return false;
	}
	spec_room(p, false);
	p->spec.agg[p->spec.naggs++] = g;
	add_scan_var(p, (Expr *) shape);
	return true;
}

/* any other aggregate, and the ones whose arguments or collation the kernels do not take: PostgreSQL's own functions over the values the scan decodes (v6_gen.h) */
static bool
compile_agg_gen(V6Plan *p, Aggref *a)
{
	SiftV6Agg	g;
	Aggref		unfiltered = *a;

	unfiltered.aggfilter = NULL;
	if (!sift_v6_gen_compile(p, &unfiltered, &g) || (a->aggfilter != NULL && !sift_v6_compile_filter(p, a->aggfilter, &g.fclause)))
		return false;
	p->spec.genref = lappend(p->spec.genref, copyObject(&unfiltered));
	spec_room(p, false);
	p->spec.agg[p->spec.naggs++] = g;
	add_scan_var(p, (Expr *) a);
	return true;
}

static bool
compile_agg(V6Plan *p, Aggref *a)
{
	const int	nderive = p->spec.nderive;
	Aggref	   *on_case = sift_vn_filter_to_case(a);

	if (compile_agg_native(p, a, a))
		return true;
	p->spec.nderive = nderive;
	if (on_case != NULL && compile_agg_native(p, on_case, a))
		return true;
	p->spec.nderive = nderive;
	return compile_agg_gen(p, a);
}

static int
key_index_of(const V6Plan *p, Index ref)
{
	int			i = 0;

	foreach_node(SortGroupClause, sgc, p->root->processed_groupClause)
	{
		if (sgc->tleSortGroupRef == ref)
			return i;
		i++;
	}
	return -1;
}

static bool
compile_grouping(V6Plan *p, GroupingFunc *gf)
{
	SiftV6Func	f = {.nargs = list_length(gf->refs)};
	int			i = 0;

	if (f.nargs > SIFT_V6_MAX_KEYS || gf->agglevelsup != 0)
		return false;
	foreach_int(ref, gf->refs)
	{
		if ((f.key[i] = key_index_of(p, ref)) < 0)
			return false;
		i++;
	}
	spec_room(p, true);
	p->spec.func[p->spec.nfuncs] = f;
	spec_room(p, false);
	p->spec.agg[p->spec.naggs++] = (SiftV6Agg) {.kind = SV6_GROUPING,.arg = {.attno = p->spec.nfuncs++},.restype = INT4OID};
	add_scan_var(p, (Expr *) gf);
	return true;
}

static bool
compile_aggs(V6Plan *p, Node *e)
{
	foreach_ptr(Node, n, pull_var_clause(e, PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS))
	{
		if (!IsA(n, Aggref) && !IsA(n, GroupingFunc))
			continue;
		if (!scan_tlist_has(p, (Expr *) n) && !(IsA(n, Aggref) ? compile_agg(p, (Aggref *) n) : compile_grouping(p, (GroupingFunc *) n)))
			return false;
	}
	return true;
}

static bool
compile_sets(V6Plan *p)
{
	List	   *sets = p->root->parse->groupingSets;

	p->spec.set_mask = palloc(Max(list_length(sets), 1) * sizeof(uint32));
	foreach_ptr(List, set, sets)
	{
		uint32		mask = 0;

		foreach_int(ref, set)
		{
			const int	k = key_index_of(p, ref);

			if (k < 0)
				return false;
			mask |= ((uint32) 1) << k;
		}
		p->spec.set_mask[p->spec.nsets++] = mask;
	}
	return true;
}

/* a key of date_trunc('month', column) over a timestamp column (the calendar of a timestamp has no zone): k is the column, p->spec.bin the binning by the month */
static bool
compile_month(V6Plan *p, const FuncExpr *f, SiftV6Col *k)
{
	const Const *unit = linitial(f->args);

	if (!IsA(unit, Const) || unit->constisnull || unit->consttype != TEXTOID || strcmp(TextDatumGetCString(unit->constvalue), "month") != 0 ||
		!column_of(p, lsecond(f->args), false, k) || k->typid != TIMESTAMPOID)
		return false;
	p->spec.bin[p->spec.nbins++] = (SiftV6Bin) {.key = p->spec.nkeys,.stride_us = SIFT_DATEBIN_MONTH,.origin = 0};
	return true;
}

/* a key of date_bin(stride, column, origin) over a timestamp or timestamptz column, with a stride and an origin PostgreSQL accepts for every source: k is the column, p->spec.bin the binning */
static bool
compile_bin(V6Plan *p, Expr *e, SiftV6Col *k)
{
	const FuncExpr *f = (const FuncExpr *) e;
	SiftDateBinShape shape = {0};
	SiftDateBin bin;
	const Const *stride,
			   *origin;
	const SiftV6Resolver rs = {resolve_column, p};
	const char *fn;
	int64		trunc_us;

	if (sift_v6_unnest_month_key(p, e, k))
	{
		p->spec.bin[p->spec.nbins++] = (SiftV6Bin) {.key = p->spec.nkeys,.stride_us = SIFT_DATEBIN_MONTH,.origin = 0};
		return true;
	}
	if (sift_v6_date_trunc_bin(&rs, e, k, &trunc_us))
	{
		p->spec.bin[p->spec.nbins++] = (SiftV6Bin) {.key = p->spec.nkeys,.stride_us = trunc_us,.origin = 0};
		return true;
	}
	if (!IsA(e, FuncExpr) || get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE)
		return false;
	fn = sift_func_name(f->funcid);
	if (list_length(f->args) == 2 && strcmp(fn, "date_trunc") == 0)
		return compile_month(p, f, k);
	if (list_length(f->args) != 3 || strcmp(fn, "date_bin") != 0)
		return false;
	stride = linitial(f->args);
	origin = lthird(f->args);
	shape.stride_const = IsA(stride, Const);
	shape.origin_const = IsA(origin, Const);
	if (shape.stride_const && shape.origin_const)
	{
		const Interval *iv = DatumGetIntervalP(stride->constvalue);

		shape.stride_null = stride->constisnull;
		shape.origin_null = origin->constisnull;
		if (!shape.stride_null)
			shape.stride = (SiftInterval16) {.time = iv->time,.day = iv->day,.month = iv->month};
		shape.origin = origin->constisnull ? 0 : DatumGetInt64(origin->constvalue);
		shape.source_ok = column_of(p, lsecond(f->args), false, k) && (k->typid == TIMESTAMPOID || k->typid == TIMESTAMPTZOID) && origin->consttype == k->typid;
	}
	if (sift_shape_datebin(&shape, &bin) != SSV_OK)
		return false;
	p->spec.bin[p->spec.nbins++] = (SiftV6Bin) {.key = p->spec.nkeys,.stride_us = bin.stride_us,.origin = bin.origin};
	return true;
}

/* a key that is the first key plus a constant: a function of it, made by the projection of the plan; the first key must then stay in range (SiftV6Spec.affine) */
static bool
affine_key(V6Plan *p, Expr *e)
{
	int64		delta;
	Oid			type;
	const Var  *v = sift_v6_affine_of(e, &delta, &type);
	SiftV6Col	base;

	if (v == NULL || p->spec.nkeys == 0 || p->root->parse->groupingSets != NIL || !column_of(p, (Expr *) v, false, &base) || base.attno != p->spec.key[0].attno ||
		p->spec.key[0].derive != 0 || p->spec.key[0].typid != type)
		return false;
	if (!p->spec.affine)
	{
		p->spec.affine = true;
		p->spec.aff_type = type;
		p->spec.aff_lo = type == INT4OID ? PG_INT32_MIN : PG_INT64_MIN;
		p->spec.aff_hi = type == INT4OID ? PG_INT32_MAX : PG_INT64_MAX;
	}
	sift_v6_affine_range(type, delta, &p->spec.aff_lo, &p->spec.aff_hi);
	return true;
}

/* a key: a column, a binned or derived column, or what the executor computes from columns; the table groups the types it has no bytes of by their values (v6_datum.h) */
static bool
compile_key(V6Plan *p, Expr *source, SiftV6Col *k)
{
	Expr	   *e = sift_v6_plan_type_vars(p, source);
	const Oid	typid = getBaseType(exprType((Node *) e));

	if (read_of(p, e, false, k) || compile_bin(p, e, k) || derived_col(p, e, true, k) || sift_v6_pg_derived_col(p, e, true, k) ||
		(typid == NUMERICOID && column_of(p, e, true, k)))
	{
		k->collid = exprCollation((Node *) e);
		if (sift_v6_col_class(k) == SV6C_DATUM)
			return sift_genkey_supported(getBaseType(k->typid));
		return !(sift_v6_class(k->typid) == SV6C_TEXT && !sift_text_bytes_equal(k->collid));
	}
	return false;
}

static bool
compile_keys(V6Plan *p, List *group_exprs)
{
	p->spec.key = palloc(Max(list_length(group_exprs), 1) * sizeof(SiftV6Col));
	foreach_ptr(Expr, e, group_exprs)
	{
		SiftV6Col  *k = &p->spec.key[p->spec.nkeys];

		if (IsA(e, Const) && !((Const *) e)->constisnull && p->root->parse->groupingSets == NIL)
			continue;
		if (affine_key(p, e))
			continue;
		if (IsA(e, PlaceHolderVar))
			e = ((PlaceHolderVar *) e)->phexpr;
		if (p->spec.nkeys == SIFT_V6_MAX_KEYS || !compile_key(p, e, k))
			return false;
		p->spec.nkeys++;
		add_scan_var(p, e);
	}
	p->spec.constant_groups = p->spec.nkeys == 0 && p->root->parse->groupClause != NIL && p->root->parse->groupingSets == NIL;
	return p->spec.nkeys > 0 || p->spec.constant_groups || group_exprs == NIL;
}

bool
sift_v6_plan_query_ok(PlannerInfo *root, RelOptInfo *output_rel)
{
	const Query *parse = root->parse;

	return sift_enable_agg && !parse->hasDistinctOn && !parse->hasTargetSRFs &&
		(parse->hasAggs || parse->groupClause != NIL || parse->distinctClause != NIL) && output_rel->reltarget != NULL;
}

bool
sift_v6_plan_fact_ok(PlannerInfo *root, RelOptInfo *rel)
{
	const RangeTblEntry *rte;

	if (rel->reloptkind != RELOPT_BASEREL || rel->indexlist == NIL || rel->lateral_relids != NULL || root->parse->hasRowSecurity)
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL && rte->securityQuals == NIL;
}

Relids
sift_v6_plan_base_relids(PlannerInfo *root, RelOptInfo *rel)
{
	Relids		base = NULL;
	int			r = -1;

	while ((r = bms_next_member(rel->relids, r)) >= 0)
		if (r < root->simple_rel_array_size && root->simple_rel_array[r] != NULL && root->simple_rel_array[r]->reloptkind == RELOPT_BASEREL)
			base = bms_add_member(base, r);
	return base;
}

bool
sift_v6_plan_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	RangeTblEntry *rte;

	if (!sift_v6_plan_query_ok(root, output_rel) ||
		(input_rel->reloptkind != RELOPT_BASEREL && input_rel->reloptkind != RELOPT_OTHER_MEMBER_REL))
		return false;
	rte = planner_rt_fetch(input_rel->relid, root);
	return rte->rtekind == RTE_RELATION && rte->tablesample == NULL && input_rel->lateral_relids == NULL && output_rel->reltarget != NULL &&
		((rte->relkind == RELKIND_RELATION && !rte->inh && rte->securityQuals == NIL) ||
		 (rte->relkind == RELKIND_PARTITIONED_TABLE && rte->inh && rte->securityQuals == NIL && input_rel->reloptkind == RELOPT_BASEREL && input_rel->part_scheme != NULL));
}

bool
sift_v6_index_owned(const IndexOptInfo *ix)
{
	return !ix->hypothetical && sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid);
}

bool
sift_v6_index_usable(const IndexOptInfo *ix)
{
	return ix->indpred == NIL && sift_v6_index_owned(ix);
}

/* does e read what the scan does not give: a column, an aggregate or a GROUPING() that is not an entry of the scan tlist, taking a part of e that is an entry (a key that is an expression) whole */
static bool
reads_outside_scan_tlist(Node *e, void *context)
{
	const V6Plan *p = context;

	if (e == NULL)
		return false;
	if (scan_tlist_has(p, (const Expr *) e))
		return false;
	if (IsA(e, PlaceHolderVar))
		return reads_outside_scan_tlist((Node *) ((PlaceHolderVar *) e)->phexpr, context);
	if (IsA(e, Var) || IsA(e, Aggref) || IsA(e, GroupingFunc))
		return true;
	return expression_tree_walker(e, reads_outside_scan_tlist, context);
}

static bool
reads_only_scan_tlist(const V6Plan *p, Node *e)
{
	return !reads_outside_scan_tlist(e, (void *) p);
}

bool
sift_v6_compile_head(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	Node	   *exprs,
			   *having = extra != NULL ? extra->havingQual : NULL;

	p->spec = (SiftV6Spec) {.relid = p->relid,.is_table = p->is_table,.ext = p->xseed};
	p->freg = sift_freg_create(CurrentMemoryContext);
	p->cap_aggs = p->cap_funcs = 0;
	p->scan_tlist = NIL;
	p->scan_vars = NULL;
	p->text = NULL;
	exprs = sift_vn_rewrite_aggs((Node *) p->out->reltarget->exprs);
	having = sift_vn_rewrite_aggs(having);
	return compile_keys(p, group_exprs) && compile_sets(p) && compile_aggs(p, exprs) && (having == NULL || compile_aggs(p, having)) &&
		reads_only_scan_tlist(p, exprs) && (having == NULL || reads_only_scan_tlist(p, having));
}

bool
sift_v6_compile_on(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return sift_v6_index_complete(p) && sift_v6_compile_head(p, extra, group_exprs) && sift_v6_compile_quals(p) && !sift_v6_json_type_clash(&p->spec);
}

void
sift_v6_add_join_path(V6Plan *p, List *group_exprs, List *having, List *child_paths, Cost extra_startup, Cost extra_total, double input_rows)
{
	CustomPath *cp = makeNode(CustomPath);
	Cost		serial_total;
	bool		locked;
	const double groups = (p->spec.nkeys == 0 ? 1.0 : sift_v6_card_group_estimate(p->root, p->rel, p->index, group_exprs, input_rows)) * Max(p->spec.nsets, 1);

	if (!sift_v6_gen_fits(&p->spec, groups))
		return;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = p->out;
	cp->path.pathtarget = p->out->reltarget;
	cp->path.rows = clamp_row_est(groups);
	scan_cost(p, groups, &cp->path.startup_cost, &cp->path.total_cost);
	cp->path.startup_cost += extra_startup;
	cp->path.total_cost += extra_total;
	cp->methods = &v6_path_methods;
	cp->custom_paths = child_paths;
	p->spec.groups_est = (uint32) Min(groups, (double) INT_MAX);
	p->spec.lock_k = child_paths == NIL && p->spec.ext == NULL && p->spec.nparts == 0 ? sift_v6_limit_of(p->root->parse) : 0;
	cp->custom_private = list_make3(sift_v6_spec_pack(&p->spec), p->scan_tlist, having);
	locked = p->spec.lock_k > 0;
	p->spec.lock_k = 0;
	agg_force_price(&cp->path);
	serial_total = cp->path.total_cost;
	add_path(p->out, &cp->path);
	if (child_paths == NIL && p->spec.ext == NULL && p->spec.nparts == 0 && !p->spec.is_table && !locked && sift_v6_spec_parallel_ok(p->root, &p->spec) && sift_v6_gen_parallel_ok(&p->spec))
		sift_v6_par_add_path(p->root, p->rel, p->out, &p->spec, &v6_path_methods, p->scan_tlist, having, groups, serial_total);
}


void
sift_v6_add_group_path(V6Plan *p, RelOptInfo *output_rel, List *group_exprs, List *having)
{
	p->out = output_rel;
	sift_v6_add_join_path(p, group_exprs, having, NIL, 0, 0, p->rel->rows);
}

const CustomPathMethods *
sift_v6_group_path_methods(void)
{
	return &v6_path_methods;
}

/* The paths of the one table of input_rel: its table, or each index of it that holds what the statement reads; extra is NULL below the grouping stage (SELECT DISTINCT) */
void
sift_v6_scan_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	RangeTblEntry *rte = planner_rt_fetch(input_rel->relid, root);
	List	   *having = extra != NULL ? (List *) extra->havingQual : NIL;
	Relation	heap = table_open(rte->relid, AccessShareLock);

	if (sift_store_ops_for_scan(rte->relid, true) != NULL)
	{
		V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.relid = rte->relid,.batch_ok = true};

		if (sift_v6_plan_table(&p, rte->relid) && sift_v6_compile_on(&p, extra, group_exprs))
			sift_v6_add_group_path(&p, output_rel, group_exprs, having);
	}
	else
		foreach_ptr(IndexOptInfo, ix, input_rel->indexlist)
		{
			V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.relid = ix->indexoid,.index = ix,.batch_ok = true};
			Relation	irel;

			if (!sift_v6_index_owned(ix))
				continue;
			irel = index_open(ix->indexoid, AccessShareLock);
			sift_v6_plan_index(&p, irel);
			if (p.ops != NULL && sift_v6_compile_on(&p, extra, group_exprs))
				sift_v6_add_group_path(&p, output_rel, group_exprs, having);
			index_close(irel, NoLock);
		}
	table_close(heap, NoLock);
}

/* A partitionwise aggregate plans each partition under the root of the parent: the expressions of the statement name the parent, and those of a partition its own columns */
static Node *
in_terms_of(PlannerInfo *root, RelOptInfo *rel, Node *e)
{
	if (rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return e;
	return adjust_appendrel_attrs_multilevel(root, e, rel, find_base_rel(root, bms_singleton_member(rel->top_parent_relids)));
}

void
sift_agg_v6_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs = NIL;

	if (stage == UPPERREL_DISTINCT)
	{
		sift_v6_distinct_paths(root, input_rel, output_rel);
		return;
	}
	if (stage == UPPERREL_ORDERED)
	{
		for (int i = 0; i < sift_store_ops_count(); i++)
			if (sift_store_ops_at(i)->scope != NULL)
				sift_store_ops_at(i)->scope->topk_paths(root, input_rel, output_rel);
		if (sift_enable_agg)
		{
			sift_v6_topk_paths(root, input_rel, output_rel);
			sift_v6_add_rows_paths(root, input_rel, output_rel);
		}
		return;
	}
	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (input_rel->reloptkind == RELOPT_JOINREL)
	{
		sift_v6_star_upper_paths(root, input_rel, output_rel, extra);
		sift_v6_unnest_upper_paths(root, input_rel, output_rel, extra);
		sift_v6_join_upper_paths(root, input_rel, output_rel, extra);
		for (int i = 0; i < sift_store_ops_count(); i++)
			if (sift_store_ops_at(i)->scope != NULL)
				sift_store_ops_at(i)->scope->upper_paths(root, input_rel, output_rel, extra);
		sift_agg_v6_join_push_paths(root, input_rel, output_rel, extra);
		return;
	}
	if (!sift_v6_plan_shape(root, input_rel, output_rel))
		return;
	foreach_ptr(Node, e, sift_v6_group_exprs_of(root))
		group_exprs = lappend(group_exprs, in_terms_of(root, input_rel, e));
	if (input_rel->part_scheme != NULL && planner_rt_fetch(input_rel->relid, root)->inh)
	{
		sift_agg_v6_partition_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
		return;
	}
	sift_v6_unnest_exists_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
	sift_v6_scan_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
}

typedef struct DependentCols
{
	List	   *keys;
} DependentCols;

/* a column the output reads that is no key and no argument of an aggregate: PostgreSQL allows it only when the keys decide it (a primary key), so it is one more key that splits no group */
static bool
dependent_cols(Node *n, DependentCols *c)
{
	if (n == NULL || list_member(c->keys, n) || IsA(n, Aggref) || IsA(n, GroupingFunc) || IsA(n, WindowFunc))
		return false;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0)
	{
		c->keys = lappend(c->keys, n);
		return false;
	}
	return expression_tree_walker(n, dependent_cols, c);
}

List *
sift_v6_group_exprs_of(PlannerInfo *root)
{
	const PathTarget *target = root->upper_targets[UPPERREL_GROUP_AGG];
	DependentCols c = {NIL};

	foreach_node(SortGroupClause, sgc, root->processed_groupClause)
		c.keys = lappend(c.keys, get_sortgroupclause_expr(sgc, root->parse->targetList));
	if (target != NULL && root->parse->groupingSets == NIL)
		dependent_cols((Node *) target->exprs, &c);
	return c.keys;
}

bool
sift_v6_compile_fact(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, IndexOptInfo *ix, GroupPathExtraData *extra, SiftV6Spec *spec, List **scan_tlist)
{
	const Oid	relid = ix != NULL ? ix->indexoid : planner_rt_fetch(fact->relid, root)->relid;
	V6Plan		p = {.root = root,.rel = fact,.out = out,.relid = relid,.index = ix};
	Relation	rel = ix != NULL ? index_open(relid, AccessShareLock) : table_open(relid, AccessShareLock);
	bool		ok;

	if (ix != NULL)
		sift_v6_plan_index(&p, rel);
	else
		sift_v6_plan_table(&p, relid);
	ok = (ix != NULL || p.is_table) && sift_v6_compile_on(&p, extra, sift_v6_group_exprs_of(root));
	if (ix != NULL)
		index_close(rel, NoLock);
	else
		table_close(rel, NoLock);
	*spec = p.spec;
	*scan_tlist = p.scan_tlist;
	return ok;
}

static bool
collect_params(Node *n, List **out)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param))
	{
		*out = lappend(*out, n);
		return false;
	}
	return expression_tree_walker(n, collect_params, out);
}

Plan *
sift_agg_v6_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	SiftV6Spec *spec = sift_v6_spec_unpack(linitial(best->custom_private));

	cs->scan.plan.targetlist = (List *) sift_vn_rewrite_aggs((Node *) tlist);
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	collect_params((Node *) spec->indexqual, &cs->custom_exprs);
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->custom_private = linitial(best->custom_private);
	cs->custom_plans = custom_plans;
	if (spec->ext != NULL)
	{
		sift_v6_ext_resolve(spec->ext, custom_plans);
		cs->custom_private = sift_v6_spec_pack(spec);
	}
	cs->methods = &sift_agg_v6_plan_methods;
	cs->scan.plan.qual = (List *) sift_vn_rewrite_aggs((Node *) lthird(best->custom_private));
	root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->relid);
	if (OidIsValid(spec->text_indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->text_indexoid);
	for (int k = 0; k < spec->nparts; k++)
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->part_index[k]);
	return &cs->scan.plan;
}
