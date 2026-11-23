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

/* the scan reads the directory of each value, then the granules its rows are in */
static void
point_cost(double tuples, double sel, int lookups, double share, Cost *total, double *pages)
{
	const double granules = Max(tuples / SIFT_GRANULE_ROWS, 1),
				touched = Min(granules, Max(tuples * sel, 1)),
				directory = Min(lookups * 3.0, *pages);

	*total = directory * random_page_cost + touched * (*pages / granules) * share * random_page_cost + cpu_operator_cost * touched * SIFT_GRANULE_ROWS;
	*pages = touched * *pages / granules;
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
	*indexCorrelation = c.correlation;
	*indexPages = Max(index->pages, 1);
	if (lookups > 0)
		point_cost(rows, sel, lookups, Min(1.0, Max(list_length(path->indexclauses), 1) / (double) Max(index->ncolumns, 1)), indexTotalCost, indexPages);
	index_close(rel, NoLock);
}
