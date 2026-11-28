/*
 * agg/cost/cost.c - what the planner knows of a scan of an index of format major 2 from the statistics its parts folded (agg/cost/card.h): the rows of the relation, the groups of its keys, and the
 * cost of the aggregate scan from the granules the clauses leave and the bytes of the columns it reads (agg/cost/model.h, the five inputs and the settings of PostgreSQL itself). The open of the index is
 * not in it: every plan that reads the index pays it, so it does not tell them apart.
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "agg/cost/model.h"
#include "agg/safety.h"
#include "agg/engine.h"
#include "agg/qual/bqual.h"
#include "agg/cost/card.h"
#include "agg/expr/pg.h"
#include "agg/join/join.h"
#include "agg/sip_scan.h"
#include "agg/unnest/jsonb.h"
#include "agg/vexpr.h"
#include "core/store/ops.h"
#include "core/visibility/visibility.h"

static const SiftCardStatsView *
index_view(const IndexOptInfo *ix)
{
	if (ix == NULL || ix->hypothetical)
		return NULL;
	return sift_v6_rel_card_view(ix->indexoid, false);
}

void
sift_v6_card_apply_rel(PlannerInfo *root, RelOptInfo *rel)
{
	if (rel->reloptkind != RELOPT_BASEREL)
		return;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (ix->indpred == NIL && sift_v6_card_rel(root, rel, ix, index_view(ix)))
			return;
}

void
sift_agg_v6_rel_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (rte->inh)
		return;
	sift_v6_card_apply_rel(root, rel);
	sift_v6_range_join_rows(root, rel);
	sift_agg_v6_sip_rel_paths(root, rel, rti, rte);
}

double
sift_v6_card_group_estimate(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, List *group_exprs, double rows)
{
	const double groups = sift_v6_card_groups(root, rel, ix, index_view(ix), group_exprs, Max(rows, 1.0));

	return groups >= 1.0 ? groups : estimate_num_groups(root, group_exprs, rows, NULL, NULL);
}

#define SIFT_CORR_CLUSTERED	0.7		/* the |correlation| from which a column counts as following the order of the rows */
#define SIFT_WORD_BYTES		sizeof(int64)

/* the share of its granules a column spread by chance leaves for a qual of selectivity sel: a granule is left unless none of its rows passes (Yao) */
static double
yao_share(double sel)
{
	return sel >= 1.0 ? 1.0 : -expm1(SIFT_GRANULE_ROWS * log1p(-sel));
}

/* |correlation| of the column of a Var with the order of the rows of the table, from its statistics; 0 without them. The first key column of a sorted store is the order itself, whatever
 * the sample of the statistics says of the heap order */
static double
var_correlation(PlannerInfo *root, Node *var, AttrNumber lead_att)
{
	VariableStatData vd;
	AttStatsSlot slot;
	double		corr = 0;

	if (lead_att > 0 && IsA(var, Var) && ((Var *) var)->varattno == lead_att)
		return 1.0;

	examine_variable(root, var, 0, &vd);
	if (HeapTupleIsValid(vd.statsTuple) && get_attstatsslot(&slot, vd.statsTuple, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = slot.nnumbers > 0 ? fabs(slot.numbers[0]) : 0;
		free_attstatsslot(&slot);
	}
	ReleaseVariableStats(vd);
	return corr;
}

/* an operator that is an equality or an ordering comparison, by its estimator */
static bool
ordering_op(Oid opno)
{
	const RegProcedure rest = get_oprrest(opno);

	return rest == F_EQSEL || rest == F_SCALARLTSEL || rest == F_SCALARLESEL || rest == F_SCALARGTSEL || rest == F_SCALARGESEL;
}

/* the clause compares a column with the order of its values (a comparison, a list of constants, a test for NULL): what the zone of a granule can answer */
static bool
zone_answers(const Expr *clause)
{
	if (IsA(clause, NullTest))
		return true;
	if (IsA(clause, OpExpr))
		return ordering_op(((const OpExpr *) clause)->opno);
	return IsA(clause, ScalarArrayOpExpr) && ordering_op(((const ScalarArrayOpExpr *) clause)->opno);
}

/*
 * the share of the granules a clause leaves: a column that follows the order of the rows keeps the share of the rows, from |correlation| 0.7 up to Yao's at 0.7 and below (v6_card_cost.h); a clause
 * no zone answers leaves every granule
 */
static double
clause_share(PlannerInfo *root, RestrictInfo *ri, AttrNumber lead_att)
{
	const double sel = zone_answers(ri->clause) ? clause_selectivity(root, (Node *) ri, 0, JOIN_INNER, NULL) : 1.0;
	List	   *vars = pull_var_clause((Node *) ri->clause, PVC_RECURSE_PLACEHOLDERS);
	const double corr = list_length(vars) == 1 ? var_correlation(root, linitial(vars), lead_att) : 0;
	const double alpha = Min(Max((corr - SIFT_CORR_CLUSTERED) / (1.0 - SIFT_CORR_CLUSTERED), 0.0), 1.0);

	return alpha * sel + (1.0 - alpha) * yao_share(sel);
}

double
sift_v6_key_skew(PlannerInfo *root, RelOptInfo *rel, AttrNumber att)
{
	Var		   *var = makeVar(rel->relid, att, InvalidOid, -1, InvalidOid, 0);
	VariableStatData vd;
	AttStatsSlot slot;
	bool		isdefault;
	double		nd,
				left,
				sq = 0;

	var->vartype = get_atttype(planner_rt_fetch(rel->relid, root)->relid, att);
	examine_variable(root, (Node *) var, 0, &vd);
	nd = get_variable_numdistinct(&vd, &isdefault);
	left = HeapTupleIsValid(vd.statsTuple) ? 1.0 - ((Form_pg_statistic) GETSTRUCT(vd.statsTuple))->stanullfrac : 0;
	if (isdefault || !HeapTupleIsValid(vd.statsTuple) || !get_attstatsslot(&slot, vd.statsTuple, STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		ReleaseVariableStats(vd);
		return 1.0;
	}
	for (int i = 0; i < slot.nnumbers; i++)
	{
		sq += (double) slot.numbers[i] * slot.numbers[i];
		left -= slot.numbers[i];
	}
	if (left > 0 && nd > slot.nnumbers)
		sq += left * left / (nd - slot.nnumbers);
	free_attstatsslot(&slot);
	ReleaseVariableStats(vd);
	return Max(nd * sq, 1.0);
}

/* Does PostgreSQL know the values a filter compares (statistics of the column or of an expression index, a unique column)? Its count of the rows the filter keeps is a default guess otherwise */
static bool
filter_counted(PlannerInfo *root, const RestrictInfo *ri)
{
	List	   *args = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->args :
		IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->args : NIL;
	VariableStatData vd;
	Node	   *other;
	bool		varonleft,
				known;

	if (list_length(args) != 2 || !get_restriction_variable(root, args, 0, &vd, &other, &varonleft))
		return true;
	known = HeapTupleIsValid(vd.statsTuple) || vd.isunique;
	ReleaseVariableStats(vd);
	return known;
}

/* Are the arguments of a function all constants? */
static bool
consts_only(const List *args)
{
	bool		all = true;

	foreach_ptr(Node, a, (List *) args)
		all &= IsA(a, Const);
	return all;
}

/* Does PostgreSQL know the values every filter of a relation compares? */
static bool
filters_counted(PlannerInfo *root, const RelOptInfo *rel)
{
	bool		all = true;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		all &= filter_counted(root, ri);
	return all;
}

/* Is the count of a lookup PostgreSQL's own: a VALUES list, a function of constants (unnest of a list), a table whose filters all have statistics; a sub-select is counted through its defaults */
static bool
lookup_counted(PlannerInfo *root, const RelOptInfo *lookup)
{
	const RangeTblEntry *rte = planner_rt_fetch(lookup->relid, root);
	const RangeTblFunction *rtf = rte->rtekind == RTE_FUNCTION && list_length(rte->functions) == 1 ? linitial_node(RangeTblFunction, rte->functions) : NULL;

	if (rte->rtekind == RTE_VALUES)
		return true;
	if (rtf != NULL && IsA(rtf->funcexpr, FuncExpr))
		return consts_only(((FuncExpr *) rtf->funcexpr)->args);
	return rte->rtekind == RTE_RELATION && filters_counted(root, lookup);
}

double
sift_v6_lookup_keys_guess(PlannerInfo *root, RelOptInfo *rel, AttrNumber att, const RelOptInfo *lookup)
{
	Var		   *var;
	VariableStatData vd;
	bool		isdefault;
	double		nd;

	if (lookup_counted(root, lookup))
		return 1.0;
	var = makeVar(rel->relid, att, get_atttype(planner_rt_fetch(rel->relid, root)->relid, att), -1, InvalidOid, 0);
	examine_variable(root, (Node *) var, 0, &vd);
	nd = get_variable_numdistinct(&vd, &isdefault);
	ReleaseVariableStats(vd);
	return isdefault ? 1.0 : sqrt(Max(nd / Max(lookup->rows, 1.0), 1.0));
}

double
sift_v6_lookups_guess(PlannerInfo *root, RelOptInfo *rel, const AttrNumber *atts, RelOptInfo *const *lookups, int n, double input_rows)
{
	double		g[SIFT_STAR_MAX_DIMS],
				f = 1.0;

	Assert(n <= SIFT_STAR_MAX_DIMS);
	for (int d = 0; d < n; d++)
		g[d] = atts[d] > 0 ? sift_v6_lookup_keys_guess(root, rel, atts[d], lookups[d]) : 1.0;
	for (int d = 0; d < n; d++)
	{
		double		most = g[d];
		bool		first = true;

		for (int e = 0; e < n; e++)
			if (e != d && atts[e] == atts[d])
			{
				first &= e > d;
				most = Max(most, g[e]);
			}
		f *= first ? most : 1.0;
	}
	return Min(f, Max(rel->rows / Max(input_rows, 1.0), 1.0));
}

/* The most groups a lifted count adds to the plan's own (sift_v6_reported_groups) */
#define SIFT_V6_LIFT_GROUPS	1000

double
sift_v6_reported_groups(const V6Plan *p, List *group_exprs, double groups)
{
	if (p->lift_rows <= 0 || p->spec.nkeys == 0)
		return groups;
	return Max(groups, Min(sift_v6_card_group_estimate(p->root, p->rel, p->index, group_exprs, p->lift_rows) * Max(p->spec.nsets, 1), groups + SIFT_V6_LIFT_GROUPS));
}

double
sift_v6_clause_prune(PlannerInfo *root, const List *clauses, AttrNumber lead_att)
{
	double		share = 1.0;

	foreach_node(RestrictInfo, ri, (List *) clauses)
		share *= clause_share(root, ri, lead_att);
	return share;
}

/* the share of the sampled granules of view that have a row on a block of the TID range [lo, hi) (blocks, as the scan compares them: an index keeps the root of a HOT chain); -1: the view keeps
 * no TID envelope of its granules */
static double
tid_share(const SiftCardStatsView *view, uint64 lo, uint64 hi)
{
	uint32		hit = 0;

	if (view == NULL || view->n_sample == 0)
		return -1.0;
	for (uint32 s = 0; s < view->n_sample; s++)
	{
		const SiftSampleGranule *g = &view->sample[s];

		if (g->tid_max == 0)
			return -1.0;
		hit += hi > lo && (g->tid_max >> 16) >= (lo >> 16) && (g->tid_min >> 16) <= ((hi - 1) >> 16);
	}
	return (double) hit / view->n_sample;
}

/* the share of the virtual blocks the sift_table reloid handed out that the TID range [lo, hi) covers: the rows of the share when its rows are in the order of their TIDs (lead_att 0), scattered
 * over its granules (Yao) when a key sorts them */
static double
block_share(Oid reloid, uint64 lo, uint64 hi, AttrNumber lead_att)
{
	Relation	rel = table_open(reloid, NoLock);
	const double blocks = Max((double) sift_vis_nblocks(rel), 1.0);
	const double first = (double) (lo >> 16);
	const double last = hi <= lo ? -1.0 : Min((double) ((hi - 1) >> 16), blocks - 1.0);

	table_close(rel, NoLock);
	if (last < first)
		return 0.0;
	return lead_att == 0 ? (last - first + 1.0) / blocks : yao_share((last - first + 1.0) / blocks);
}

double
sift_v6_table_prune(PlannerInfo *root, Index relid, const List *clauses, AttrNumber lead_att)
{
	const Oid	reloid = planner_rt_fetch(relid, root)->relid;
	uint64		lo,
				hi;
	List	   *rest;
	double		share;

	if (!sift_v6_tid_bounds(relid, clauses, &lo, &hi, &rest))
		return sift_v6_clause_prune(root, clauses, lead_att);
	share = tid_share(sift_v6_rel_card_view(reloid, true), lo, hi);
	return (share >= 0 ? share : block_share(reloid, lo, hi, lead_att)) * sift_v6_clause_prune(root, rest, lead_att);
}

/* the attribute of the table the parts of the index are sorted by first (0: none) */
static AttrNumber
lead_heap_att(const IndexOptInfo *ix)
{
	const SiftStoreOps *ops = ix != NULL ? sift_store_ops_for_oid(ix->indexoid, false) : NULL;
	const int	lead = ops != NULL && ops->order != NULL ? ops->order->lead_att(ix->indexoid, false) : 0;

	return lead > 0 && lead <= ix->ncolumns ? ix->indexkeys[lead - 1] : 0;
}

/* the share of the granules of the index the quals leave: what the zones of its sampled granules say of the quals the estimator describes, the share of the rows for the others on the
 * sorted lead key lead (a parameter of a generic plan; 0: none), Yao's for the rest */
static double
f_prune(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, double granules, AttrNumber lead)
{
	List	   *rest,
			   *others;
	const double zones = sift_v6_card_prune(root, rel, ix, view, &rest);
	uint64		lo,
				hi;
	double		tids = sift_v6_tid_bounds(rel->relid, rest, &lo, &hi, &others) ? tid_share(view, lo, hi) : -1.0;
	double		share;

	if (tids >= 0)
		rest = others;
	share = (zones < 0 ? 1.0 : zones) * (tids < 0 ? 1.0 : tids) * sift_v6_clause_prune(root, rest, lead);

	return Min(Max(share, 1.0 / granules), 1.0);
}

static int
index_leaf(const IndexOptInfo *ix, int col)
{
	return col > 0 && col < SIFT_V6_ELEM_VATT ? sift_v6_card_leaf(ix, col - 1) : -1;
}

static int
heap_leaf(const IndexOptInfo *ix, int attno)
{
	const int	col = ix != NULL ? agg_index_col(ix, attno) : -1;

	return col >= 0 ? sift_v6_card_leaf(ix, col) : -1;
}

/*
 * The bytes of a decoded string of index column col (0-based): PostgreSQL's width of its column of the table rel (NULL: none). The statistics of the index size what a column takes as stored,
 * a dictionary code for a string, and a scan that groups by the strings hashes and compares their bytes.
 */
static double
string_width(const RelOptInfo *rel, const IndexOptInfo *ix, int col)
{
	const AttrNumber att = rel != NULL && ix != NULL && col >= 0 && col < ix->ncolumns ? ix->indexkeys[col] : 0;
	const Oid	type = att > 0 ? ix->opcintype[col] : InvalidOid;

	return (type == TEXTOID || type == VARCHAROID || type == BPCHAROID) && att <= rel->max_attr ? (double) rel->attr_widths[att - rel->min_attr] : 0;
}

/*
 * The decoded bytes a row of an array of index column col (0-based) costs for the dictionary entries of its granule: each array a granule has not seen is interned as an entry, its image
 * inflated whole, SIFT_IMAGE_INFLATE times the work of a byte of a column of numbers. PostgreSQL's statistics of the column of the table size the arrays and count the distinct ones; none
 * without them.
 */
static double
image_bytes(const SiftCardStatsView *view, const IndexOptInfo *ix, int col)
{
	const AttrNumber att = col >= 0 && col < ix->ncolumns ? ix->indexkeys[col] : 0;
	const double rows = Max((double) view->total_live_rows, 1.0),
				granule_rows = rows / Max((double) view->total_granules, 1.0);
	HeapTuple	tp;
	Form_pg_statistic st;
	double		bytes;

	if (att <= 0 || !type_is_array(exprType((Node *) ((TargetEntry *) list_nth(ix->indextlist, col))->expr)))
		return 0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(IndexGetRelation(ix->indexoid, false)), Int16GetDatum(att), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 0;
	st = (Form_pg_statistic) GETSTRUCT(tp);
	bytes = SIFT_IMAGE_INFLATE * st->stawidth * sift_card_ndv(st->stadistinct < 0 ? -st->stadistinct * rows : st->stadistinct, rows, granule_rows) / granule_rows;
	ReleaseSysCache(tp);
	return bytes;
}

/*
 * the bytes of a row of the columns in cols (the attributes of the index the Vars of INDEX_VAR name when index_cols, else those of the table), decoded or as stored: those of the view, or unsized
 * for a column it has no size of; decoded, a string of wide at least its width in rel (NULL: none), and an array the entries of its images
 */
static double
cols_bytes(const SiftCardStatsView *view, const Bitmapset *cols, const IndexOptInfo *ix, bool index_cols, const RelOptInfo *rel, const Bitmapset *wide, bool stored,
		   double unsized)
{
	double		bytes = 0;
	int			a = -1;

	while ((a = bms_next_member(cols, a)) >= 0)
	{
		const int	col = a + FirstLowInvalidHeapAttributeNumber;
		const int	leaf = index_cols ? index_leaf(ix, col) : heap_leaf(ix, col);
		const SiftColCardStats *c = leaf >= 0 && view != NULL && (uint32) leaf < view->ncols ? &view->cols[leaf] : NULL;
		const double b = c == NULL ? 0 : stored ? c->avg_comp_bytes_row : c->avg_dec_bytes_row;
		const int	icol = index_cols ? col - 1 : ix != NULL ? agg_index_col(ix, col) : -1;

		bytes += Max(b > 0 ? b : unsized, stored || !bms_is_member(a, wide) ? 0 : string_width(rel, ix, icol));
		bytes += stored || c == NULL ? 0 : image_bytes(view, ix, icol);
	}
	return bytes;
}

/*
 * the bytes of a row of the columns of the quals: a column the statement reads for nothing else (read) is decoded only for the share keep of the rows whose granule its zone
 * does not decide (sift_v6_card_undecided; NULL: every row)
 */
static double
filter_bytes(const SiftCardStatsView *view, const Bitmapset *filter, const Bitmapset *read, const IndexOptInfo *ix, bool index_cols, const RelOptInfo *rel, const Bitmapset *wide,
			 bool stored, double unsized, const double *keep)
{
	double		bytes = 0;
	int			a = -1;

	while ((a = bms_next_member(filter, a)) >= 0)
	{
		const int	col = a + FirstLowInvalidHeapAttributeNumber;
		const int	leaf = index_cols ? index_leaf(ix, col) : heap_leaf(ix, col);
		const double b = cols_bytes(view, bms_make_singleton(a), ix, index_cols, rel, wide, stored, unsized);

		bytes += keep != NULL && leaf >= 0 && (uint32) leaf < view->ncols && !bms_is_member(a, read) ? b * keep[leaf] : b;
	}
	return bytes;
}

/* the share of the rows of a granule that hold a value it has not met yet, for a column of ndv distinct values spread by chance */
static double
granule_new_share(double ndv)
{
	return ndv * -expm1(-SIFT_GRANULE_ROWS / Max(ndv, 1.0)) / SIFT_GRANULE_ROWS;
}

/* the distinct values of the column att of the table of rel, from the statistics of PostgreSQL: every row's own without them */
static double
heap_ndistinct(PlannerInfo *root, const RelOptInfo *rel, Oid relid, AttrNumber att, Oid type)
{
	VariableStatData vd;
	bool		isdefault;
	double		nd;

	examine_variable(root, (Node *) makeVar(rel->relid, att, type, -1, InvalidOid, 0), 0, &vd);
	nd = get_variable_numdistinct(&vd, &isdefault);
	ReleaseVariableStats(vd);
	return isdefault ? rel->tuples : nd;
}

/* the attribute of the table a scan decodes for col: of the index (index_cols: its 1-based column), or of the table, which a sift_table (ix NULL) holds whole */
static AttrNumber
scan_heap_att(const IndexOptInfo *ix, int col, bool index_cols)
{
	if (index_cols)
		return col > 0 && col <= ix->ncolumns ? ix->indexkeys[col - 1] : 0;
	return col > 0 && (ix == NULL || agg_index_col(ix, col) >= 0) ? col : 0;
}

/*
 * per row decoded, the operators of the arrays of the columns in cols (as cols_bytes): a unit hashes, interns and inflates each array it has not met before a test reads it, its width and its
 * distinct values those of the statistics of PostgreSQL
 */
static double
arrays_intern_ops(PlannerInfo *root, const RelOptInfo *rel, const IndexOptInfo *ix, const Bitmapset *cols, bool index_cols)
{
	const Oid	relid = planner_rt_fetch(rel->relid, root)->relid;
	double		ops = 0;
	int			a = -1;

	while ((a = bms_next_member(cols, a)) >= 0)
	{
		const AttrNumber att = scan_heap_att(ix, a + FirstLowInvalidHeapAttributeNumber, index_cols);
		const Oid	type = att > 0 ? get_atttype(relid, att) : InvalidOid;

		if (type_is_array(type))
			ops += granule_new_share(heap_ndistinct(root, rel, relid, att, type)) * get_attavgwidth(relid, att) / 64.0 * SIFT_ARRAY_INTERN_OPS;
	}
	return ops;
}

/* the bytes of a row of the leaves: as cols_bytes, for the columns the statistics hold by leaf */
static double
leaves_bytes(const SiftCardStatsView *view, const Bitmapset *leaves, bool stored, double unsized)
{
	double		bytes = 0;
	int			l = -1;

	while ((l = bms_next_member(leaves, l)) >= 0)
	{
		const SiftColCardStats *c = view != NULL && (uint32) l < view->ncols ? &view->cols[l] : NULL;
		const double b = c == NULL ? 0 : stored ? c->avg_comp_bytes_row : c->avg_dec_bytes_row;

		bytes += b > 0 ? b : unsized;
	}
	return bytes;
}

/* the leaf a virtual attribute reads: of a key of a document, or the values of the map of a member */
static void
add_vatt_leaf(const SiftV6Spec *spec, Bitmapset **leaves, int attno)
{
	*leaves = bms_add_member(*leaves, sift_v6_spec_vatt_col(spec, attno, FLOAT8OID, 0).jleaf - 1);
}

/* the columns of the index the clauses (filter: of the scan keys over the index, of the batch quals over the table) or the keys and arguments read, and the leaves of the keys of documents they read instead of the document: a scan decodes those alone */
static Bitmapset *
spec_cols(const SiftV6Spec *spec, const IndexOptInfo *ix, bool filter, Bitmapset **leaves)
{
	Bitmapset  *cols = NULL;

	if (filter)
	{
		Bitmapset  *virt = NULL;
		int			a = -1;

		pull_varattnos((Node *) spec->indexqual, INDEX_VAR, &cols);
		pull_varattnos((Node *) spec->filter, INDEX_VAR, &cols);
		pull_varattnos((Node *) spec->vqual, SIFT_V6_BQ_RELID, &virt);
		while ((a = bms_next_member(cols, a)) >= 0)
			if (sift_v6_vatt_is_json(a + FirstLowInvalidHeapAttributeNumber))
			{
				add_vatt_leaf(spec, leaves, a + FirstLowInvalidHeapAttributeNumber);
				cols = bms_del_member(cols, a);
			}
		for (a = -1; (a = bms_next_member(virt, a)) >= 0;)
		{
			const int	att = a + FirstLowInvalidHeapAttributeNumber,
						col = att > 0 ? agg_index_col(ix, att) : -1;

			if (sift_v6_vatt_is_json(att))
				add_vatt_leaf(spec, leaves, att);
			else if (col >= 0)
				cols = bms_add_member(cols, col + 1 - FirstLowInvalidHeapAttributeNumber);
		}
		return cols;
	}
	for (int i = 0; i < spec->nkeys + spec->naggs; i++)
	{
		const SiftV6Col c = i < spec->nkeys ? spec->key[i] : spec->agg[i - spec->nkeys].arg;

		if (c.jleaf > 0)
			*leaves = bms_add_member(*leaves, c.jleaf - 1);
		else if (c.attno > 0)
			cols = bms_add_member(cols, c.attno - FirstLowInvalidHeapAttributeNumber);
	}
	return cols;
}

/* the columns of the index the scan groups by: their strings are hashed and compared, at least their width a row (the arguments of the aggregates and the quals cost the decode the statistics say) */
static Bitmapset *
spec_keys(const SiftV6Spec *spec)
{
	Bitmapset  *keys = NULL;

	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].jleaf == 0 && spec->key[i].attno > 0)
			keys = bms_add_member(keys, spec->key[i].attno - FirstLowInvalidHeapAttributeNumber);
	return keys;
}

/* what a scan pays once before its rows: the open of the source and of each part it reads (agg/cost/model.h) */
static double
open_cost(double parts)
{
	return (SIFT_OPEN_OPS + SIFT_OPEN_PART_OPS * parts) * cpu_operator_cost;
}

typedef struct ScanBytes
{
	const SiftCardStatsView *view;
	double		table_rows;
	double		granules;
	double		pages;			/* of the index, or of the table a sift_table scan reads */
	double		f_prune;
	double		surv;
	double		filter_row;		/* bytes of a row of the columns of the quals, decoded on the rows of the granules the zones leave */
	double		measure_row;	/* ... and of the others, on the rows that pass the quals */
	double		io_col_row;		/* the bytes of a column of a row in the parts of the index, for a column the statistics do not size: the columns share the parts alike */
	double		filter_io_row;	/* the bytes of those columns as they are stored, which a page read costs */
	double		measure_io_row;
} ScanBytes;

static void
scan_bytes(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, AttrNumber lead, ScanBytes *b)
{
	b->view = index_view(ix);
	b->table_rows = b->view != NULL ? (double) Max(b->view->total_live_rows, 1) : Max(rel->tuples, 1.0);
	b->granules = b->view != NULL ? (double) Max(b->view->total_granules, 1) : Max(1.0, ceil(b->table_rows / SIFT_GRANULE_ROWS));
	b->io_col_row = b->view != NULL ? Min((double) b->view->total_bytes / (Max(ix->ncolumns, 1) * b->table_rows), SIFT_WORD_BYTES) : SIFT_WORD_BYTES;
	b->pages = Max((double) (ix != NULL ? ix->pages : rel->pages), 1.0);
	b->surv = Min(Max(rel->rows, 1.0), b->table_rows);
	b->f_prune = f_prune(root, rel, ix, b->view, b->granules, lead);
}

static SiftCostInputs
scan_inputs(const ScanBytes *b)
{
	const double decoded = Min(b->table_rows, Max(b->f_prune * b->table_rows, b->surv));

	return (SiftCostInputs) {
		.g_eff = decoded / SIFT_GRANULE_ROWS,
		.b_dec = decoded * b->filter_row + b->surv * b->measure_row,
		.b_io = decoded * b->filter_io_row + b->surv * b->measure_io_row,
		.n_groups = 1.0,
		.f_join = 1.0,
		.r_bw_cpu = SIFT_COST_R_BW_CPU,
		.f_cold = sift_agg_cold_share(b->pages),
	};
}

static double
scan_parts(const ScanBytes *b)
{
	return b->view != NULL ? Max(1.0, (double) b->view->nfiles) : 1.0;
}

Cost
sift_v6_fetch_cost(bool is_table)
{
	return is_table ? SIFT_TABLE_FETCH_OPS * cpu_operator_cost : cpu_tuple_cost;
}

Cost
sift_v6_rel_read_cost(const RelOptInfo *rel)
{
	Cost		spc_seq;

	get_tablespace_page_costs(rel->reltablespace, NULL, &spc_seq);
	return sift_agg_cold_share(rel->pages) * rel->pages * spc_seq + rel->baserestrictcost.startup + rel->tuples * (cpu_tuple_cost + rel->baserestrictcost.per_tuple);
}

/* a probe of an index of a relation the cache holds, for each loop of a nested loop: the entries and rows it finds and the operators of a descent, and its page reads for the share the cache does not hold */
static Cost
probe_cost(const Path *probe)
{
	const RelOptInfo *rel = probe->parent;
	const Cost	cpu = Max(probe->rows, 1.0) * (cpu_index_tuple_cost + cpu_tuple_cost + rel->baserestrictcost.per_tuple) + ceil(log2(Max(rel->tuples, 2.0))) * cpu_operator_cost;

	return Min(cpu + sift_agg_cold_share(rel->pages) * probe->total_cost, probe->total_cost);
}

/* the inner side of a nested loop that probes an index for each row of its outer side */
static bool
is_probe(const Path *p)
{
	return p->param_info != NULL && (IsA(p, IndexPath) || IsA(p, BitmapHeapPath));
}

Cost
sift_v6_dim_cost(const Path *child)
{
	const RelOptInfo *rel = child->parent;
	Cost		spc_seq;

	if (IsA(child, ProjectionPath))
		return sift_v6_dim_cost(((const ProjectionPath *) child)->subpath) + child->total_cost - ((const ProjectionPath *) child)->subpath->total_cost;
	if (IsA(child, NestPath) && is_probe(((const JoinPath *) child)->innerjoinpath))
	{
		const JoinPath *jp = (const JoinPath *) child;

		return sift_v6_dim_cost(jp->outerjoinpath) + jp->outerjoinpath->rows * probe_cost(jp->innerjoinpath) + child->rows * cpu_tuple_cost;
	}

	if (child->pathtype != T_SeqScan)
		return child->total_cost;
	get_tablespace_page_costs(rel->reltablespace, NULL, &spc_seq);
	return child->total_cost - (1.0 - sift_agg_cold_share(rel->pages)) * rel->pages * spc_seq;
}

/* per row decoded, what the quals of the scan that PostgreSQL's executor evaluates cost it */
static Cost
pg_qual_cost(PlannerInfo *root, const IndexOptInfo *ix, const List *vqual)
{
	SiftV6Input in = {.indexoid = ix->indexoid};
	Cost		per_row = 0;

	sift_v6_input_init_info(&in, ix);
	foreach_ptr(Expr, clause, (List *) vqual)
		if (sift_v6_bq_runs_pg(&in, clause))
		{
			QualCost	qc;

			cost_qual_eval_node(&qc, (Node *) clause, root);
			per_row += qc.per_tuple;
		}
	return per_row;
}

/*
 * The operators of the test of a row against the constants of an IN list on an integer column: a bit of a bitmap where the values of a chunk span fewer than SIFT_IN_BITMAP_SPAN, else a
 * binary search of the constants (col/granule/cfilt/kern.c). The span of a chunk is that of the column, narrowed by how little its zones overlap.
 */
static double
in_test_ops(const SiftCardStatsView *view, const IndexOptInfo *ix, bool index_cols, const ScalarArrayOpExpr *sa)
{
	const Node *arg = linitial(sa->args);
	const int	att = IsA(arg, Var) ? ((const Var *) arg)->varattno : 0;
	const int	leaf = index_cols ? index_leaf(ix, att) : heap_leaf(ix, att);
	const SiftColCardStats *c = leaf >= 0 && view != NULL && (uint32) leaf < view->ncols ? &view->cols[leaf] : NULL;
	const Oid	type = exprType(arg);
	const double n = sift_v6_const_array_len(lsecond(sa->args));

	if (n <= 1 || !sa->useOr || type == NUMERICOID || (sift_v6_class(type) != SV6C_I32 && sift_v6_class(type) != SV6C_I64))
		return 0;
	if (c != NULL && ((double) c->max_i64 - (double) c->min_i64) * Max(c->zone_overlap_ratio, 1.0 / Max(view->total_granules, 1)) < SIFT_IN_BITMAP_SPAN)
		return 1.0;
	return SIFT_IN_LEVEL_OPS * log2(n + 1);
}

/* per row decoded, the search of the constants of the IN lists among the clauses (RestrictInfos, or bare clauses on the index columns when index_cols) */
static Cost
in_lists_cost(const SiftCardStatsView *view, const IndexOptInfo *ix, bool index_cols, const List *clauses)
{
	double		ops = 0;

	foreach_ptr(Node, n, clauses)
	{
		const Node *clause = IsA(n, RestrictInfo) ? (const Node *) ((const RestrictInfo *) n)->clause : n;

		if (IsA(clause, ScalarArrayOpExpr))
			ops += in_test_ops(view, ix, index_cols, (const ScalarArrayOpExpr *) clause);
	}
	return ops * cpu_operator_cost;
}

Cost
sift_v6_in_lists_cost(const IndexOptInfo *ix, const List *clauses)
{
	return in_lists_cost(index_view(ix), ix, false, clauses);
}

/*
 * per row that passes the quals, what the derivations PostgreSQL evaluates cost (agg/expr/pg.h): the operators of the expression, as PostgreSQL prices it in its own plan, and one for each input the
 * row puts in the slot of the expression; the columns of the index they read are added to cols
 */
static Cost
pg_derive_cost(PlannerInfo *root, const SiftV6Spec *spec, Bitmapset **cols)
{
	Cost		per_row = 0;

	for (int i = 0; i < spec->nderive; i++)
		if (spec->derive[i].kind == SVD_PG_EXPR)
		{
			SiftV6PgItem it;
			QualCost	qc;

			sift_v6_pgexpr_item(spec, &spec->derive[i], &it);
			cost_qual_eval_node(&qc, (Node *) it.expr, root);
			per_row += qc.per_tuple + it.nin * cpu_operator_cost;
			for (int k = 0; k < it.nin; k++)
				if (it.col[k].src == SV6SRC_ATTR && it.col[k].attno > 0)
					*cols = bms_add_member(*cols, it.col[k].attno - FirstLowInvalidHeapAttributeNumber);
		}
	return per_row;
}

SiftV6AggPrice
sift_v6_agg_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups)
{
	ScanBytes	b;
	SiftV6AggPrice price;
	Bitmapset  *filter_leaves = NULL,
			   *measure_leaves = NULL,
			   *filter = spec_cols(spec, ix, true, &filter_leaves),
			   *read = spec_cols(spec, ix, false, &measure_leaves),
			   *measure,
			   *keys = spec_keys(spec);
	const Cost	derive_row = pg_derive_cost(root, spec, &read);
	const double *keep;

	measure = bms_difference(read, filter);
	scan_bytes(root, rel, ix, lead_heap_att(ix), &b);
	keep = sift_v6_card_undecided(root, rel, ix, b.view);
	b.surv = Min(Max(clauselist_selectivity(root, rel->baserestrictinfo, rel->relid, JOIN_INNER, NULL) * b.table_rows, 1.0), b.table_rows);
	b.filter_row = filter_bytes(b.view, filter, read, ix, true, rel, keys, false, SIFT_WORD_BYTES, keep) + leaves_bytes(b.view, filter_leaves, false, SIFT_WORD_BYTES);
	b.measure_row = cols_bytes(b.view, measure, ix, true, rel, keys, false, SIFT_WORD_BYTES) + leaves_bytes(b.view, measure_leaves, false, SIFT_WORD_BYTES);
	b.filter_io_row = filter_bytes(b.view, filter, read, ix, true, NULL, NULL, true, b.io_col_row, keep) + leaves_bytes(b.view, filter_leaves, true, b.io_col_row);
	b.measure_io_row = cols_bytes(b.view, measure, ix, true, NULL, NULL, true, b.io_col_row) + leaves_bytes(b.view, measure_leaves, true, b.io_col_row);
	price.in = scan_inputs(&b);
	price.in.n_groups = Max(groups, 1.0);
	price.in.qual_row = pg_qual_cost(root, ix, spec->vqual) + in_lists_cost(b.view, ix, true, spec->indexqual) +
		arrays_intern_ops(root, rel, ix, bms_union(filter, measure), true) * cpu_operator_cost;
	price.selectivity = Min(b.surv / (price.in.g_eff * SIFT_GRANULE_ROWS), 1.0);
	price.in.qual_row += derive_row * price.selectivity;
	price.open = open_cost(scan_parts(&b));
	price.granule_rows = b.table_rows / b.granules;
	return price;
}

void
sift_v6_agg_cost(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total)
{
	const SiftV6AggPrice price = sift_v6_agg_price(root, rel, ix, spec, groups);

	sift_agg_choose_workers(&price.in, price.selectivity, false, false, price.open, 0, startup, total);
}

void
sift_v6_scan_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, AttrNumber key, Cost *open, Cost *total)
{
	ScanBytes	b;
	SiftCostInputs in;
	Cost		startup;
	Bitmapset  *filter = key > 0 ? bms_make_singleton(key - FirstLowInvalidHeapAttributeNumber) : NULL,
			   *measure = NULL,
			   *read;
	const double *keep;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, rel->relid, &filter);
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &measure);
	read = key > 0 ? bms_add_member(bms_copy(measure), key - FirstLowInvalidHeapAttributeNumber) : measure;
	scan_bytes(root, rel, ix, 0, &b);
	keep = sift_v6_card_undecided(root, rel, ix, b.view);
	b.filter_row = filter_bytes(b.view, filter, read, ix, false, rel, filter, false, SIFT_WORD_BYTES, keep);
	b.measure_row = cols_bytes(b.view, measure, ix, false, rel, measure, false, SIFT_WORD_BYTES);
	b.filter_io_row = filter_bytes(b.view, filter, read, ix, false, NULL, NULL, true, b.io_col_row, keep);
	b.measure_io_row = cols_bytes(b.view, measure, ix, false, NULL, NULL, true, b.io_col_row);
	in = scan_inputs(&b);
	in.qual_row = in_lists_cost(b.view, ix, false, rel->baserestrictinfo) + arrays_intern_ops(root, rel, ix, bms_union(filter, measure), false) * cpu_operator_cost;
	*open = open_cost(scan_parts(&b));
	sift_agg_choose_workers(&in, Min(b.surv / (in.g_eff * SIFT_GRANULE_ROWS), 1.0), false, false, *open, 0, &startup, total);
}
