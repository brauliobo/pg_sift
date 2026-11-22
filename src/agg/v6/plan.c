/*
 * agg/v6/plan.c - the path of SiftAggV6: an aggregate of one table that has a sift_col index of format major 2 holding every column the query reads. Its WHERE clauses are the
 * quals of the index scan the scan of the index decides on the stored values; the groups are plain columns, the aggregates count, sum, avg, min, max and count(DISTINCT) of one column.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

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
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
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
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/v6/v6_argext.h"
#include "agg/v6/v6_argext_sql.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_cost.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_plan_shape.h"
#include "col/am.h"
#include "col/am/format_ops.h"
#include "col/am/preds.h"
#include "col/col.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_store_schema.h"
#include "table/dispatch.h"
#include "core/catname.h"

static SiftV6Rel
rel_of(const V6Plan *p)
{
	return (SiftV6Rel) {.relid = p->rel->relid,.index = p->index,.colrel = p->colrel,.f2 = p->f2};
}

static const CustomPathMethods v6_path_methods = {"SiftAggV6", sift_agg_v6_plan_path, NULL};

/* The attribute of the host a column of the table is: the column of the index, or the attribute of the table; 0: the host does not hold it */
static int
host_attno(const V6Plan *p, AttrNumber attno)
{
	if (p->store != NULL)
		return attno > 0 && attno <= p->store->natts ? attno : 0;
	for (int i = 0; i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == attno)
			return i + 1;
	return 0;
}

static const SiftColSchema *
host_schema(const V6Plan *p)
{
	return p->store != NULL ? p->store->schema : p->colrel->schema;
}

static int
host_first(const V6Plan *p, int att)
{
	return p->store != NULL ? p->store->att_first[att] : p->colrel->first[att];
}

static int
host_nleaves(const V6Plan *p, int att)
{
	return p->store != NULL ? sift_f2_store_attr_leaves(p->store, att) : p->colrel->nleaves[att];
}

static Oid
host_opfamily(const V6Plan *p, int att)
{
	return p->store != NULL ? p->store->opfamily[att] : p->index->opfamily[att - 1];
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

static bool
column_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || (v->vartype == NUMERICOID && !numeric_ok))
		return false;
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

/* A column the scan reads as a value: a column of the index, the text or the float8 of a key of a jsonb document, a payload column of a dimension or the element of the unnest */
static bool
read_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const SiftV6Rel rel = rel_of(p);

	return column_of(p, e, numeric_ok, out) ||
		(p->store == NULL && (sift_v6_json_read(&rel, e, out) ||
							  (exprType((Node *) e) != NUMERICOID && (sift_v6_star_column(p, e, out) || sift_v6_unnest_column(p, e, out)))));
}

static void
scan_cost(const V6Plan *p, double groups, Cost *startup, Cost *total)
{
	SiftV6CostIn in = {.tuples = p->rel->tuples,.allvisfrac = p->rel->allvisfrac,.groups = groups,.table = p->store != NULL,
	.sel = clauselist_selectivity(p->root, p->rel->baserestrictinfo, 0, JOIN_INNER, NULL),.ncols_read = sift_v6_spec_ncols_read(&p->spec),
	.ncols_host = p->store != NULL ? p->store->natts : p->index->ncolumns};

	if (p->store == NULL)
		sift_v6_agg_cost(p->rel, p->index, &p->spec, groups, startup, total);
	else
	{
		*startup = 0;
		*total = sift_v6_scan_cost(&in);
	}
	if (p->text != NULL)
	{
		double		touch = 1.0,
					text_cost = sift_v6_text_cost(p->root, p->rel, p->text, p->text_qual, p->text_query, &touch);

		*total = *total * touch + text_cost;
	}
}

static void
add_scan_var(V6Plan *p, Expr *e)
{
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (equal(te->expr, e))
			return;
	p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry(copyObject(e), list_length(p->scan_tlist) + 1, NULL, false));
}

bool
sift_v6_agg_kind(const Aggref *a, SiftV6Kind *kind)
{
	const char *name = get_func_name(a->aggfnoid);
	const bool	catalog = get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE;
	const int	argdir = sift_argext_aggref_dir(a);

	if (argdir >= 0)
	{
		*kind = argdir == SIFT_ARG_MAX ? SV6_ARG_MAX : SV6_ARG_MIN;
		return a->aggfilter == NULL;
	}
	if (!catalog || name == NULL || a->aggfilter != NULL || a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL || a->aggvariadic)
		return false;
	if (strcmp(name, "count") == 0)
		*kind = a->aggstar ? SV6_COUNT_STAR : a->aggdistinct != NIL ? SV6_COUNT_DISTINCT : SV6_COUNT;
	else if (a->aggdistinct != NIL)
		return false;
	else if (strcmp(name, "sum") == 0)
		*kind = SV6_SUM;
	else if (strcmp(name, "avg") == 0)
		*kind = SV6_AVG;
	else if (strcmp(name, "min") == 0)
		*kind = SV6_MIN;
	else if (strcmp(name, "max") == 0)
		*kind = SV6_MAX;
	else if (list_length(a->aggargtypes) != 1 || !sift_v6_ext_kind(name, linitial_oid(a->aggargtypes), kind))
		return false;
	return true;
}

static bool
kind_takes(SiftV6Kind kind, Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	switch (kind)
	{
		case SV6_COUNT:
			return cls != SV6C_NONE;
		case SV6_COUNT_DISTINCT:
			return cls != SV6C_NONE;
		case SV6_SUM:
			return typid != FLOAT4OID && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64);
		case SV6_AVG:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64;
		case SV6_MIN:
		case SV6_MAX:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64 || cls == SV6C_TEXT;
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
			return cls == SV6C_F64;
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
			return cls == SV6C_BOOL;
		case SV6_ARG_MAX:
		case SV6_ARG_MIN:
			return cls == SV6C_I32 || cls == SV6C_I64;
		default:
			return true;
	}
}

static Expr *index_qual(V6Plan *p, Expr *clause);

/* FILTER (WHERE ..): a conjunction of the quals of a scan, over the columns of the index; the same clause of two aggregates is one clause (at most 64 of them: a bit of the mask of a row each) */
static bool
compile_filter(V6Plan *p, Expr *filter, int *fclause)
{
	List	   *quals = NIL;
	SiftClause *clause = palloc(sizeof(SiftClause));
	SiftBytes	canon;
	int			id;

	p->in_filter = true;
	foreach_ptr(Expr, c, make_ands_implicit(filter))
	{
		Expr	   *q = index_qual(p, c);

		if (q == NULL)
			return false;
		quals = lappend(quals, q);
	}
	p->in_filter = false;
	sift_bytes_init(&canon, CurrentMemoryContext);
	if (!sift_v6_clause_make(CurrentMemoryContext, quals, &p->spec, clause, &canon) || (id = sift_freg_add(p->freg, canon.buf, canon.len)) < 0)
		return false;
	if (id == p->spec.nfilters)
	{
		p->spec.filter = lappend(p->spec.filter, quals);
		p->spec.nfilters++;
	}
	*fclause = id + 1;
	return true;
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

		if (!column_of(p, e, true, col) || !kind_takes(SV6_COUNT_DISTINCT, col->typid) ||
			(sift_v6_class(col->typid) == SV6C_TEXT && !sift_text_bytes_equal(exprCollation((Node *) e))))
			return false;
		n++;
	}
	g->nxcols = n - 1;
	return true;
}

static bool
compile_agg(V6Plan *p, Aggref *a)
{
	SiftV6Agg	g = {.restype = a->aggtype};
	Aggref		unfiltered = *a;
	SiftV6Class cls;
	Oid			coll;

	unfiltered.aggfilter = NULL;
	if (!sift_v6_agg_kind(&unfiltered, &g.kind) || (a->aggfilter != NULL && !compile_filter(p, a->aggfilter, &g.fclause)))
		return false;
	if (g.kind == SV6_COUNT_DISTINCT && list_length(a->args) == 1 && IsA(((TargetEntry *) linitial(a->args))->expr, RowExpr))
	{
		if (!compile_distinct_row(p, (const RowExpr *) ((TargetEntry *) linitial(a->args))->expr, &g))
			return false;
	}
	else if (g.kind != SV6_COUNT_STAR)
	{
		const bool	two = g.kind == SV6_ARG_MAX || g.kind == SV6_ARG_MIN;

		Expr	   *arg = list_length(a->args) > 0 ? ((TargetEntry *) linitial(a->args))->expr : NULL;
		const bool	score = !two && g.kind != SV6_COUNT_DISTINCT && sift_v6_score_col(p, arg, &g.arg);

		if (list_length(a->args) != (two ? 2 : 1) || !(score || (read_of(p, arg, !two, &g.arg) && kind_takes(g.kind, g.arg.typid))))
			return false;
		if (two && (!column_of(p, ((TargetEntry *) lsecond(a->args))->expr, false, &g.arg2) || sift_v6_class(g.arg2.typid) == SV6C_BOOL || sift_v6_class(g.arg2.typid) == SV6C_TEXT))
			return false;
		cls = sift_v6_class(g.arg.typid);
		coll = exprCollation((Node *) ((TargetEntry *) linitial(a->args))->expr);
		if (cls == SV6C_TEXT && !((g.kind == SV6_MIN || g.kind == SV6_MAX) ? sift_collate_is_c(coll) : sift_text_bytes_equal(coll)))
			return false;
	}
	p->spec.agg = p->spec.agg ? repalloc(p->spec.agg, (p->spec.naggs + 1) * sizeof(SiftV6Agg)) : palloc((p->spec.naggs + 1) * sizeof(SiftV6Agg));
	p->spec.agg[p->spec.naggs++] = g;
	add_scan_var(p, (Expr *) a);
	return true;
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
	p->spec.func = p->spec.func ? repalloc(p->spec.func, (p->spec.nfuncs + 1) * sizeof(SiftV6Func)) : palloc((p->spec.nfuncs + 1) * sizeof(SiftV6Func));
	p->spec.func[p->spec.nfuncs] = f;
	p->spec.agg = p->spec.agg ? repalloc(p->spec.agg, (p->spec.naggs + 1) * sizeof(SiftV6Agg)) : palloc((p->spec.naggs + 1) * sizeof(SiftV6Agg));
	p->spec.agg[p->spec.naggs++] = (SiftV6Agg) {.kind = SV6_GROUPING,.arg = {.attno = p->spec.nfuncs++},.restype = INT4OID};
	add_scan_var(p, (Expr *) gf);
	return true;
}

static bool
compile_aggs(V6Plan *p, Node *e)
{
	foreach_node(Node, n, pull_var_clause(e, PVC_INCLUDE_AGGREGATES))
	{
		bool		seen = false;

		if (!IsA(n, Aggref) && !IsA(n, GroupingFunc))
			continue;
		foreach_node(TargetEntry, te, p->scan_tlist)
			seen |= equal(te->expr, n);
		if (!seen && !(IsA(n, Aggref) ? compile_agg(p, (Aggref *) n) : compile_grouping(p, (GroupingFunc *) n)))
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
	const char *fn;

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

static bool
compile_keys(V6Plan *p, List *group_exprs)
{
	p->spec.key = palloc(Max(list_length(group_exprs), 1) * sizeof(SiftV6Col));
	foreach_ptr(Expr, e, group_exprs)
	{
		SiftV6Col  *k = &p->spec.key[p->spec.nkeys];

		if (p->spec.nkeys == SIFT_V6_MAX_KEYS || !(read_of(p, e, false, k) || compile_bin(p, e, k)) || sift_v6_class(k->typid) == SV6C_F64 ||
			(sift_v6_class(k->typid) == SV6C_TEXT && !sift_text_bytes_equal(exprCollation((Node *) e))))
			return false;
		p->spec.nkeys++;
		add_scan_var(p, e);
	}
	return true;
}

/* a constant, or a parameter the scan reads when it starts (and again when it is rescanned) */
static bool
operand_usable(const V6Plan *p, const Node *n)
{
	if (IsA(n, Param))
		return !p->in_filter && (((const Param *) n)->paramkind == PARAM_EXTERN || ((const Param *) n)->paramkind == PARAM_EXEC);
	return IsA(n, Const) && !((const Const *) n)->constisnull;
}

static bool
is_operand(const Node *n)
{
	return IsA(n, Const) || IsA(n, Param);
}

static Expr *
index_var(const Var *v, int col)
{
	return (Expr *) makeVar(INDEX_VAR, col + 1, v->vartype, v->vartypmod, v->varcollid, 0);
}

static Expr *
qual_compare(V6Plan *p, OpExpr *op)
{
	Node	   *l,
			   *r;
	OpExpr	   *q = copyObject(op);
	SiftV6Col	col;
	int			strat;

	if (list_length(op->args) != 2)
		return NULL;
	l = linitial(q->args);
	r = lsecond(q->args);
	if (IsA(r, Var) && is_operand(l))
	{
		Node	   *t = l;

		q->opno = get_commutator(q->opno);
		if (!OidIsValid(q->opno))
			return NULL;
		q->opfuncid = get_opcode(q->opno);
		l = r;
		r = t;
		q->args = list_make2(l, r);
	}
	if (!column_of(p, (Expr *) l, false, &col) || !operand_usable(p, r))
		return NULL;
	strat = get_op_opfamily_strategy(q->opno, host_opfamily(p, col.attno));
	if (strat == COL_STRAT_NE ? sift_v6_class(col.typid) == SV6C_F64 : (strat < BTLessStrategyNumber || strat > BTGreaterStrategyNumber))
		return NULL;
	if (sift_v6_class(col.typid) == SV6C_TEXT && ((strat != BTEqualStrategyNumber && strat != COL_STRAT_NE) || !sift_text_bytes_equal(q->inputcollid)))
		return NULL;
	linitial(q->args) = index_var((Var *) l, col.attno - 1);
	return (Expr *) q;
}

static Expr *
qual_in_list(V6Plan *p, ScalarArrayOpExpr *sa)
{
	ScalarArrayOpExpr *q = copyObject(sa);
	SiftV6Col	col;

	if (!sa->useOr || (p->index != NULL && !p->index->amsearcharray) || list_length(sa->args) != 2 || !IsA(lsecond(sa->args), Const) ||
		!column_of(p, linitial(sa->args), false, &col) || ((Const *) lsecond(sa->args))->constisnull || sift_v6_class(col.typid) == SV6C_F64 ||
		get_op_opfamily_strategy(sa->opno, host_opfamily(p, col.attno)) != BTEqualStrategyNumber ||
		(sift_v6_class(col.typid) == SV6C_TEXT && !sift_text_bytes_equal(sa->inputcollid)))
		return NULL;
	linitial(q->args) = index_var(linitial(sa->args), col.attno - 1);
	return (Expr *) q;
}

static Expr *
qual_null_test(V6Plan *p, NullTest *nt)
{
	NullTest   *q = copyObject(nt);
	SiftV6Col	col;

	if (nt->argisrow || !column_of(p, nt->arg, true, &col))
		return NULL;
	q->arg = index_var((Var *) nt->arg, col.attno - 1);
	return (Expr *) q;
}

static Expr *
index_qual(V6Plan *p, Expr *clause)
{
	switch (nodeTag(clause))
	{
		case T_OpExpr:
			return qual_compare(p, (OpExpr *) clause);
		case T_ScalarArrayOpExpr:
			return qual_in_list(p, (ScalarArrayOpExpr *) clause);
		case T_NullTest:
			return qual_null_test(p, (NullTest *) clause);
		default:
			return NULL;
	}
}

/* the clauses of the query imply the predicate of a partial index: every row the query reads is in the index */
static bool
index_complete(const V6Plan *p)
{
	List	   *clauses = NIL;

	if (p->index == NULL || p->index->indpred == NIL)
		return true;
	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		clauses = lappend(clauses, ri->clause);
	return predicate_implied_by(p->index->indpred, clauses, false);
}

static bool
compile_quals(V6Plan *p)
{
	const SiftV6Rel rel = rel_of(p);

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		Expr	   *q;
		List	   *jq;

		if (p->index != NULL && p->index->indpred != NIL && predicate_implied_by(list_make1(ri->clause), p->index->indpred, false))
			continue;
		if (!ri->pseudoconstant && sift_v6_text_qual(p, ri))
			continue;
		q = ri->pseudoconstant ? NULL : index_qual(p, ri->clause);
		jq = ri->pseudoconstant || q != NULL || p->store != NULL ? NIL : sift_v6_json_qual(&rel, ri->clause);
		if (q == NULL && jq == NIL)
			return false;
		if (q != NULL)
			p->spec.indexqual = lappend(p->spec.indexqual, q);
		else
			p->spec.jsonqual = lappend(p->spec.jsonqual, jq);
	}
	return sift_v6_compile_text(p);
}

bool
sift_v6_plan_query_ok(PlannerInfo *root, RelOptInfo *output_rel)
{
	const Query *parse = root->parse;

	return sift_enable_agg && !parse->hasDistinctOn && !parse->hasTargetSRFs && !parse->hasWindowFuncs && (parse->hasAggs || parse->groupClause != NIL) && output_rel->reltarget != NULL;
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
	return ix->indexprs == NIL && !ix->hypothetical && sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid);
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
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (equal(te->expr, e))
			return false;
	if (IsA(e, Var) || IsA(e, Aggref) || IsA(e, GroupingFunc) || IsA(e, PlaceHolderVar))
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
	p->spec = (SiftV6Spec) {.relid = p->relid,.is_table = p->store != NULL,.ext = p->xseed};
	p->freg = sift_freg_create(CurrentMemoryContext);
	p->scan_tlist = NIL;
	p->text = NULL;
	return compile_keys(p, group_exprs) && compile_sets(p) && compile_aggs(p, (Node *) p->out->reltarget->exprs) && (extra->havingQual == NULL || compile_aggs(p, extra->havingQual)) &&
		reads_only_scan_tlist(p, (Node *) p->out->reltarget->exprs) && (extra->havingQual == NULL || reads_only_scan_tlist(p, extra->havingQual));
}

bool
sift_v6_compile_on(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return index_complete(p) && sift_v6_compile_head(p, extra, group_exprs) && compile_quals(p);
}

void
sift_v6_add_join_path(V6Plan *p, List *group_exprs, List *having, List *child_paths, Cost extra_startup, Cost extra_total, double input_rows)
{
	CustomPath *cp = makeNode(CustomPath);
	const double groups = (p->spec.nkeys == 0 ? 1.0 : sift_v6_card_group_estimate(p->root, p->rel, p->index, group_exprs, input_rows)) * Max(p->spec.nsets, 1);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = p->out;
	cp->path.pathtarget = p->out->reltarget;
	cp->path.rows = clamp_row_est(groups);
	scan_cost(p, groups, &cp->path.startup_cost, &cp->path.total_cost);
	cp->path.startup_cost += extra_startup;
	cp->path.total_cost += extra_total;
	cp->methods = &v6_path_methods;
	cp->custom_paths = child_paths;
	cp->custom_private = list_make3(sift_v6_spec_pack(&p->spec), p->scan_tlist, having);
	agg_force_price(&cp->path);
	add_path(p->out, &cp->path);
	if (child_paths == NIL && p->spec.ext == NULL && p->spec.nparts == 0 && !p->spec.is_table)
		sift_v6_par_add_path(p->root, p->rel, p->out, &p->spec, &v6_path_methods, p->scan_tlist, having, groups);
}

void
sift_v6_add_group_path(V6Plan *p, RelOptInfo *output_rel, List *group_exprs, List *having)
{
	p->out = output_rel;
	sift_v6_add_join_path(p, group_exprs, having, NIL, 0, 0, p->rel->rows);
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
	RangeTblEntry *rte;
	Relation	heap;

	if (stage == UPPERREL_ORDERED)
		sift_agg_v6_scope_topk_paths(root, input_rel, output_rel);
	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (input_rel->reloptkind == RELOPT_JOINREL)
	{
		sift_v6_star_upper_paths(root, input_rel, output_rel, extra);
		sift_v6_unnest_upper_paths(root, input_rel, output_rel, extra);
		sift_agg_v6_scope_upper_paths(root, input_rel, output_rel, extra);
		return;
	}
	if (!sift_v6_plan_shape(root, input_rel, output_rel))
		return;
	foreach_node(SortGroupClause, sgc, root->processed_groupClause)
		group_exprs = lappend(group_exprs, in_terms_of(root, input_rel, (Node *) get_sortgroupclause_expr(sgc, root->parse->targetList)));
	if (input_rel->part_scheme != NULL && planner_rt_fetch(input_rel->relid, root)->inh)
	{
		sift_agg_v6_partition_paths(root, input_rel, output_rel, (GroupPathExtraData *) extra, group_exprs);
		return;
	}
	rte = planner_rt_fetch(input_rel->relid, root);
	heap = table_open(rte->relid, NoLock);
	if (sift_tbl_is_table(heap) && sift_tbl_ops(heap) == &sift_tbl_v2_ops)
	{
		V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.relid = rte->relid,.store = sift_f2_store_open(heap)};

		if (sift_v6_compile_on(&p, (GroupPathExtraData *) extra, group_exprs))
			sift_v6_add_group_path(&p, output_rel, group_exprs, (List *) ((GroupPathExtraData *) extra)->havingQual);
	}
	else
		foreach_ptr(IndexOptInfo, ix, input_rel->indexlist)
		{
			V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.relid = ix->indexoid,.index = ix};
			Relation	irel;

			if (!sift_v6_index_owned(ix))
				continue;
			irel = index_open(ix->indexoid, NoLock);
			p.colrel = sift_col_rel(irel);
			p.f2 = p.colrel->stale ? NULL : sift_f2_rel(irel);
			if (p.f2 != NULL && sift_v6_compile_on(&p, (GroupPathExtraData *) extra, group_exprs))
				sift_v6_add_group_path(&p, output_rel, group_exprs, (List *) ((GroupPathExtraData *) extra)->havingQual);
			index_close(irel, NoLock);
		}
	table_close(heap, NoLock);
}

List *
sift_v6_group_exprs_of(PlannerInfo *root)
{
	List	   *group_exprs = NIL;

	foreach_node(SortGroupClause, sgc, root->processed_groupClause)
		group_exprs = lappend(group_exprs, get_sortgroupclause_expr(sgc, root->parse->targetList));
	return group_exprs;
}

bool
sift_v6_compile_fact(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, IndexOptInfo *ix, GroupPathExtraData *extra, SiftV6Spec *spec, List **scan_tlist)
{
	V6Plan		p = {.root = root,.rel = fact,.out = out,.relid = ix->indexoid,.index = ix};
	List	   *group_exprs = sift_v6_group_exprs_of(root);
	Relation	irel = index_open(ix->indexoid, NoLock);
	bool		ok;

	p.colrel = sift_col_rel(irel);
	p.f2 = sift_f2_rel(irel);
	ok = sift_v6_compile_on(&p, extra, group_exprs);
	index_close(irel, NoLock);
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

	cs->scan.plan.targetlist = tlist;
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
	cs->scan.plan.qual = lthird(best->custom_private);
	root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->relid);
	if (OidIsValid(spec->text_indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->text_indexoid);
	for (int k = 0; k < spec->nparts; k++)
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->part_index[k]);
	return &cs->scan.plan;
}
