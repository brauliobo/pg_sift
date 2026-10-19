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
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
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
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
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

/* Most entries read from a btree while planning to find where they lie in the heap */
#define PROBE_MAX_ENTRIES	4096

/* The scan key of `col = constant` for the one column of a btree */
static bool
equality_key(IndexOptInfo *ii, Expr *qual, ScanKeyData *key)
{
	OpExpr	   *op;
	Node	   *arg;
	int			strategy;
	Oid			lefttype,
				righttype;

	if (!IsA(qual, OpExpr) || list_length(((OpExpr *) qual)->args) != 2)
		return false;
	op = (OpExpr *) qual;
	arg = lsecond(op->args);
	while (IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (!IsA(arg, Const) || ((Const *) arg)->constisnull ||
		get_op_opfamily_strategy(op->opno, ii->opfamily[0]) != BTEqualStrategyNumber)
		return false;
	get_op_opfamily_properties(op->opno, ii->opfamily[0], false, &strategy, &lefttype, &righttype);
	ScanKeyEntryInitialize(key, 0, 1, BTEqualStrategyNumber, righttype, op->inputcollid, get_opcode(op->opno),
						   ((Const *) arg)->constvalue);
	return true;
}

/* The heap block of the nth entry of the scan in its direction, InvalidBlockNumber when it has fewer */
static BlockNumber
nth_entry_block(IndexScanDesc scan, ScanDirection dir, int64 n)
{
	ItemPointer tid = NULL;

	for (int64 i = 0; i < n; i++)
	{
		if ((tid = index_getnext_tid(scan, dir)) == NULL)
			return InvalidBlockNumber;
		if (i % 1024 == 1023)
			CHECK_FOR_INTERRUPTS();
	}
	return ItemPointerGetBlockNumber(tid);
}

/*
 * Where in the heap the entries of `col = constant` lie, read from the btree: the entries of a key are
 * in heap order, so the nth of them marks the heap position before which a scan that checks the filter
 * after the ranker verifies every match (the column's statistics cannot say: a value stored in one
 * stretch of the heap does not make the column correlated), and the last one how far they reach.
 * The same probe of the btree the planner makes for the end of a range (get_actual_variable_endpoint).
 */
static bool
key_span(RelOptInfo *rel, RangeTblEntry *rte, IndexPath *ip, double entries, HeapSpan *span)
{
	IndexOptInfo *ii = ip->indexinfo;
	ScanKeyData key;
	IndexClause *ic;
	Relation	heap,
				index;
	IndexScanDesc scan;
	SnapshotData snapshot;
	BlockNumber nth,
				last;

	if (entries < 1 || entries > PROBE_MAX_ENTRIES || ii->nkeycolumns != 1 || list_length(ip->indexclauses) != 1)
		return false;
	ic = linitial_node(IndexClause, ip->indexclauses);
	if (list_length(ic->indexquals) != 1 || !equality_key(ii, linitial_node(RestrictInfo, ic->indexquals)->clause, &key))
		return false;
	heap = table_open(rte->relid, NoLock);
	index = index_open(ii->indexoid, NoLock);
	InitNonVacuumableSnapshot(snapshot, GlobalVisTestFor(heap));
	scan = sift_index_beginscan(heap, index, &snapshot, 1);
	index_rescan(scan, &key, 1, NULL, 0);
	last = nth_entry_block(scan, BackwardScanDirection, 1);
	index_rescan(scan, &key, 1, NULL, 0);
	nth = nth_entry_block(scan, ForwardScanDirection, (int64) ceil(entries));
	index_endscan(scan);
	index_close(index, NoLock);
	table_close(heap, NoLock);
	if (last == InvalidBlockNumber)
		return false;
	/* fewer entries than wanted: the scan never holds as many rows as it needs, whatever it meets */
	span->hi = Min((double) last / Max(rel->pages, 1), 1.0);
	span->lo = nth == InvalidBlockNumber ? span->hi : Min((double) nth / Max(rel->pages, 1), span->hi);
	return true;
}

/* The heap positions of a bitmap's tuples; correlation mixes the span of a sorted column with the whole heap */
static HeapSpan
path_span(PlannerInfo *root, RelOptInfo *rel, Path *path, double entries)
{
	HeapSpan	span = whole_heap;

	if (IsA(path, BitmapAndPath))
		foreach_ptr(Path, p, ((BitmapAndPath *) path)->bitmapquals)
			span = span_intersect(span, path_span(root, rel, p, entries));
	else if (IsA(path, BitmapOrPath))
	{
		span = (HeapSpan) {1, 0};
		foreach_ptr(Path, p, ((BitmapOrPath *) path)->bitmapquals)
			span = span_union(span, path_span(root, rel, p, entries));
	}
	else if (IsA(path, IndexPath) && ((IndexPath *) path)->indexinfo->relam == BTREE_AM_OID &&
			 ((IndexPath *) path)->indexinfo->indexkeys[0] != 0)
	{
		IndexOptInfo *ii = ((IndexPath *) path)->indexinfo;
		double		rho;

		if (key_span(rel, planner_rt_fetch(rel->relid, root), (IndexPath *) path, entries, &span))
			return span;
		rho = index_correlation(root, rel, ii);

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

/* The comparisons of one btree descent, as the planner counts them for the first one (btcostestimate) */
static Cost
btree_descent_cost(IndexOptInfo *ii)
{
	return (ceil(log(Max(ii->tuples, 2)) / log(2.0)) + (Max(ii->tree_height, 0) + 1) * 50.0) * cpu_operator_cost;
}

/*
 * Leaf pages a btree scan with = ANY(array) quals descends to: the elements are visited in key order and
 * those on one leaf page share its descent, so a list inside a narrow range of the column touches few of
 * them, and a list spread over the column up to one per element.
 */
static double
saop_descents(PlannerInfo *root, RelOptInfo *rel, IndexPath *ip, double scans)
{
	IndexOptInfo *ii = ip->indexinfo;
	double		leaves = Max(ii->pages, 1),
				range = 1;

	foreach_node(IndexClause, ic, ip->indexclauses)
		foreach_node(RestrictInfo, rinfo, ic->indexquals)
			if (ic->indexcol == 0 && IsA(rinfo->clause, ScalarArrayOpExpr) && ii->indexkeys[0] != 0)
			{
				HeapSpan	values = qual_span(root, rel, ii, rinfo->clause);

				range = Min(range, Max(values.hi - values.lo, 0));
			}
	leaves *= range;
	return Max(leaves * (1 - exp(-scans / Max(leaves, 1))), 1);
}

/*
 * What an index path with = ANY(array) quals costs less than the planner charges: a random leaf page
 * for each element, a cold index, and one descent in all. The leaves are read in key order, at the
 * sequential page cost, and each of them is reached by a descent.
 */
static Cost
saop_saving(PlannerInfo *root, RelOptInfo *rel, Path *path)
{
	IndexPath  *ip;
	Cost		startup,
				total,
				saved = random_page_cost;
	Selectivity sel;
	double		scans = 1,
				correlation,
				pages,
				descents;

	if (IsA(path, BitmapAndPath) || IsA(path, BitmapOrPath))
	{
		Cost		sum = 0;

		foreach_ptr(Path, p, IsA(path, BitmapAndPath) ? ((BitmapAndPath *) path)->bitmapquals : ((BitmapOrPath *) path)->bitmapquals)
			sum += saop_saving(root, rel, p);
		return sum;
	}
	if (!IsA(path, IndexPath) || (ip = (IndexPath *) path)->indexinfo->relam != BTREE_AM_OID)
		return 0;
	foreach_node(IndexClause, ic, ip->indexclauses)
		foreach_node(RestrictInfo, rinfo, ic->indexquals)
			if (IsA(rinfo->clause, ScalarArrayOpExpr))
				scans *= estimate_array_length(root, lsecond(((ScalarArrayOpExpr *) rinfo->clause)->args));
	if (scans <= 1)
		return 0;
	random_page_cost = seq_page_cost;
	PG_TRY();
	{
		ip->indexinfo->amcostestimate(root, ip, 1.0, &startup, &total, &sel, &correlation, &pages);
	}
	PG_FINALLY();
	{
		random_page_cost = saved;
	}
	PG_END_TRY();
	descents = saop_descents(root, rel, ip, scans);
	total += (descents - 1) * btree_descent_cost(ip->indexinfo) - Max(pages - descents, 0) * seq_page_cost;
	return Max(ip->indextotalcost - total, 0);
}

void
sift_filter_estimate(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp, double entries, SiftFilterEst *est)
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
	index_cost -= saop_saving(root, rel, bp->bitmapqual);
	/* compute_bitmap_pages assumes the tuples are scattered; a correlated column packs them into fewer pages */
	minpages = Min(ceil(tuples / Max(rel->tuples, 1) * rel->pages), pages);
	correlation = btree_correlation(root, rel, bp->bitmapqual);
	pages = minpages + (1 - correlation * correlation) * (pages - minpages);
	maxentries = tbm_calculate_entries((Size) work_mem * 1024);
	lossy = pages > maxentries ? 1.0 - maxentries / pages : 0.0;
	est->sel = Min(tuples / Max(rel->tuples, 1), 1.0);
	est->sel += (Min(pages / Max(rel->pages, 1), 1.0) - est->sel) * lossy;
	est->cost = index_cost + (tuples + Min(pages, maxentries)) * FILTER_ENTRY_COST;
	span = path_span(root, rel, bp->bitmapqual, entries);
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
