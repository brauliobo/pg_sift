/*
 * sift_pushdown.c - bitmap paths for the filter-mask pushdown of the ranked
 * SiftScan (sift_pushdown.h).
 *
 * The planner builds a bitmap scan for a relation's restrictions in
 * create_index_paths, which is not exported in pieces. It is run again over the
 * relation's other indexes with plain index scans disabled, so the bitmap path
 * survives the dominance check of add_path, and the relation's own paths are
 * put back afterwards.
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_statistic.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "sift.h"
#include "sift_pushdown.h"

/* Per bitmap tuple and per heap page of the mask: the tbm entry, then its mask row and AND in every block (measured: ~0.1 us) */
#define FILTER_ENTRY_COST	(2 * cpu_tuple_cost)

int			sift_filter_pushdown = SIFT_PUSHDOWN_ON;

static const struct config_enum_entry pushdown_options[] = {
	{"off", SIFT_PUSHDOWN_OFF, false},
	{"on", SIFT_PUSHDOWN_ON, false},
	{"force", SIFT_PUSHDOWN_FORCE, false},
	{"false", SIFT_PUSHDOWN_OFF, true},
	{"true", SIFT_PUSHDOWN_ON, true},
	{NULL, 0, false}
};

void
sift_pushdown_init(void)
{
	DefineCustomEnumVariable("sift.filter_pushdown",
							 "Ranked scans take the tuples a bitmap of the other quals keeps: off, on (by cost) or force.",
							 NULL, &sift_filter_pushdown, SIFT_PUSHDOWN_ON, pushdown_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/* The indexes that can answer a bitmap scan and are not sift's */
static List *
bitmap_indexes(RelOptInfo *rel)
{
	List	   *usable = NIL;

	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam != sift_catalog()->am && ii->amhasgetbitmap)
			usable = lappend(usable, ii);
	return usable;
}

BitmapHeapPath *
sift_filter_bitmap_path(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexes = NIL,
			   *paths = rel->pathlist,
			   *partial = rel->partial_pathlist,
			   *indexlist = rel->indexlist,
			   *joininfo = rel->joininfo;
	bool		eclass_joins = rel->has_eclass_joins,
				parallel = rel->consider_parallel;
	BitmapHeapPath *best = NULL;
	uint64		saved;

	if (sift_filter_pushdown == SIFT_PUSHDOWN_OFF || !enable_bitmapscan || rel->lateral_relids != NULL ||
		(indexes = bitmap_indexes(rel)) == NIL)
		return NULL;
	rel->pathlist = rel->partial_pathlist = rel->joininfo = NIL;
	rel->indexlist = indexes;
	rel->has_eclass_joins = rel->consider_parallel = false;
	saved = sift_plain_index_scans_off(rel);
	create_index_paths(root, rel);
	sift_plain_index_scans_restore(rel, saved);
	foreach_ptr(Path, p, rel->pathlist)
		if (IsA(p, BitmapHeapPath) && p->param_info == NULL &&
			(best == NULL || p->total_cost < best->path.total_cost))
			best = (BitmapHeapPath *) p;
	rel->pathlist = paths;
	rel->partial_pathlist = partial;
	rel->indexlist = indexlist;
	rel->joininfo = joininfo;
	rel->has_eclass_joins = eclass_joins;
	rel->consider_parallel = parallel;
	return best;
}

/* The physical order correlation of a btree's leading column with the heap, as the index cost estimate reads it */
static double
index_correlation(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii)
{
	HeapTuple	tup;
	AttStatsSlot slot;
	double		correlation = 0;

	if (ii->relam != BTREE_AM_OID || ii->indexkeys[0] == 0)
		return 0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(rel->relid, root)->relid),
						  Int16GetDatum(ii->indexkeys[0]), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0;
	if (get_attstatsslot(&slot, tup, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		correlation = slot.numbers[0];
		free_attstatsslot(&slot);
	}
	ReleaseSysCache(tup);
	return correlation;
}

static double
btree_correlation(PlannerInfo *root, RelOptInfo *rel, Path *bitmapqual)
{
	return IsA(bitmapqual, IndexPath) ? index_correlation(root, rel, ((IndexPath *) bitmapqual)->indexinfo) : 0;
}

/*
 * Where in the heap the tuples a bitmap keeps can lie, as fractions [lo, hi] of the heap in the order the
 * ranker visits it (ascending superblocks). The ranker verifies every match before lo, so a post-filter
 * scan pays for that prefix.
 */
typedef struct HeapSpan
{
	double		lo,
				hi;
} HeapSpan;

static const HeapSpan whole_heap = {0, 1};

static HeapSpan
span_intersect(HeapSpan a, HeapSpan b)
{
	HeapSpan	r = {Max(a.lo, b.lo), Min(a.hi, b.hi)};

	r.hi = Max(r.hi, r.lo);
	return r;
}

static HeapSpan
span_union(HeapSpan a, HeapSpan b)
{
	return (HeapSpan) {Min(a.lo, b.lo), Max(a.hi, b.hi)};
}

/* The fraction of the leading column's rows below (BTLessStrategyNumber) or above (BTGreaterStrategyNumber) a value, from its statistics */
static double
column_fraction(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, int strategy, Oid valtype, Datum val)
{
	Oid			typid,
				collation,
				opno;
	int32		typmod;
	int16		typlen;
	bool		byval;
	OpExpr	   *clause;

	get_atttypetypmodcoll(planner_rt_fetch(rel->relid, root)->relid, ii->indexkeys[0], &typid, &typmod, &collation);
	if (!OidIsValid(opno = get_opfamily_member(ii->opfamily[0], typid, valtype, strategy)))
		return 0;
	get_typlenbyval(valtype, &typlen, &byval);
	clause = (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) makeVar(rel->relid, ii->indexkeys[0], typid, typmod, collation, 0),
									  (Expr *) makeConst(valtype, -1, collation, typlen, val, false, byval), InvalidOid, collation);
	set_opfuncid(clause);
	return clause_selectivity(root, (Node *) clause, rel->relid, JOIN_INNER, NULL);
}

/* The smallest and largest element of a constant array, ordered by the index's operator family; false when it has none or the family cannot order them */
static bool
array_bounds(IndexOptInfo *ii, Const *array, Oid *elemtype, Datum *min, Datum *max)
{
	ArrayType  *arr = DatumGetArrayTypeP(array->constvalue);
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int16		typlen;
	bool		byval;
	char		align;
	Oid			cmp;
	FmgrInfo	flinfo;
	bool		any = false;

	*elemtype = ARR_ELEMTYPE(arr);
	if (!OidIsValid(cmp = get_opfamily_proc(ii->opfamily[0], *elemtype, *elemtype, BTORDER_PROC)))
		return false;
	get_typlenbyvalalign(*elemtype, &typlen, &byval, &align);
	deconstruct_array(arr, *elemtype, typlen, byval, align, &elems, &nulls, &n);
	fmgr_info(cmp, &flinfo);
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			continue;
		if (!any || DatumGetInt32(FunctionCall2Coll(&flinfo, ii->indexcollations[0], elems[i], *min)) < 0)
			*min = elems[i];
		if (!any || DatumGetInt32(FunctionCall2Coll(&flinfo, ii->indexcollations[0], elems[i], *max)) > 0)
			*max = elems[i];
		any = true;
	}
	return any;
}

/*
 * The strategy (< <= = >= >) of a qual on the leading column that compares it with a constant, or with
 * the elements of a constant array (= ANY only), and the smallest and largest value it compares with
 */
static bool
qual_bounds(IndexOptInfo *ii, Expr *qual, int *strategy, Oid *valtype, Datum *min, Datum *max)
{
	Node	   *arg;
	Oid			opno;

	if (IsA(qual, OpExpr) && list_length(((OpExpr *) qual)->args) == 2)
		opno = ((OpExpr *) qual)->opno;
	else if (IsA(qual, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) qual)->useOr)
		opno = ((ScalarArrayOpExpr *) qual)->opno;
	else
		return false;
	arg = lsecond(IsA(qual, OpExpr) ? ((OpExpr *) qual)->args : ((ScalarArrayOpExpr *) qual)->args);
	while (IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (!IsA(arg, Const) || ((Const *) arg)->constisnull || (*strategy = get_op_opfamily_strategy(opno, ii->opfamily[0])) == 0)
		return false;
	if (IsA(qual, ScalarArrayOpExpr))
		return *strategy == BTEqualStrategyNumber && array_bounds(ii, (Const *) arg, valtype, min, max);
	*valtype = ((Const *) arg)->consttype;
	*min = *max = ((Const *) arg)->constvalue;
	return true;
}

/* The heap positions of a qual on the leading column of an ascending-correlated btree: the rows from its smallest to its largest value */
static HeapSpan
qual_span(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, Expr *qual)
{
	HeapSpan	span = whole_heap;
	int			strategy;
	Oid			valtype;
	Datum		min,
				max;

	if (!qual_bounds(ii, qual, &strategy, &valtype, &min, &max))
		return span;
	if (strategy == BTEqualStrategyNumber || strategy == BTGreaterStrategyNumber || strategy == BTGreaterEqualStrategyNumber)
		span.lo = column_fraction(root, rel, ii, BTLessStrategyNumber, valtype, min);
	if (strategy == BTEqualStrategyNumber || strategy == BTLessStrategyNumber || strategy == BTLessEqualStrategyNumber)
		span.hi = 1 - column_fraction(root, rel, ii, BTGreaterStrategyNumber, valtype, max);
	return span;
}

/* The heap positions of a bitmap's tuples; correlation mixes the span of a sorted column with the whole heap */
static HeapSpan
path_span(PlannerInfo *root, RelOptInfo *rel, Path *path)
{
	HeapSpan	span = whole_heap;

	if (IsA(path, BitmapAndPath))
		foreach_ptr(Path, p, ((BitmapAndPath *) path)->bitmapquals)
			span = span_intersect(span, path_span(root, rel, p));
	else if (IsA(path, BitmapOrPath))
	{
		span = (HeapSpan) {1, 0};
		foreach_ptr(Path, p, ((BitmapOrPath *) path)->bitmapquals)
			span = span_union(span, path_span(root, rel, p));
	}
	else if (IsA(path, IndexPath) && ((IndexPath *) path)->indexinfo->relam == BTREE_AM_OID &&
			 ((IndexPath *) path)->indexinfo->indexkeys[0] != 0)
	{
		IndexOptInfo *ii = ((IndexPath *) path)->indexinfo;
		double		rho = index_correlation(root, rel, ii);

		foreach_node(IndexClause, ic, ((IndexPath *) path)->indexclauses)
			if (ic->indexcol == 0)
				foreach_node(RestrictInfo, rinfo, ic->indexquals)
					span = span_intersect(span, qual_span(root, rel, ii, rinfo->clause));
		/* the position of a value falls with a negative correlation, and is anywhere without one */
		if (rho < 0)
			span = (HeapSpan) {1 - span.hi, 1 - span.lo};
		span = (HeapSpan) {rho * rho * span.lo, 1 - rho * rho * (1 - span.hi)};
	}
	return span;
}

void
sift_filter_estimate(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp, SiftFilterEst *est)
{
	double		tuples,
				pages,
				minpages,
				correlation,
				maxentries,
				lossy;
	Cost		index_cost;
	Selectivity bitmap_sel;
	HeapSpan	span;

	pages = compute_bitmap_pages(root, rel, bp->bitmapqual, 1.0, &index_cost, &tuples);
	/* compute_bitmap_pages counts the tuples of the lossy pages it takes as scattered; the bitmap keeps its selectivity */
	cost_bitmap_tree_node(bp->bitmapqual, &index_cost, &bitmap_sel);
	tuples = clamp_row_est(bitmap_sel * rel->tuples);
	/* compute_bitmap_pages assumes the tuples are scattered; a correlated column packs them into fewer pages */
	minpages = Min(ceil(tuples / Max(rel->tuples, 1) * rel->pages), pages);
	correlation = btree_correlation(root, rel, bp->bitmapqual);
	pages = minpages + (1 - correlation * correlation) * (pages - minpages);
	maxentries = tbm_calculate_entries((Size) work_mem * 1024);
	lossy = pages > maxentries ? 1.0 - maxentries / pages : 0.0;
	est->sel = Min(tuples / Max(rel->tuples, 1), 1.0);
	est->sel += (Min(pages / Max(rel->pages, 1), 1.0) - est->sel) * lossy;
	est->cost = index_cost + (tuples + Min(pages, maxentries)) * FILTER_ENTRY_COST;
	span = path_span(root, rel, bp->bitmapqual);
	est->start = span.lo;
	est->span = Max(span.hi - span.lo, bitmap_sel);
}

void
sift_filter_plan_deps(PlannerInfo *root, Plan *bitmap)
{
	if (IsA(bitmap, BitmapIndexScan))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, ((BitmapIndexScan *) bitmap)->indexid);
	else if (IsA(bitmap, BitmapAnd))
		foreach_ptr(Plan, p, ((BitmapAnd *) bitmap)->bitmapplans)
			sift_filter_plan_deps(root, p);
	else if (IsA(bitmap, BitmapOr))
		foreach_ptr(Plan, p, ((BitmapOr *) bitmap)->bitmapplans)
			sift_filter_plan_deps(root, p);
}
