/*
 * agg/cost/cost.c - what the planner knows of a scan of an index of format major 2 from the statistics its parts folded (agg/cost/card.h): the rows of the relation, the groups of its keys, and the
 * cost of the aggregate scan from the granules the clauses leave and the bytes of the columns it reads (agg/cost/model.h, the five inputs and the settings of PostgreSQL itself). The open of the index is
 * not in it: every plan that reads the index pays it, so it does not tell them apart.
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_statistic.h"
#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "agg/cost/model.h"
#include "agg/safety.h"
#include "agg/engine.h"
#include "agg/qual/bqual.h"
#include "agg/cost/card.h"
#include "agg/join/join.h"
#include "agg/sip_scan.h"
#include "agg/unnest/jsonb.h"
#include "agg/vexpr.h"

static const SiftCardStatsView *
index_view(const IndexOptInfo *ix)
{
	if (ix == NULL || ix->hypothetical)
		return NULL;
	return sift_v6_rel_card_view(ix->indexoid);
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

/* the share of the granules a clause leaves: a column that follows the order of the rows keeps the share of the rows, from |correlation| 0.7 up to Yao's at 0.7 and below (v6_card_cost.h) */
static double
clause_share(PlannerInfo *root, RestrictInfo *ri, AttrNumber lead_att)
{
	const double sel = clause_selectivity(root, (Node *) ri, 0, JOIN_INNER, NULL);
	List	   *vars = pull_var_clause((Node *) ri->clause, 0);
	const double corr = list_length(vars) == 1 ? var_correlation(root, linitial(vars), lead_att) : 0;
	const double alpha = Min(Max((corr - SIFT_CORR_CLUSTERED) / (1.0 - SIFT_CORR_CLUSTERED), 0.0), 1.0);

	return alpha * sel + (1.0 - alpha) * yao_share(sel);
}

double
sift_v6_clause_prune(PlannerInfo *root, const List *clauses, AttrNumber lead_att)
{
	double		share = 1.0;

	foreach_node(RestrictInfo, ri, (List *) clauses)
		share *= clause_share(root, ri, lead_att);
	return share;
}

/* the share of the granules of the index the quals leave: what the zones of its sampled granules say of the quals the estimator describes, Yao's for the others */
static double
f_prune(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, double granules)
{
	List	   *rest;
	const double zones = sift_v6_card_prune(root, rel, ix, view, &rest);
	const double share = (zones < 0 ? 1.0 : zones) * sift_v6_clause_prune(root, rest, 0);

	return Min(Max(share, 1.0 / granules), 1.0);
}

/* the bytes of a row of the columns in cols (the attributes of the index the Vars of INDEX_VAR name, or those of the relation when rel is given), decoded or as stored: those of the view, or unsized for a column it has no size of */
static double
cols_bytes(const SiftCardStatsView *view, const Bitmapset *cols, int leaf_of(const void *, int), const void *ctx, bool stored, double unsized)
{
	double		bytes = 0;
	int			a = -1;

	while ((a = bms_next_member(cols, a)) >= 0)
	{
		const int	leaf = leaf_of(ctx, a + FirstLowInvalidHeapAttributeNumber);
		const SiftColCardStats *c = leaf >= 0 && view != NULL && (uint32) leaf < view->ncols ? &view->cols[leaf] : NULL;
		const double b = c == NULL ? 0 : stored ? c->avg_comp_bytes_row : c->avg_dec_bytes_row;

		bytes += b > 0 ? b : unsized;
	}
	return bytes;
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

static int
index_leaf(const void *ix, int col)
{
	return col > 0 && col < SIFT_V6_ELEM_VATT ? sift_v6_card_leaf((const IndexOptInfo *) ix, col - 1) : -1;
}

static int
heap_leaf(const void *ix, int attno)
{
	const int	col = ix != NULL ? agg_index_col((const IndexOptInfo *) ix, attno) : -1;

	return col >= 0 ? sift_v6_card_leaf((const IndexOptInfo *) ix, col) : -1;
}

/* the columns of the index the clauses (filter) or the keys and arguments read, and the leaves of the keys of documents they read instead of the document: a scan decodes those alone */
static Bitmapset *
spec_cols(const SiftV6Spec *spec, bool filter, Bitmapset **leaves)
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
				*leaves = bms_add_member(*leaves, sift_v6_vatt_leaf(a + FirstLowInvalidHeapAttributeNumber));
				cols = bms_del_member(cols, a);
			}
		for (a = -1; (a = bms_next_member(virt, a)) >= 0;)
			if (sift_v6_vatt_is_json(a + FirstLowInvalidHeapAttributeNumber))
				*leaves = bms_add_member(*leaves, sift_v6_vatt_leaf(a + FirstLowInvalidHeapAttributeNumber));
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
scan_bytes(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, ScanBytes *b)
{
	b->view = index_view(ix);
	b->table_rows = b->view != NULL ? (double) Max(b->view->total_live_rows, 1) : Max(rel->tuples, 1.0);
	b->granules = b->view != NULL ? (double) Max(b->view->total_granules, 1) : Max(1.0, ceil(b->table_rows / SIFT_GRANULE_ROWS));
	b->io_col_row = b->view != NULL ? Min((double) b->view->total_bytes / (Max(ix->ncolumns, 1) * b->table_rows), SIFT_WORD_BYTES) : SIFT_WORD_BYTES;
	b->pages = Max((double) (ix != NULL ? ix->pages : rel->pages), 1.0);
	b->surv = Min(Max(rel->rows, 1.0), b->table_rows);
	b->f_prune = f_prune(root, rel, ix, b->view, b->granules);
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

Cost
sift_v6_dim_cost(const Path *child)
{
	const RelOptInfo *rel = child->parent;
	Cost		spc_seq;

	if (IsA(child, ProjectionPath))
		return sift_v6_dim_cost(((const ProjectionPath *) child)->subpath) + child->total_cost - ((const ProjectionPath *) child)->subpath->total_cost;

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

SiftV6AggPrice
sift_v6_agg_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups)
{
	ScanBytes	b;
	SiftV6AggPrice price;
	Bitmapset  *filter_leaves = NULL,
			   *measure_leaves = NULL,
			   *filter = spec_cols(spec, true, &filter_leaves),
			   *measure = bms_difference(spec_cols(spec, false, &measure_leaves), filter);

	scan_bytes(root, rel, ix, &b);
	b.surv = Min(Max(clauselist_selectivity(root, rel->baserestrictinfo, rel->relid, JOIN_INNER, NULL) * b.table_rows, 1.0), b.table_rows);
	b.filter_row = cols_bytes(b.view, filter, index_leaf, ix, false, SIFT_WORD_BYTES) + leaves_bytes(b.view, filter_leaves, false, SIFT_WORD_BYTES);
	b.measure_row = cols_bytes(b.view, measure, index_leaf, ix, false, SIFT_WORD_BYTES) + leaves_bytes(b.view, measure_leaves, false, SIFT_WORD_BYTES);
	b.filter_io_row = cols_bytes(b.view, filter, index_leaf, ix, true, b.io_col_row) + leaves_bytes(b.view, filter_leaves, true, b.io_col_row);
	b.measure_io_row = cols_bytes(b.view, measure, index_leaf, ix, true, b.io_col_row) + leaves_bytes(b.view, measure_leaves, true, b.io_col_row);
	price.in = scan_inputs(&b);
	price.in.n_groups = Max(groups, 1.0);
	price.in.pg_qual = pg_qual_cost(root, ix, spec->vqual);
	price.selectivity = Min(b.surv / (price.in.g_eff * SIFT_GRANULE_ROWS), 1.0);
	price.open = open_cost(scan_parts(&b));
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
			   *measure = NULL;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, rel->relid, &filter);
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &measure);
	scan_bytes(root, rel, ix, &b);
	b.filter_row = cols_bytes(b.view, filter, heap_leaf, ix, false, SIFT_WORD_BYTES);
	b.measure_row = cols_bytes(b.view, measure, heap_leaf, ix, false, SIFT_WORD_BYTES);
	b.filter_io_row = cols_bytes(b.view, filter, heap_leaf, ix, true, b.io_col_row);
	b.measure_io_row = cols_bytes(b.view, measure, heap_leaf, ix, true, b.io_col_row);
	in = scan_inputs(&b);
	*open = open_cost(scan_parts(&b));
	sift_agg_choose_workers(&in, Min(b.surv / (in.g_eff * SIFT_GRANULE_ROWS), 1.0), false, false, *open, 0, &startup, total);
}
