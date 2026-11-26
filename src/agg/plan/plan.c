/*
 * agg/plan/plan.c - the path of SiftAggV6: an aggregate of one table that has a sift_col index of format major 2 holding every column the query reads. Its WHERE clauses are the
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
#include "catalog/pg_type_d.h"
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

#include "agg/safety.h"
#include "agg/agg.h"
#include "agg/ht/ht.h"
#include "agg/plan/int.h"
#include "agg/par/pg.h"
#include "agg/ext.h"
#include "agg/engine.h"
#include "agg/join/push.h"
#include "agg/cost/card.h"
#include "agg/vexpr.h"
#include "agg/expr/num.h"
#include "agg/argext/argext.h"
#include "agg/argext/sql.h"
#include "agg/cost/card.h"
#include "agg/cost/cost.h"
#include "agg/star/run.h"
#include "agg/filter/pg.h"
#include "agg/gen/gen.h"
#include "agg/pct.h"
#include "agg/settings.h"
#include "agg/plan/rewrite.h"
#include "agg/refuse.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/join/plan.h"
#include "agg/lateral_plan.h"
#include "agg/plan/shape.h"
#include "agg/datum.h"
#include "agg/open_scan.h"
#include "agg/expr/pg.h"
#include "agg/qual/qual.h"
#include "agg/source_rel.h"
#include "col/am.h"
#include "col/am/preds.h"
#include "col/col.h"
#include "table/dispatch.h"
#include "core/catname.h"

SiftV6Rel
sift_v6_plan_rel(const V6Plan *p)
{
	return (SiftV6Rel) {.relid = p->rel->relid,.reloid = p->relid,.index = p->index,.colrel = p->colrel,.table = p->is_table ? &p->sch : NULL,.ops = p->ops};
}

static const CustomPathMethods v6_path_methods = {"SiftAggV6", sift_agg_v6_plan_path, NULL};

static void
scan_cost(const V6Plan *p, double groups, Cost *startup, Cost *total)
{
	if (!p->is_table)
		sift_v6_agg_cost(p->root, p->rel, p->index, &p->spec, groups, startup, total);
	else
	{
		const AttrNumber lead = p->ops->order != NULL ? p->ops->order->lead_att(p->relid, true) : 0;
		const SiftV6CostIn in = {.tuples = p->rel->tuples,.pages = p->rel->pages,.rows = p->rel->rows,.f_prune = sift_v6_clause_prune(p->root, p->rel->baserestrictinfo, lead),
		.groups = groups,.ncols_read = sift_v6_spec_ncols_read(&p->spec)};

		sift_v6_scan_cost(&in, startup, total);
	}
	if (p->text != NULL)
	{
		double		touch = 1.0,
					text_cost = sift_v6_text_cost(p->root, p->rel, p->text, p->text_qual, p->text_queries, &touch);

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

		if (!sift_v6_plan_column(p, e, true, col) || !sift_v6_kind_takes(SV6_COUNT_DISTINCT, col->typid) ||
			(sift_v6_class(col->typid) == SV6C_TEXT && !sift_text_bytes_equal(exprCollation((Node *) e))))
			return false;
		n++;
	}
	g->nxcols = n - 1;
	return true;
}

/* the kind of an aggregate of the statement: percentile_cont of a float8 keeps the values of a group in one arena (v6_pct.h) */
static bool
agg_kind_of(const V6Plan *p, const Aggref *unfiltered, SiftV6Agg *g)
{
	if (sift_v6_pct_cont_of(unfiltered, &g->frac))
	{
		g->kind = SV6_PCT_CONT;
		return true;
	}
	if (!sift_v6_agg_kind(unfiltered, &g->kind))
		return false;
	if ((g->kind == SV6_COUNT || g->kind == SV6_COUNT_DISTINCT) && sift_v6_count_counts_rows(p->root, unfiltered))
		g->kind = SV6_COUNT_STAR;
	return true;
}

/* An aggregate the native kernels of the scan fold, not entered in the scan tlist (the caller decides what the node gives it as) */
bool
sift_v6_compile_agg_native(V6Plan *p, Aggref *agg)
{
	const Aggref *a = sift_v6_unnest_filter_as_case(p, agg);
	SiftV6Agg	g = {.restype = a->aggtype};
	Aggref		unfiltered = *a;
	SiftV6Class cls;
	Oid			coll;

	unfiltered.aggfilter = NULL;
	if (!agg_kind_of(p, &unfiltered, &g))
		return false;
	if (a->aggfilter != NULL && !sift_v6_compile_filter(p, a->aggfilter, &g.fclause))
	{
		Aggref	   *folded = sift_v6_filter_fold((Aggref *) a);

		return folded != NULL && sift_v6_compile_agg_native(p, folded);
	}
	if (g.kind == SV6_COUNT_DISTINCT && list_length(a->args) == 1 && IsA(((TargetEntry *) linitial(a->args))->expr, RowExpr))
	{
		if (!compile_distinct_row(p, (const RowExpr *) ((TargetEntry *) linitial(a->args))->expr, &g))
			return false;
	}
	else if (g.kind != SV6_COUNT_STAR)
	{
		const bool	two = g.kind == SV6_ARG_MAX || g.kind == SV6_ARG_MIN;

		Expr	   *arg = list_length(a->args) > 0 ? sift_v6_plan_json_vars(p, ((TargetEntry *) linitial(a->args))->expr, true) : NULL;
		const bool	score = !two && !sift_v6_kind_distinct(g.kind) && sift_v6_score_col(p, arg, &g.arg);
		const bool	derived_ok = !two && !sift_v6_kind_valued(g.kind);

		if (list_length(a->args) != (two ? 2 : 1) ||
			!(score || ((sift_v6_read_of(p, arg, !two, &g.arg) || (derived_ok && (sift_v6_derived_col(p, arg, false, &g.arg) || sift_v6_pg_derived_col(p, arg, false, &g.arg)))) && sift_v6_kind_takes(g.kind, g.arg.typid))) ||
			sift_v6_col_class(&g.arg) == SV6C_DATUM)
			return false;
		if (g.arg.derive > 0 && p->spec.derive[g.arg.derive - 1].kind == SVD_PG_EXPR && g.kind >= SV6_STDDEV_POP && g.kind <= SV6_VAR_SAMP)
			return false;
		if (g.arg.derive > 0 && p->spec.derive[g.arg.derive - 1].kind == SVD_NUM_EXPR && g.kind != SV6_SUM && g.kind != SV6_AVG && g.kind != SV6_COUNT)
			return false;
		if (two && (!sift_v6_plan_column(p, ((TargetEntry *) lsecond(a->args))->expr, false, &g.arg2) || sift_v6_class(g.arg2.typid) == SV6C_BOOL || sift_v6_class(g.arg2.typid) == SV6C_TEXT))
			return false;
		cls = sift_v6_class(g.arg.typid);
		coll = exprCollation((Node *) ((TargetEntry *) linitial(a->args))->expr);
		if (cls == SV6C_TEXT && !((g.kind == SV6_MIN || g.kind == SV6_MAX) ? sift_collate_is_c(coll) : sift_text_bytes_equal(coll)))
			return false;
	}
	spec_room(p, false);
	p->spec.agg[p->spec.naggs++] = g;
	return true;
}

static Node *
placeholder_contents(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
		return placeholder_contents((Node *) ((PlaceHolderVar *) n)->phexpr, context);
	return expression_tree_mutator(n, placeholder_contents, context);
}

/* any other aggregate, and the ones whose arguments or collation the kernels do not take: PostgreSQL's own functions over the values the scan decodes (v6_gen.h); the arguments are
 * evaluated over the columns of the scan, where a placeholder is the expression it holds */
static bool
compile_agg_gen(V6Plan *p, Aggref *a)
{
	SiftV6Agg	g;
	Aggref		unfiltered = *a;
	Aggref	   *ref;

	unfiltered.args = (List *) placeholder_contents((Node *) a->args, NULL);
	unfiltered.aggdirectargs = (List *) placeholder_contents((Node *) a->aggdirectargs, NULL);
	unfiltered.aggfilter = NULL;
	if ((ref = sift_v6_gen_compile(p, &unfiltered, &g)) == NULL || (a->aggfilter != NULL && !sift_v6_compile_filter(p, a->aggfilter, &g.fclause)))
		return false;
	p->spec.genref = lappend(p->spec.genref, ref);
	spec_room(p, false);
	p->spec.agg[p->spec.naggs++] = g;
	return true;
}

static bool
compile_agg(V6Plan *p, Aggref *a)
{
	const int	nderive = p->spec.nderive;
	Aggref	   *on_case = sift_vn_filter_to_case(a);

	if (sift_v6_compile_agg_native(p, a))
		return true;
	p->spec.nderive = nderive;
	if (on_case != NULL && sift_v6_compile_agg_native(p, on_case))
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
	return true;
}

static bool
compile_aggs(V6Plan *p, Node *e)
{
	foreach_ptr(Node, n, pull_var_clause(e, PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS))
	{
		if (!IsA(n, Aggref) && !IsA(n, GroupingFunc))
			continue;
		if (scan_tlist_has(p, (Expr *) n))
			continue;
		if (!(IsA(n, Aggref) ? compile_agg(p, (Aggref *) n) : compile_grouping(p, (GroupingFunc *) n)))
			return false;
		add_scan_var(p, (Expr *) n);
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
		!sift_v6_plan_column(p, lsecond(f->args), false, k) || k->typid != TIMESTAMPOID)
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
	const SiftV6Resolver rs = {sift_v6_resolve_column, p};
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
		shape.source_ok = sift_v6_plan_column(p, lsecond(f->args), false, k) && (k->typid == TIMESTAMPOID || k->typid == TIMESTAMPTZOID) && origin->consttype == k->typid;
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

	if (v == NULL || p->spec.nkeys == 0 || p->root->parse->groupingSets != NIL || !sift_v6_plan_column(p, (Expr *) v, false, &base) || base.attno != p->spec.key[0].attno ||
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
key_of(V6Plan *p, Expr *source, SiftV6Col *k)
{
	Expr	   *e = sift_v6_plan_json_vars(p, source, true);
	const Oid	typid = getBaseType(exprType((Node *) e));

	if (sift_v6_read_of(p, e, false, k) || compile_bin(p, e, k) || sift_v6_derived_col(p, e, true, k) || sift_v6_pg_derived_col(p, e, true, k) ||
		(typid == NUMERICOID && sift_v6_plan_column(p, e, true, k)))
	{
		k->collid = exprCollation((Node *) e);
		return true;
	}
	return false;
}

/* a key of the groups: a datum groups by its image, a text by its bytes, which the collation must not tell apart */
static bool
compile_key(V6Plan *p, Expr *source, SiftV6Col *k)
{
	if (!key_of(p, source, k))
		return false;
	if (sift_v6_col_class(k) == SV6C_DATUM)
		return sift_genkey_supported(getBaseType(k->typid));
	return !(sift_v6_class(k->typid) == SV6C_TEXT && !sift_text_bytes_equal(k->collid));
}

bool
sift_v6_compile_sort_key(V6Plan *p, Expr *e, SiftV6Col *k)
{
	return key_of(p, e, k);
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

	if (rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL || agg_query_has_rls(root) || !agg_rel_security_ok(root, rel))
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return sift_v6_rte_stored(rte) && !rte->inh && rte->tablesample == NULL && sift_v6_rel_has_source(root, rel);
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
	return rte->rtekind == RTE_RELATION && rte->tablesample == NULL && input_rel->lateral_relids == NULL && output_rel->reltarget != NULL && agg_rel_security_ok(root, input_rel) &&
		((sift_v6_rte_stored(rte) && !rte->inh) ||
		 (rte->relkind == RELKIND_PARTITIONED_TABLE && rte->inh && input_rel->reloptkind == RELOPT_BASEREL && input_rel->part_scheme != NULL));
}

bool
sift_v6_index_owned(const IndexOptInfo *ix)
{
	return !ix->hypothetical && sift_store_ops_may_own(ix->relam, false) && sift_v6_rel_owned(ix->indexoid, false);
}

bool
sift_v6_index_usable(const IndexOptInfo *ix)
{
	return agg_index_complete(ix) && sift_v6_index_owned(ix);
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

/* an output as the scan computes it (CASE as FILTER, the sums of an offset collapsed); a sorted one stays the expression of its pathkey, which the Sort above finds by equal() */
static Node *
rewrite_output(Node *e, Index sortgroupref)
{
	return sortgroupref != 0 ? e : sift_vn_rewrite_aggs(e);
}

static List *
rewrite_tlist(List *tlist)
{
	List	   *out = NIL;

	foreach_node(TargetEntry, te, tlist)
	{
		TargetEntry *copy = flatCopyTargetEntry(te);

		copy->expr = (Expr *) rewrite_output((Node *) te->expr, te->ressortgroupref);
		out = lappend(out, copy);
	}
	return out;
}

static List *
rewrite_target(const PathTarget *target)
{
	List	   *out = NIL;
	int			i = 0;

	foreach_ptr(Node, e, target->exprs)
		out = lappend(out, rewrite_output(e, get_pathtarget_sortgroupref(target, i++)));
	return out;
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
	p->gen_vars = NIL;
	p->text = NULL;
	exprs = (Node *) rewrite_target(p->out->reltarget);
	having = sift_vn_rewrite_aggs(having);
	return compile_keys(p, group_exprs) && compile_sets(p) && compile_aggs(p, exprs) && (having == NULL || compile_aggs(p, having)) &&
		reads_only_scan_tlist(p, exprs) && (having == NULL || reads_only_scan_tlist(p, having));
}

bool
sift_v6_compile_on(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return sift_v6_index_complete(p) && sift_v6_compile_head(p, extra, group_exprs) && sift_v6_compile_quals(p) && !sift_v6_json_type_clash(&p->spec) && sift_v6_json_cast_order_ok(p);
}

/* the format of the relation serves what the spec asks of it: with a zone-pruned scan, the capabilities the spec reads; without one, the batches of open() */
static bool
format_serves(const V6Plan *p)
{
	const SiftStoreOps *ops = p->ops;
	const SiftV6Spec *s = &p->spec;

	if (ops == NULL)
		return false;
	if (ops->zone == NULL)
		return sift_v6_open_scan_ok(s);
	return (ops->zone->batch_cols == 0 || sift_v6_spec_batch_fits(s, ops->zone->batch_cols)) && (!p->is_table || !OidIsValid(s->text_indexoid) || ops->zone->table_text) &&
		(!p->is_table || s->ext == NULL || s->ext->ndims == 0 || ops->zone->table_star) &&
		(!s->rows || (ops->order != NULL && ops->order->scan_rows != NULL)) && (s->ext == NULL || !s->ext->unnest || ops->jsonb != NULL) && (s->par_strategy == 0 || ops->par != NULL);
}

/* the cost of the cheapest path the relation has so far (-1: none) */
static double
cheapest_total(const RelOptInfo *rel)
{
	double		c = -1;

	foreach_ptr(Path, path, rel->pathlist)
		if (c < 0 || path->total_cost < c)
			c = path->total_cost;
	return c;
}

/* What the node pays again when a dimension is over its share of work_mem: the reads of the dimensions for each part or chunk, and a scan of the fact table for each pass (agg/star/run.h) */
static Cost
passes_cost(const V6Plan *p, List *child_paths, Cost scan)
{
	double		passes = 1;
	Cost		cost = 0;

	for (int d = list_length(child_paths) - 1; d >= 0; d--)
	{
		const Path *child = list_nth(child_paths, d);
		double		parts;
		const double reads = sift_v6_star_reads(p->spec.ext, d, child->rows, child->pathtarget->width, &parts);

		cost += (reads * passes - 1) * sift_v6_dim_cost(child);
		passes *= parts;
	}
	return cost + (passes - 1) * scan;
}

void
sift_v6_add_join_path(V6Plan *p, List *group_exprs, List *having, List *child_paths, Cost extra_startup, Cost extra_total, double input_rows)
{
	CustomPath *cp = makeNode(CustomPath);
	Cost		serial_total,
				passes;
	bool		locked;
	const double groups = (p->spec.nkeys == 0 ? 1.0 : sift_v6_card_group_estimate(p->root, p->rel, p->index, group_exprs, input_rows)) * Max(p->spec.nsets, 1);

	if (!format_serves(p) || !sift_v6_gen_fits(&p->spec, groups))
		return;
	if (p->text != NULL && sift_v6_search_key_first(p->root, p->rel, p->rel->tuples * clause_selectivity(p->root, (Node *) p->text_qual->clause, p->rel->relid, JOIN_INNER, NULL)))
	{
		sift_agg_refuse("search", AGG_REFUSE_COST, "a scan of a btree on the other conditions reads fewer rows than the scan walks postings", 0, -1);
		return;
	}
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = p->out;
	cp->path.pathtarget = p->out->reltarget;
	cp->path.rows = clamp_row_est(having == NIL ? groups : groups * clauselist_selectivity(p->root, having, 0, JOIN_INNER, NULL));
	scan_cost(p, groups, &cp->path.startup_cost, &cp->path.total_cost);
	passes = child_paths == NIL ? 0 : passes_cost(p, child_paths, cp->path.total_cost);
	cp->path.startup_cost += extra_startup + passes;
	cp->path.total_cost += extra_total + passes;
	cp->methods = &v6_path_methods;
	cp->custom_paths = child_paths;
	p->spec.groups_est = (uint32) Min(groups, (double) INT_MAX);
	p->spec.lock_k = child_paths == NIL && p->spec.ext == NULL && p->spec.nparts == 0 ? sift_v6_limit_of(p->root->parse) : 0;
	cp->custom_private = list_make3(sift_v6_spec_pack(&p->spec), p->scan_tlist, having);
	locked = p->spec.lock_k > 0;
	p->spec.lock_k = 0;
	sift_agg_arm_priced("single", cp->path.total_cost, cheapest_total(p->out));
	sift_v6_force_price(&cp->path);
	serial_total = cp->path.total_cost;
	add_path(p->out, &cp->path);
	if (child_paths == NIL && p->spec.ext == NULL && !locked && sift_v6_spec_parallel_ok(p->root, &p->spec) && sift_v6_gen_parallel_ok(&p->spec))
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

/* The paths of the one relation of input_rel: its table, or each index of it that holds what the statement reads; extra is NULL below the grouping stage (SELECT DISTINCT) */
void
sift_v6_scan_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	int			nsrc;
	SiftV6RelSource *src = sift_v6_rel_sources_alloc(root, input_rel, &nsrc);

	for (int i = 0; i < nsrc; i++)
		sift_v6_source_group_path(root, input_rel, output_rel, &src[i], extra, group_exprs);
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
		group_exprs = sift_v6_group_exprs_of(root);
		sift_v6_star_upper_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
		sift_v6_unnest_upper_paths(root, input_rel, output_rel, extra);
		sift_v6_join_upper_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
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
	List	   *grouped;
} DependentCols;

/* a column the output reads that is no key and no argument of an aggregate: PostgreSQL allows it only when the keys decide it (a primary key), so it is one more key that splits no group.
 * So is a key of the statement PostgreSQL dropped (a constant of its equivalence class) that reads a column: it is the key, not the columns it reads, which may split its groups */
static bool
dependent_cols(Node *n, DependentCols *c)
{
	if (n == NULL || list_member(c->keys, n) || IsA(n, Aggref) || IsA(n, GroupingFunc) || IsA(n, WindowFunc))
		return false;
	if ((IsA(n, Var) && ((Var *) n)->varlevelsup == 0) || (list_member(c->grouped, n) && contain_var_clause(n)))
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
	DependentCols c = {NIL, get_sortgrouplist_exprs(root->parse->groupClause, root->parse->targetList)};

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

Plan *
sift_agg_v6_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	SiftV6Spec *spec = sift_v6_spec_unpack(linitial(best->custom_private));

	sift_agg_arm_used("single");
	cs->scan.plan.targetlist = rewrite_tlist(tlist);
	if (spec->rows)
		cs->scan.plan.targetlist = (List *) sift_v6_rows_window_aggs((Node *) cs->scan.plan.targetlist);
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	sift_v6_collect_params((Node *) spec->indexqual, &cs->custom_exprs);
	sift_v6_collect_params((Node *) spec->vqual, &cs->custom_exprs);
	foreach_ptr(List, item, spec->pgexpr)
		sift_v6_collect_params(linitial(item), &cs->custom_exprs);
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->custom_private = linitial(best->custom_private);
	cs->custom_plans = custom_plans;
	if (spec->ext != NULL)
	{
		sift_v6_lateral_adopt(root, best->custom_paths);
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
