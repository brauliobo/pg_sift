/*
 * col/amscan/cost.c - the planner side of a sift_col index: cost estimate, relation info
 * address paths, the rows-only plain index, the bitmap switches
 * the planner hooks that make a column scan a path of the relation
 */
#include "postgres.h"
#include <math.h>
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "access/genam.h"
#include "catalog/pg_statistic.h"
#include "utils/syscache.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "col/pg.h"
#include "col/am.h"
#include "col/amscan/amscan_int.h"

/*
 * col/amscan/amscan.c - the bitmap scan of a sift_col index (docs/COL-SCAN.md): the keys of the operator classes sift.col_*_ops become
 * predicates of the column source, the TIDs of the rows that pass go to the TID bitmap, and the heap scan above applies MVCC.
 * No amgettuple: an index scan has no order to give (units are in TID order), ordered reads stay with the top-N of SiftAgg.
 */

/*
 * The cost of a scan, in microseconds of a warm run (the figures below are measured, docs/COL-SCAN.md), turned into the planner's units by SIFT_COST_COL_US_PER_UNIT (col/pg.h)
 * in sift_col_amcostestimate, and scaled with cpu_operator_cost like the rest of the planner's costs.
 */
#define COL_SCAN_FIXED		24.0	/* opening the index, the snapshot and the directory of the segment: the startup */

#define COL_SCAN_FIXED_TID	12.0	/* ... for a lookup the directory of heap addresses answers: the source is opened and the first addresses read in 67,000 instructions (16 us measured, 4 of them the probe) */

#define COL_SCAN_WALK		0.03	/* the zones and summaries of one unit tested against the constants */

#define COL_SCAN_WORD		0.002	/* ... when the point directory answered, so only the bits of the kept units are walked */

#define COL_SCAN_PROBE		4.0		/* one key looked up in a point directory: its slots are a page or two of a table the lookups of other keys do not share */

#define COL_SCAN_UNIT		22.0	/* reading and decoding the head of a unit that is kept, besides its pages */

#define COL_SCAN_UNIT_RESCAN	5.0		/* ... for the first unit of each probe of a scan repeated for the rows of a join: the unit is looked up by the key, not walked (8.5 us a probe of 1,000 in a nested loop over 1,000,000 rows with the heap row and the join) */
#define COL_SCAN_UNIT_PAGES	3		/* the pages of that head, or of the whole unit where the index is narrower */

#define COL_SCAN_ROW		0.0005	/* the filter of one value of a column of a kept unit by a comparison: 0.4 to 0.5 ns measured over 3,000,000 rows of a date, an integer, a text and a numeric column (the bounds of a range are one pass over the lanes) */

#define COL_SCAN_ROW_VAR	0.009	/* ... of a column tested by anything but a comparison (a pattern: 2.8 ns a row measured for a text, the elements of an array one by one) */

#define COL_SCAN_EMIT_IOS	0.055	/* ... to an index-only scan, which forms a tuple of the columns the plan reads and no heap row: 56 ns a row for an index of 3 columns, 62 for 12 and 85 for 30 (100,000 rows of a table of 80 columns, whose heap takes 6.7 ms to scan), so 0.055 and */
#define COL_SCAN_ORDERED	0.045	/* a row of an ordered index-only scan (ordpath.c) of a whole order array: the merge of the segments and the write buffers, no unit read. 1,000,000 rows of a random key, summed: 1,019 Minstr and 133 ms where Seq Scan and Sort take 1,266 and 211 (run b5080144b211-20261102T020207Z-39fc); the heap plan is 1.75 us a cost unit */
#define COL_SCAN_ORDERED_GROUP	0.6	/* ... the open of the merge of one group of an ordered pair (ordered.c): the group looked up in each segment, a cursor made, the chunk the last group decoded kept: 4.6 k instructions (0.23) measured (perf stat on a development cluster) over 300,000 rows of random keys in 1,000, 30,000 and 100,000 groups (276, 410 and 730 Minstr). 0.6 is where the plan changes to Seq Scan and Sort at the rows a group where they take the same time: 45 ms and 42 at 3 rows a group (100,000 groups), 38 and 46 at 5, 26 and 55 at 10; the Sort is priced at 1.7 times its time there (40,700 units against 42 ms), which the term makes up for */
#define COL_SCAN_ORDERED_SORT	0.042	/* ... of an order with compact chunks or the ends only (the default build): the units are read in batches and the rows sorted by key and address, 913 Minstr and 109 ms in the same run */

#define COL_SCAN_EMIT_IOS_COL	0.001	/* ... for each column of the index */
#define COL_SCAN_EMIT_IOS_VAR	0.06	/* ... and for each text column the plan reads, which is formed by the path of a variable width (115 ns a row with one, 56 without); the TID of the bitmap node takes 10 ns */

#define COL_SCAN_RESCAN		0.9		/* a probe whose key a parent query passes down (a correlated subplan) costs at least this share of the sequential scan of the table: see rescan_floor */

#define COL_SCAN_KEY		20.0	/* a key of an IN list past the first that is not looked up in a directory of addresses (the units that may hold it are read, or the sorted keys are sought for it): the units that may hold it are searched for it, 2.5 us measured a key (60 to 500 keys of 300,000 and 1,000,000 rows, 0.16 to 1.15 ms) where the b-tree's probe takes 0.8 us and is priced 7: the price of a b-tree's cached probe is 9 units to the microsecond, and a scan that is priced by its microseconds would take the lookup of 500 keys from it for twice the time */
#define COL_SCAN_FETCH		0.5		/* the heap row of each tuple an index scan (not a bitmap) hands over, for the probes of a scan repeated for the rows of a join: PostgreSQL's price of the page falls with the loops (the pages stay in the cache) and the tuple stays 1.2 us (124 rows a probe, 200 us measured, priced 94 units) */
#define COL_SCAN_FETCH_ROW	0.1		/* ... and for the rows of a scan that is not repeated, the heap row beside the index tuple: PostgreSQL prices the pages (sequential or cached) and 10 ns the tuple */
#define COL_SCAN_IOS_PROBE	15.0	/* the tuple of the columns an index-only scan forms for the row of each probe of a scan repeated for the rows of a join, whose unit it decodes: 17 to 26 us a probe (1,000 probes of 100,000 to 1,000,000 rows, 100 of 20,000) where the plain index scan hands over the address and the heap reads the row, 1.4 to 9.5 */
#define COL_SCAN_EMIT		0.01		/* handing one row over: its position in the unit, its TID and its visibility, 10 to 12 ns a row measured in the bitmap node (a year of 600,000 rows 7 ms, all 3,000,000 rows 30 ms); the cost units are about a microsecond, as the constants above */

bool		sift_enable_col_scan = true;

double		sift_plan_scale = 1.0;

static bool
exec_param_of_parent(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC)
	{
		for (PlannerInfo *up = root->parent_root; up != NULL; up = up->parent_root)
			foreach_node(PlannerParamItem, ppi, up->plan_params)
				if (ppi->paramId == ((Param *) node)->paramid)
					return true;
		return false;
	}
	return expression_tree_walker(node, exec_param_of_parent, root);
}

/*
 * A scan whose key is a parameter of a parent query (EXISTS (SELECT .. WHERE t.k = outer.k), a scalar subselect) runs once for each row the parent evaluates it for,
 * and PostgreSQL multiplies the price of one run by its estimate of those rows (twice the rows of the node, which a few correlated filters put 1,000 times too low)
 * to choose between that plan and a hashed subplan, which pays once. A probe of a point directory (0.2 to 0.4 ms, 270 units) multiplied by 10 rows looks cheaper than
 * the pass over the table (28,000 units) and runs 5,000 times: 2.9 s against 92 ms. So the total of the probe is never below COL_SCAN_RESCAN of the sequential scan
 * (PostgreSQL divides both by the rows a key matches to price an EXISTS, so the ratio holds there): the hashed subplan wins whenever it is there, as it does with the scan
 * off, and where none fits in memory the probe still beats the sequential scan for each row (0.03 ms against 2 ms).
 */
static double
rescan_floor(PlannerInfo *root, IndexPath *path)
{
	IndexOptInfo *index = path->indexinfo;

	/* a parent that has its relations is planning a subquery of its FROM list (a LATERAL one): its loops are the rows of the join, which the join prices, not the estimate of a subplan's calls */
	if (root->parent_root == NULL || root->parent_root->simple_rel_array != NULL)
		return 0.0;
	foreach_node(IndexClause, ic, path->indexclauses)
		foreach_node(RestrictInfo, ri, ic->indexquals)
			if (exec_param_of_parent((Node *) ri->clause, root))
				return COL_SCAN_RESCAN * (index->rel->pages * seq_page_cost + index->rel->tuples * cpu_tuple_cost);
	return 0.0;
}

/*
 * The scans genericcostestimate counts for the lists of a leaf that the scan cuts to the keys all of its equalities hold (narrow_lists): the product of their lengths over
 * the fewest keys of any of them, 1 for an equality; *narrowed = the leaves.
 */
static double
redundant_scans(PlannerInfo *root, IndexPath *path, Bitmapset **narrowed)
{
	IndexOptInfo *index = path->indexinfo;
	double		redundant = 1.0;

	for (int col = 0; col < index->nkeycolumns; col++)
	{
		double		product = 1.0,
					fewest = 0.0;
		int			tests = 0;

		foreach_node(IndexClause, ic, path->indexclauses)
		{
			if (ic->indexcol != col)
				continue;
			foreach_node(RestrictInfo, ri, ic->indexquals)
			{
				double		keys = 0.0;

				if (IsA(ri->clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) ri->clause)->useOr &&
					get_op_opfamily_strategy(((ScalarArrayOpExpr *) ri->clause)->opno, index->opfamily[col]) == BTEqualStrategyNumber)
				{
					keys = Max(1.0, estimate_array_length(root, lsecond(((ScalarArrayOpExpr *) ri->clause)->args)));
					product *= keys;
				}
				else if (IsA(ri->clause, OpExpr) && get_op_opfamily_strategy(((OpExpr *) ri->clause)->opno, index->opfamily[col]) == BTEqualStrategyNumber)
					keys = 1.0;
				if (keys > 0.0)
				{
					fewest = tests++ == 0 ? keys : Min(fewest, keys);
				}
			}
		}
		if (tests > 1)
		{
			redundant *= product / fewest;
			*narrowed = bms_add_member(*narrowed, col);
		}
	}
	return redundant;
}

/* the text columns (the ones with a collation) of the index that the plan reads */
static int
ios_var_columns(IndexOptInfo *index)
{
	Bitmapset  *used = NULL;
	int			n = 0;

	pull_varattnos((Node *) index->rel->reltarget->exprs, index->rel->relid, &used);
	foreach_node(RestrictInfo, ri, index->rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, index->rel->relid, &used);
	for (int col = 0; col < index->nkeycolumns; col++)
		n += OidIsValid(index->indexcollations[col]) && bms_is_member(index->indexkeys[col] - FirstLowInvalidHeapAttributeNumber, used);
	return n;
}

/*
 * The cost of the scan in the planner's units (about microseconds, see COL_SCAN_FIXED): the startup is the opening of the index and the pruning of the units (the
 * zones and summaries of each, or the keys looked up in the point directory and the bits of the kept units); the rest is reading the kept units, filtering their
 * values (only the rows the directory lists, when it is what keeps them) and handing the rows over. The start is separate so that a LIMIT stops the scan early: the units are read one at a time.
 */
void
sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
						Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Relation	irel;
	double		kept,
				pkept,
				tid,
				extra,
				units,
				kept_units,
				idx_rows,
				rescan,
				repeat,
				row_cost,
				scale = cpu_operator_cost / 0.0025 / SIFT_COST_COL_US_PER_UNIT;
	Bitmapset  *cols = NULL,
			   *slow = NULL,
			   *narrowed = NULL;
	double		redundant = redundant_scans(root, path, &narrowed);
	int			nprobes;

	/* the bounds of a range on one column are one test of it; a column with a clause that is no comparison of the operator class (a pattern, an array operator) keeps the price of a value tested one by one; IS [NOT] NULL tests a bit, as a comparison does */
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		cols = bms_add_member(cols, ic->indexcol);
		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			int			strategy = IsA(ri->clause, OpExpr) ? get_op_opfamily_strategy(((OpExpr *) ri->clause)->opno, index->opfamily[ic->indexcol]) : 0;

			if ((strategy < BTLessStrategyNumber || strategy > BTGreaterStrategyNumber) && !IsA(ri->clause, NullTest) &&
				!(IsA(ri->clause, ScalarArrayOpExpr) && bms_is_member(ic->indexcol, narrowed)))
				slow = bms_add_member(slow, ic->indexcol);
		}
	}
	row_cost = bms_num_members(bms_difference(cols, slow)) * COL_SCAN_ROW;
	foreach_node(IndexClause, ic, path->indexclauses)
		if (bms_is_member(ic->indexcol, slow))
			row_cost += COL_SCAN_ROW_VAR;

	genericcostestimate(root, path, loop_count, &costs);
	if (path->indexscandir != NoMovementScanDirection && path->path.pathkeys != NIL)
	{
		/* the order of the first column is walked from its start: every row of the index, at the price of the merge (ordpath.c); a compact order is sorted by the first row, so its work is the startup */
		bool		pair = index->indextlist != NIL && ((TargetEntry *) linitial(index->indextlist))->resname != NULL && strcmp(((TargetEntry *) linitial(index->indextlist))->resname, COL_ORDERED_PAIR_MARK) == 0;
		bool		whole = pair || sift_col_ordered_whole(index->indexoid);
		double		walk = costs.numIndexTuples * scale;
		/* a pair opens the merge of each group of the first column (g = value: that one) */
		double		groups = pair && path->indexclauses != NIL ? 1.0 : pair ? Min(estimate_num_groups(root, list_make1(((TargetEntry *) linitial(index->indextlist))->expr), Max(index->rel->tuples, 1.0), NULL, NULL), costs.numIndexTuples) : 0.0;

		*indexStartupCost = scale * COL_SCAN_FIXED + (whole ? 0.0 : walk * COL_SCAN_ORDERED_SORT);
		*indexTotalCost = scale * COL_SCAN_FIXED + (whole ? walk * COL_SCAN_ORDERED : walk * COL_SCAN_ORDERED_SORT) + groups * scale * COL_SCAN_ORDERED_GROUP + costs.numIndexTuples * cpu_index_tuple_cost;
		*indexSelectivity = costs.indexSelectivity;
		*indexCorrelation = 0.0;
		*indexPages = Max(1.0, (double) index->pages);
		return;
	}
	irel = index_open(index->indexoid, AccessShareLock);
	units = Max(1.0, ceil(index->rel->pages / 256.0));
	pkept = point_kept(root, path, irel, units, &nprobes, &tid, &extra);
	kept = Min(Min(zones_kept(root, path, irel), sets_kept(root, path, irel)), pkept);
	index_close(irel, AccessShareLock);
	kept_units = kept * units;
	/* an IN list or ANY (array) is a scan for each element, and genericcostestimate counts the tuples of one: the lists of a leaf that has an equality, or another list, are one test of the fewest keys (narrow_lists) */
	idx_rows = costs.numIndexTuples * Max(costs.num_sa_scans / redundant, 1);
	/* a scan repeated for the rows of a join (loop_count) opens the index once and walks its pages once: a probe is the unit it looks up (82 units priced against 8.5 us measured) */
	repeat = Max(loop_count, 1.0);
	/* the seek in the sorted keys is made for each key a probe passes down: it is not shared by the loops, as the opening of the index is */
	costs.indexStartupCost = scale * ((COL_SCAN_FIXED - (nprobes > 0 ? tid : 0.0) * (COL_SCAN_FIXED - COL_SCAN_FIXED_TID) + (nprobes > 0 ? nprobes * COL_SCAN_PROBE + kept_units * COL_SCAN_WORD : units * COL_SCAN_WALK)) / repeat + extra);
	costs.indexTotalCost = costs.indexStartupCost
		+ (nprobes > 0 ? 1.0 - tid : 1.0) * (scale * (repeat > 1.0 ? Min(kept_units, 1.0) * COL_SCAN_UNIT_RESCAN + Max(kept_units - 1.0, 0.0) * COL_SCAN_UNIT : kept_units * COL_SCAN_UNIT) +
											  kept_units * Min(COL_SCAN_UNIT_PAGES, Max(1.0, index->pages / units)) * seq_page_cost)
		+ (nprobes > 0 ? idx_rows * (1.0 - tid) : kept * index->rel->tuples) * row_cost * scale
		+ (nprobes == 0 || extra > 0.0 ? Max(costs.num_sa_scans / redundant - 1.0, 0.0) * COL_SCAN_KEY * scale : 0.0)
		+ index->pages * cpu_tuple_cost / repeat
		+ (path->path.pathtype == T_IndexScan ? idx_rows * (repeat > 1.0 ? COL_SCAN_FETCH : COL_SCAN_FETCH_ROW) * scale : (repeat > 1.0 ? COL_SCAN_IOS_PROBE * scale : 0.0))
		+ idx_rows * (scale * (path->path.pathtype == T_IndexOnlyScan ? COL_SCAN_EMIT_IOS + COL_SCAN_EMIT_IOS_COL * index->nkeycolumns + COL_SCAN_EMIT_IOS_VAR * ios_var_columns(index) : COL_SCAN_EMIT) + cpu_index_tuple_cost);
	rescan = rescan_floor(root, path);
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = Max(costs.indexTotalCost, rescan);
	*indexSelectivity = costs.indexSelectivity;
	/* the addresses of a lookup come in heap order (a list ascends, the segments follow each other), so the tuple scan reads the heap as a bitmap scan does */
	*indexCorrelation = nprobes > 0 ? tid : 0.0;
	*indexPages = Max(1.0, kept * index->pages);
}

/* sift.plan_scale: the table the index covers has that many times the pages and rows, with the statistics of the columns as they are (an absolute n_distinct stays, a fraction grows) */
static void
col_scale_rel(RelOptInfo *rel)
{
	if (sift_plan_scale <= 1.0)
		return;
	rel->pages = (BlockNumber) Min(rel->pages * sift_plan_scale, (double) (MaxBlockNumber - 1));
	rel->tuples *= sift_plan_scale;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		ii->pages = (BlockNumber) Min(ii->pages * sift_plan_scale, (double) (MaxBlockNumber - 1));
		ii->tuples *= sift_plan_scale;
	}
}

/* ---- off means off ---- */

/*
 * With sift.enable_col_scan off the planner must build no bitmap path on a sift_col index: a path with a bad cost still beats one that
 * enable_seqscan = off disabled (PostgreSQL counts the disabled nodes before the cost). The index stays in the list, SiftAgg reads it there.
 * Up to PostgreSQL 18 get_relation_info_hook clears amhasgetbitmap of the index; 19 has no such hook, so the bitmap paths that use one are
 * disabled in the pathlist of the relation, one node more than a sequential scan that enable_seqscan turned off. sift.plan_scale is applied by
 * get_relation_info_hook up to 18 and by build_simple_rel_hook, which replaces it, in 19.
 */
static set_rel_pathlist_hook_type prev_pathlist_hook;

/*
 * The probes a parameterized path is run for: the rows of the smallest outer relation it reads (indxpath.c's get_loop_count, which is not exported). The path beside an
 * index-only scan of a join is priced for as many as the scan is: for one probe a unit is walked, not looked up, and the plain path never beat the index-only one.
 */
static double
probe_loops(PlannerInfo *root, Relids outer)
{
	double		loops = 0.0;
	int			x = -1;

	while ((x = bms_next_member(outer, x)) >= 0)
	{
		RelOptInfo *o = root->simple_rel_array[x];

		if (o != NULL && o->reloptkind == RELOPT_BASEREL)
			loops = loops == 0.0 ? o->rows : Min(loops, o->rows);
	}
	return Max(loops, 1.0);
}

/*
 * A query the index covers gets an index-only path, and the bitmap path PostgreSQL builds beside it carries the price of that one: a read of every kept unit (the
 * lookup of a key in the point directory returns the addresses and reads none, when the directory lists them). A lookup by one clause of the directory therefore
 * reads its units through the index-only path, 20 to 50 times slower than the bitmap path on the addresses. The bitmap path is added priced as what it does.
 */
static void
add_address_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *added = NIL;

	foreach_ptr(Path, p, rel->pathlist)
	{
		IndexPath  *ip = (IndexPath *) p;
		IndexPath  *bp;

		if (!IsA(p, IndexPath) || p->pathtype != T_IndexOnlyScan || ip->indexinfo->relam != sift_col_am_oid() || ip->indexclauses == NIL ||
			ip->indexorderbys != NIL)
			continue;
		bp = create_index_path(root, ip->indexinfo, ip->indexclauses, ip->indexorderbys, ip->indexorderbycols, ip->path.pathkeys, ip->indexscandir, false,
							   PATH_REQ_OUTER(&ip->path), probe_loops(root, PATH_REQ_OUTER(&ip->path)), false);
		if (bp->path.total_cost >= ip->path.total_cost)
			continue;
		added = lappend(added, bp);
		added = lappend(added, create_bitmap_heap_path(root, rel, (Path *) bp, bms_union(rel->lateral_relids, PATH_REQ_OUTER(&bp->path)), 1.0, 0));
	}
	foreach_ptr(Path, p, added)
		add_path(rel, p);
}

/*
 * A column of the default class sift.col_ops (what CREATE INDEX .. USING sift_col (col) gives) is in the family of the typed classes (pg_sift--3.6.sql), so a
 * statement that returns rows (WHERE id = 5, an IN list, a range, LIKE 'abc%') can take an index or bitmap scan of a plain index. A statement SiftAgg can answer,
 * one that aggregates, groups, deduplicates, has a window function or sorts under a LIMIT, keeps the paths it always had on such an index: an index whose columns
 * are all of the default class gives it no index or bitmap path (a path built and then dropped would take the sequential scan it dominates with it), so that the
 * calibrated choice between SiftAgg and the normal plan is the one of 3.5 for them. An index with a column of a named class is offered everything, as before.
 * SiftAgg reads the index alone, so a statement that reads a column the index does not hold (avg(score) over a window of the dates) is not its to answer, and the
 * index keeps its paths for the rows of the window.
 */
static bool
index_holds_columns_read(PlannerInfo *root, IndexOptInfo *ii)
{
	Query	   *q = root->parse;
	Bitmapset  *used = NULL;
	int			x = -1;

	pull_varattnos((Node *) q->targetList, ii->rel->relid, &used);
	pull_varattnos((Node *) q->jointree, ii->rel->relid, &used);
	pull_varattnos(q->havingQual, ii->rel->relid, &used);
	while ((x = bms_next_member(used, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;
		bool		held = false;

		for (int i = 0; i < ii->ncolumns && !held; i++)
			held = ii->indexkeys[i] == attno;
		if (!held)
			return false;
	}
	return true;
}

/* the most keys of an IN list for which an index scan with a heap fetch for each competes with the unit reads of SiftAgg (SIFT_CORDER_MAX_KEYS) */
#define POINT_LOOKUP_KEYS	512
/*
 * ... and the rows they return: an index scan prices the heap addresses of a lookup as in heap order, which holds for a few rows and not for hundreds (250,000 rows, one day of 216: priced 76
 * against 187 for SiftAgg, takes 1.4 ms against 0.55), so a column of hundreds of rows a key is SiftAgg's, whose zones and key lists keep few units
 */
#define POINT_LOOKUP_ROWS_KEY	16
#define POINT_LOOKUP_ROWS		1024

/* whether qual is a constant key or a list of up to POINT_LOOKUP_KEYS of them for a column of the index (of the table heapid) that has a point directory of addresses or an order array */
static bool
point_lookup_qual(PlannerInfo *root, Node *qual, IndexOptInfo *ii, Oid heapid, const char *tid_cols, const char *ord_cols)
{
	Node	   *l,
			   *r;
	Var		   *var;
	Const	   *c;
	char	   *colname;
	double		keys = 1.0;

	if (IsA(qual, OpExpr) && list_length(((OpExpr *) qual)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) qual;
		char	   *name = get_opname(op->opno);
		bool		eq = name != NULL && strcmp(name, "=") == 0;

		if (!eq)
			return false;
		l = linitial(op->args);
		r = lsecond(op->args);
	}
	else if (IsA(qual, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) qual)->useOr)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) qual;
		char	   *name = get_opname(sa->opno);

		if (name == NULL || strcmp(name, "=") != 0)
			return false;
		l = linitial(sa->args);
		r = lsecond(sa->args);
	}
	else
		return false;
	if (IsA(qual, OpExpr) && IsA(r, Var) && IsA(l, Const))
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	while (IsA(l, RelabelType))
		l = (Node *) ((RelabelType *) l)->arg;
	if (!IsA(l, Var) || !IsA(r, Const))
		return false;
	var = (Var *) l;
	c = (Const *) r;
	if (var->varno != ii->rel->relid || var->varlevelsup != 0 || c->constisnull)
		return false;
	if (IsA(qual, ScalarArrayOpExpr))
	{
		ArrayType  *arr = DatumGetArrayTypeP(c->constvalue);

		keys = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
		if (keys > POINT_LOOKUP_KEYS)
			return false;
	}
	colname = get_attname(heapid, var->varattno, true);
	if (colname == NULL)
		return false;
	for (int i = 0; i < ii->nkeycolumns; i++)
		if (ii->indexkeys[i] == var->varattno)
		{
			HeapTuple	stats;
			double		distinct,
						per_key;

			if (!sift_col_list_has(tid_cols, colname) && !sift_col_list_has(ord_cols, colname))
				return false;
			/* the statistics of the column by their cache: the relation is not in the planner's arrays yet, so examine_variable cannot be asked */
			stats = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(heapid), Int16GetDatum(var->varattno), BoolGetDatum(false));
			if (!HeapTupleIsValid(stats))
				return false;
			distinct = ((Form_pg_statistic) GETSTRUCT(stats))->stadistinct;
			ReleaseSysCache(stats);
			per_key = distinct < 0 ? 1.0 / -distinct : ii->rel->tuples * Max(sift_plan_scale, 1.0) / Max(distinct, 1.0);
			return per_key <= POINT_LOOKUP_ROWS_KEY && keys * per_key <= POINT_LOOKUP_ROWS;
		}
	return false;
}

/*
 * A statement that aggregates and asks for the rows of a few keys of a column the index can look up by itself (an equality or an IN list of constants, a directory of heap
 * addresses or an order array on the column): the rows are a few heap pages away, and SiftAgg reads a unit chunk of each key (about 40 buffer accesses against the 4 of a heap
 * page), so the index and bitmap paths stay and the costs choose (1,000,000 rows, 60 keys: 0.47 ms of SiftAgg, 0.07 of the index scan with its aggregate).
 */
static bool
point_lookup_only(PlannerInfo *root, IndexOptInfo *ii)
{
	Query	   *q = root->parse;
	Relation	idx;
	const char *tid_cols,
			   *ord_cols;
	List	   *quals;
	bool		ok = q->jointree != NULL && q->jointree->quals != NULL && list_length(q->jointree->fromlist) == 1 && q->groupClause == NIL && q->groupingSets == NIL &&
		q->distinctClause == NIL && !q->hasWindowFuncs && q->sortClause == NIL;

	if (!ok)
		return false;
	idx = index_open(ii->indexoid, NoLock);
	/* copies: the catalog lookups below can reload the index entry, which frees its options */
	tid_cols = pstrdup(sift_col_options_point_tid_columns(idx));
	ord_cols = sift_col_options_order_array_columns(idx);
	ord_cols = pstrdup(ord_cols ? ord_cols : "");
#ifdef SIFT_TESTING
	sift_col_test_inval(idx);	/* the catalog lookups below can take a lock that reloads the entry */
#endif
	quals = IsA(q->jointree->quals, List) ? (List *) q->jointree->quals : list_make1(q->jointree->quals);
	if (IsA(q->jointree->quals, BoolExpr) && ((BoolExpr *) q->jointree->quals)->boolop == AND_EXPR)
		quals = ((BoolExpr *) q->jointree->quals)->args;
	foreach_ptr(Node, qual, quals)
		if (!point_lookup_qual(root, qual, ii, planner_rt_fetch(ii->rel->relid, root)->relid, tid_cols, ord_cols))
		{
			ok = false;
			break;
		}
	index_close(idx, NoLock);
	return ok;
}

static void
plain_index_for_rows_only(PlannerInfo *root, IndexOptInfo *ii)
{
	Query	   *q = root->parse;

	if (ii->relam != sift_col_am_oid() ||
		!(q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->distinctClause != NIL || q->hasWindowFuncs ||
		  (q->sortClause != NIL && (q->limitCount != NULL || q->limitOffset != NULL))))
		return;
	for (int i = 0; i < ii->nkeycolumns; i++)
		if (ii->opcintype[i] != ANYELEMENTOID)
			return;
	if (!index_holds_columns_read(root, ii) || point_lookup_only(root, ii))
		return;
	ii->amhasgetbitmap = ii->amhasgettuple = false;
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
col_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	bool		indexed = false;

	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
		{
			indexed = true;
			plain_index_for_rows_only(root, ii);
			if (!sift_enable_col_scan)
				ii->amhasgetbitmap = ii->amhasgettuple = false;
		}
	if (indexed)
		col_scale_rel(rel);
}

static void
col_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (sift_enable_col_scan)
	{
		add_address_paths(root, rel);
		sift_col_add_ordered_paths(root, rel);
	}
}

void
sift_col_scan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = col_get_relation_info;
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = col_set_rel_pathlist;
}
#else
static bool
bitmap_uses_col(Path *path)
{
	switch (nodeTag(path))
	{
		case T_IndexPath:
			return ((IndexPath *) path)->indexinfo->relam == sift_col_am_oid();
		case T_BitmapAndPath:
			foreach_ptr(Path, p, ((BitmapAndPath *) path)->bitmapquals)
				if (bitmap_uses_col(p))
					return true;
			return false;
		case T_BitmapOrPath:
			foreach_ptr(Path, p, ((BitmapOrPath *) path)->bitmapquals)
				if (bitmap_uses_col(p))
					return true;
			return false;
		default:
			return false;
	}
}

/* disabled like a node of a plan whose enable_ setting is off: it loses to every path that is not, whatever its cost */
static void
disable_col_bitmaps(List *paths)
{
	foreach_ptr(Path, p, paths)
		if (IsA(p, BitmapHeapPath) && bitmap_uses_col(((BitmapHeapPath *) p)->bitmapqual))
			sift_path_disable(p, Max(sift_disabled_nodes(p), 1) + 1);
}

static build_simple_rel_hook_type prev_simple_rel_hook;

static void
col_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	bool		indexed = false;

	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
		{
			indexed = true;
			plain_index_for_rows_only(root, ii);
		}
	if (indexed)
		col_scale_rel(rel);
}

static void
col_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (sift_enable_col_scan)
	{
		add_address_paths(root, rel);
		sift_col_add_ordered_paths(root, rel);
		return;
	}
	disable_col_bitmaps(rel->pathlist);
	disable_col_bitmaps(rel->partial_pathlist);
}

void
sift_col_scan_init(void)
{
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = col_set_rel_pathlist;
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = col_build_simple_rel;
}
#endif
