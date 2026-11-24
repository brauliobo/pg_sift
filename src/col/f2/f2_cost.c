/*
 * col/f2/f2_cost.c - the cost of a scan of an index of format major 2. The marks of the key order and the zones of the time column leave only the granules that can hold a row of the quals, so the scan
 * reads the share of the index they leave, decodes the columns of its filters first and the others only for the rows that survive them, and returns the addresses of the rows that pass them in the order
 * of the addresses, so that the heap is read in order. Its inputs are what PostgreSQL itself has for the quals (the statistics of the table, never the counts the parts folded of rows a role may not see),
 * and its price is in the cost parameters of PostgreSQL (col/f2/f2_cost_model.h). An index-only scan also forms a tuple of the returned columns for each row it returns (col/f2/scan/ret.c), which costs as
 * much as the executor reading a heap tuple does. An equality or a short list on an integer, uuid or text column that is not the lead of the key is priced by its point directory (a key of several columns is asked by as many
 * lookups as the product of their values, never fewer than its fewest column, so the cheapest column prices it).
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "catalog/pg_type_d.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "access/genam.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_point_dir.h"
#include "col/f2/f2_cost_model.h"
#include "col/f2/scan/ord_stream.h"
#include "core/sift.h"
#include "catalog/pg_statistic.h"
#include "access/stratnum.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"

#define F2_RETURN_ROW_FACTOR	3.0

/* the clauses of the index scan on one column of the index */
static List *
clauses_on(const IndexPath *path, int col)
{
	List	   *l = NIL;

	foreach_node(IndexClause, ic, path->indexclauses)
		if (ic->indexcol == col)
			l = lappend(l, ic->rinfo);
	return l;
}

/* the selectivity of the clauses on the leading columns of leaves that all have one; 1 when the first has none */
static double
prefix_selectivity(PlannerInfo *root, const IndexPath *path, const SiftColSchema *schema, const uint16 *leaves, int n)
{
	List	   *prefix = NIL;

	for (int i = 0; i < n; i++)
	{
		List	   *c = clauses_on(path, schema->cols[leaves[i]].attno - 1);

		if (c == NIL)
			break;
		prefix = list_concat(prefix, c);
	}
	return prefix == NIL ? 1.0 : clauselist_selectivity(root, prefix, path->indexinfo->rel->relid, JOIN_INNER, NULL);
}

/* the share of the parts the window of the time columns leaves: the more selective of the two */
static double
time_selectivity(PlannerInfo *root, const IndexPath *path, const SiftColRel *r, const SiftF2Rel *f)
{
	double		sel = 1.0;

	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		const uint16 leaf = (uint16) (f->spec.time_cols[s] - 1);

		if (f->spec.time_cols[s] > 0)
			sel = Min(sel, prefix_selectivity(root, path, r->schema, &leaf, 1));
	}
	return sel;
}

/* the filter columns of the scan: how many, and the share of the rows the most selective one leaves */
static int
filter_columns(PlannerInfo *root, const IndexPath *path, double *lead)
{
	const IndexOptInfo *index = path->indexinfo;
	int			n = 0;

	*lead = 1.0;
	for (int c = 0; c < index->ncolumns; c++)
	{
		List	   *l = clauses_on(path, c);

		if (l == NIL)
			continue;
		n++;
		*lead = Min(*lead, clauselist_selectivity(root, l, index->rel->relid, JOIN_INNER, NULL));
	}
	return n;
}

/* Whether the point directory keys a column of this type: an integer, a date or a time, a uuid, a text whose collation compares bytes */
static bool
point_type(Oid typid, Oid collid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case UUIDOID:
			return true;
		case TEXTOID:
		case VARCHAROID:
			return !OidIsValid(collid) || get_collation_isdeterministic(collid);
		default:
			return false;
	}
}

/* The values an equality or a short list asks of a column a point directory answers from three pages each (at most the pages of the index); 0 for any other qual */
static int
point_values(PlannerInfo *root, const IndexClause *ic, const IndexOptInfo *index)
{
	const Node *clause = (const Node *) ic->rinfo->clause;
	Node	   *expr = (Node *) list_nth_node(TargetEntry, index->indextlist, ic->indexcol)->expr;
	int			nvals = 0;

	if (!point_type(exprType(expr), exprCollation(expr)))
		return 0;
	if (IsA(clause, OpExpr) && get_oprrest(((const OpExpr *) clause)->opno) == F_EQSEL)
		nvals = 1;
	else if (IsA(clause, ScalarArrayOpExpr) && ((const ScalarArrayOpExpr *) clause)->useOr && get_oprrest(((const ScalarArrayOpExpr *) clause)->opno) == F_EQSEL)
		nvals = estimate_array_length(root, (Node *) lsecond(((const ScalarArrayOpExpr *) clause)->args));
	return nvals <= SIFT_POINT_MAX_VALUES ? nvals : 0;
}

/* The lookups of the clause that asks the fewest values among those a point directory answers, 0 when none is one; the column that leads the key of the index (lead, or -1) has its marks instead */
static int
point_lookups(PlannerInfo *root, const IndexPath *path, int lead)
{
	int			fewest = 0;

	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const int	nvals = ic->indexcol == lead ? 0 : point_values(root, ic, path->indexinfo);

		if (nvals > 0 && (fewest == 0 || nvals < fewest))
			fewest = nvals;
	}
	return fewest;
}

/* the scan reads the directory of each value, then the granules its rows are in; the pages of a loop are those of the loops before it: what a later loop reads is what index_pages_fetched says the cache does not hold */
static void
point_cost(PlannerInfo *root, const IndexOptInfo *index, double loop_count, double tuples, double sel, int lookups, double share, Cost *total, double *pages)
{
	const double granules = Max(tuples / SIFT_GRANULE_ROWS, 1),
				touched = Min(granules, Max(tuples * sel, 1)),
				directory = Min(lookups * 3.0, *pages),
				read = directory + touched * (*pages / granules) * share;
	double		io_reuse = 1.0;

	if (loop_count > 1.0)
		io_reuse = sift_cost_clamp(index_pages_fetched(read * loop_count, Max(index->pages, 1), Max((double) index->pages, 1.0), root) / (read * loop_count), 0.02, 1.0);
	*total = read * random_page_cost * io_reuse + cpu_operator_cost * touched * SIFT_GRANULE_ROWS;
	*pages = touched * *pages / granules;
}

/* The correlation of the leading key column with the order of the rows of the heap, as the statistics of the table have it (what the rows of an ordered scan cost to fetch depends on it) */
static double
lead_correlation(PlannerInfo *root, const IndexOptInfo *index)
{
	const RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);
	HeapTuple	stats = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(rte->relid), Int16GetDatum(index->indexkeys[0]), BoolGetDatum(rte->inh));
	AttStatsSlot slot;
	double		corr = 0.0;

	if (!HeapTupleIsValid(stats))
		return corr;
	if (get_attstatsslot(&slot, stats, STATISTIC_KIND_CORRELATION, index->sortopfamily[0], ATTSTATSSLOT_NUMBERS))
	{
		corr = slot.nnumbers == 1 ? slot.numbers[0] : 0.0;
		free_attstatsslot(&slot);
	}
	ReleaseSysCache(stats);
	return index->reverse_sort[0] ? -corr : corr;
}

void
sift_f2_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation,
					   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *quals = get_quals_from_indexclauses(path->indexclauses);
	Selectivity sel = quals == NIL ? 1.0 : clauselist_selectivity(root, quals, index->rel->relid, JOIN_INNER, NULL);
	Relation	rel = index_open(index->indexoid, AccessShareLock);
	const SiftPlanSnapshot *plan = sift_plan_cache_get(rel, CurrentMemoryContext);
	const SiftColRel *r = sift_col_rel_private(rel, CurrentMemoryContext);
	const SiftF2Rel *f = sift_f2_rel(rel);
	const double rows = Max((double) plan->total_rows - (double) plan->total_dead, 1.0),
				f_time = time_selectivity(root, path, r, f),
				f_mark = f->nkey > 0 ? prefix_selectivity(root, path, r->schema, f->key_cols, f->nkey) : 1.0;
	const int	lookups = rows >= SIFT_POINT_MIN_ROWS ? point_lookups(root, path, f->nkey > 0 ? r->schema->cols[f->key_cols[0]].attno - 1 : -1) : 0;
	double		lead;
	SiftScanCostIn in = {.seq_page_cost = seq_page_cost,.cpu_operator_cost = cpu_operator_cost,.cpu_index_tuple_cost = cpu_index_tuple_cost,.block_size = BLCKSZ,.total_rows = rows,
	.total_bytes = Max((double) plan->total_bytes, 1.0),.total_granules = Max(ceil(rows / SIFT_COST_GRANULE_ROWS), 1.0),.sel = sel,.io_reuse = 1.0};
	SiftScanCost c;
	int			nparts = 0;

	for (int i = 0; i < plan->npartitions; i++)
		nparts += plan->partlists[i].nparts;
	in.parts_surv = Max(1.0, ceil(nparts * f_time));
	in.f_prune = sift_f_prune(rows * f_time, rows, f_mark, in.total_granules);
	in.n_filt = filter_columns(root, path, &lead);
	in.filt_share = Min(1.0, Max(in.n_filt, 1) / (double) Max(index->ncolumns, 1));
	in.sel_lead = Max(lead, sel);
	in.rescan = loop_count > 1.0;
	in.key_ordered = f->nkey > 0 && clauses_on(path, r->schema->cols[f->key_cols[0]].attno - 1) != NIL;
	c = sift_scan_cost(&in);
	if (loop_count > 1.0)
	{
		const double pages = Max(c.bytes_read / BLCKSZ, 1.0),
					fetched = index_pages_fetched(pages * loop_count, Max(index->pages, 1), Max((double) index->pages, 1.0), root);

		in.io_reuse = sift_cost_clamp(fetched / (pages * loop_count), 0.02, 1.0);
		c = sift_scan_cost(&in);
	}
	*indexStartupCost = c.startup;
	*indexTotalCost = c.total;
	if (path->path.pathtype == T_IndexOnlyScan)
		*indexTotalCost += cpu_index_tuple_cost * F2_RETURN_ROW_FACTOR * Max(sel * rows, 1.0);
	*indexSelectivity = sel;
	*indexCorrelation = path->path.pathkeys != NIL && index->sortopfamily != NULL ? lead_correlation(root, index) : c.correlation;
	if (path->path.pathkeys != NIL)
		*indexTotalCost += cpu_operator_cost * Max(sel * rows, 1.0) * log2(Max(nparts, 2));
	*indexPages = Max(index->pages, 1);
	if (lookups > 0)
		point_cost(root, index, loop_count, rows, sel, lookups, Min(1.0, Max(list_length(path->indexclauses), 1) / (double) Max(index->ncolumns, 1)), indexTotalCost, indexPages);
	index_close(rel, NoLock);
}

/* ---- the order of an index ---- */

/* The planner offers the order of the key columns of an index only when its scan gives it (sift_f2_native_order_matches): the opclasses tell the planner every index of sift_col has one */
static void
ord_withhold_unmatched(RelOptInfo *rel)
{
	const SiftCatalog *cat = sift_catalog();

	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		Relation	index;

		if (ii->relam != cat->col_am || ii->sortopfamily == NULL)
			continue;
		index = ii->hypothetical ? NULL : index_open(ii->indexoid, NoLock);
		if (index == NULL || !sift_f2_native_order_matches(index))
			ii->sortopfamily = NULL;
		if (index != NULL)
			index_close(index, NoLock);
	}
}

/* ---- keyset pagination ---- */

static const Node *
strip_relabel(const Node *n)
{
	while (IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	return n;
}

#if PG_VERSION_NUM >= 180000
#define ROWCMP(rc)	((int) (rc)->cmptype)
#else
#define ROWCMP(rc)	((int) (rc)->rctype)
#endif

static bool
row_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID;
}

/* The executor compares the members of a row by the comparison function of the family of the column: the operator and that function are both in it */
static bool
comparable_in(Oid opno, Oid opfamily)
{
	int			strategy;
	Oid			left,
				right;

	if (get_op_opfamily_strategy(opno, opfamily) == 0)
		return false;
	get_op_opfamily_properties(opno, opfamily, false, &strategy, &left, &right);
	return OidIsValid(get_opfamily_proc(opfamily, left, right, 1));
}

/*
 * PostgreSQL offers a row comparison (a, b) > ($1, $2) to btree only: for the other access methods it is a filter that is tested on every row an ordered scan reads from the start. The clause is
 * offered to the scan as a lossy index clause on the leading columns (the node still tests it: the clause the scan gets is one of the families of the index, so that the planner does not find it the same as the filter), as its integer or date or timestamp members, of the type of their column, give the scan a range of the key.
 */
static IndexClause *
rowcompare_clause(PlannerInfo *root, const RelOptInfo *rel, const IndexOptInfo *ix, RestrictInfo *rinfo)
{
	const RowCompareExpr *rc = (const RowCompareExpr *) rinfo->clause;
	IndexClause *ic;
	List	   *cols = NIL;
	ListCell   *l,
			   *r,
			   *o;
	RowCompareExpr *index_rc;
	int			i = 0;

	if (!IsA(rc, RowCompareExpr) || ROWCMP(rc) == COMPARE_EQ || list_length(rc->largs) > Min(ix->nkeycolumns, SIFT_BOUND_COMPS))
		return NULL;
	forthree(l, rc->largs, r, rc->rargs, o, rc->opnos)
	{
		const Node *left = strip_relabel(lfirst(l));
		const Node *right = lfirst(r);

		if (!IsA(left, Var) || ((const Var *) left)->varno != rel->relid || ((const Var *) left)->varlevelsup != 0 || ((const Var *) left)->varattno != ix->indexkeys[i] ||
			!row_type(exprType(left)) || !row_type(exprType(right)) || (exprType(left) != exprType(right) && (exprType(left) == DATEOID || exprType(left) == TIMESTAMPOID || exprType(left) == TIMESTAMPTZOID)) ||
			contain_volatile_functions((Node *) right) || bms_is_member(rel->relid, pull_varnos(root, (Node *) right)) || !comparable_in(lfirst_oid(o), ix->opfamily[i]))
			return NULL;
		cols = lappend_int(cols, i++);
	}
	index_rc = copyObject((RowCompareExpr *) rc);
	index_rc->opfamilies = NIL;
	for (int c = 0; c < i; c++)
		index_rc->opfamilies = lappend_oid(index_rc->opfamilies, ix->opfamily[c]);
	ic = makeNode(IndexClause);
	ic->rinfo = rinfo;
	ic->indexquals = list_make1(make_simple_restrictinfo(root, (Expr *) index_rc));
	ic->lossy = true;
	ic->indexcol = 0;
	ic->indexcols = cols;
	return ic;
}

static set_rel_pathlist_hook_type prev_pathlist_hook;

/* The ordered index paths of the relation again with the row comparison among their index clauses */
static void
ord_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *added = NIL;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	foreach_ptr(Path, p, rel->pathlist)
	{
		const IndexPath *ip = (const IndexPath *) p;
		IndexClause *ic = NULL;

		if (!IsA(p, IndexPath) || p->pathkeys == NIL || p->param_info != NULL || ip->indexinfo->relam != cat->col_am)
			continue;
		foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
			if (ic == NULL)
				ic = rowcompare_clause(root, rel, ip->indexinfo, ri);
		foreach_node(IndexClause, have, ip->indexclauses)
			if (ic != NULL && have->rinfo == ic->rinfo)
				ic = NULL;
		if (ic != NULL)
			added = lappend(added, create_index_path(root, ip->indexinfo, lappend(list_copy(ip->indexclauses), ic), ip->indexorderbys, ip->indexorderbycols, p->pathkeys, ip->indexscandir,
													 p->pathtype == T_IndexOnlyScan, NULL, 1.0, false));
	}
	foreach_ptr(Path, np, added)
		add_path(rel, np);
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
ord_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	ord_withhold_unmatched(rel);
}

void
sift_f2_ord_plan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = ord_get_relation_info;
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = ord_pathlist;
}
#else
static build_simple_rel_hook_type prev_simple_rel_hook;

static void
ord_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	ord_withhold_unmatched(rel);
}

void
sift_f2_ord_plan_init(void)
{
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = ord_build_simple_rel;
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = ord_pathlist;
}
#endif
