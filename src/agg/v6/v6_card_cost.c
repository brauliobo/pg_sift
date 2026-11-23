/*
 * agg/v6/v6_cost.c - what the planner knows of a scan of an index of format major 2 from the statistics its parts folded (agg/v6/v6_card.h): the rows of the relation, the groups of its keys, and the
 * cost of the aggregate scan from the granules the clauses leave and the bytes of the columns it reads (agg/cost.h, the five inputs and the settings of PostgreSQL itself). The open of the index is
 * not in it: every plan that reads the index pays it, so it does not tell them apart.
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_statistic.h"
#include "nodes/bitmapset.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "agg/cost.h"
#include "agg/plan/safety.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_join.h"
#include "agg/v6/v6_sip.h"
#include "agg/v6/v6_vexpr.h"
#include "col/f2/f2_cost_model.h"

static const SiftCardStatsView *
index_view(const IndexOptInfo *ix)
{
	const SiftV6Format *f = sift_v6_format();

	if (ix == NULL || f->card_view == NULL || ix->hypothetical || !f->owns_rel(ix->indexoid, false))
		return NULL;
	return f->card_view(ix->indexoid);
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

/* |correlation| of the column of a Var with the order of the rows of the table, from its statistics; 0 without them */
static double
var_correlation(PlannerInfo *root, Node *var)
{
	VariableStatData vd;
	AttStatsSlot slot;
	double		corr = 0;

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
clause_share(PlannerInfo *root, RestrictInfo *ri)
{
	const double sel = clause_selectivity(root, (Node *) ri, 0, JOIN_INNER, NULL);
	List	   *vars = pull_var_clause((Node *) ri->clause, 0);
	const double corr = list_length(vars) == 1 ? var_correlation(root, linitial(vars)) : 0;
	const double alpha = Min(Max((corr - SIFT_CORR_CLUSTERED) / (1.0 - SIFT_CORR_CLUSTERED), 0.0), 1.0);

	return alpha * sel + (1.0 - alpha) * yao_share(sel);
}

double
sift_v6_clause_prune(PlannerInfo *root, const List *clauses)
{
	double		share = 1.0;

	foreach_node(RestrictInfo, ri, (List *) clauses)
		share *= clause_share(root, ri);
	return share;
}

/* the share of the granules of the index the quals leave: what the zones of its sampled granules say of the quals the estimator describes, Yao's for the others */
static double
f_prune(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, double granules)
{
	List	   *rest;
	double		zones = sift_v6_card_prune(root, rel, ix, view, &rest);

	return Min(Max((zones < 0 ? 1.0 : zones) * sift_v6_clause_prune(root, rest), 1.0 / granules), 1.0);
}

/* the bytes of a row of the columns in cols (the attributes of the index the Vars of INDEX_VAR name, or those of the relation when rel is given): those of the view, or a word for a column it has no size of */
static double
cols_bytes(const SiftCardStatsView *view, const Bitmapset *cols, int leaf_of(const void *, int), const void *ctx)
{
	double		bytes = 0;
	int			a = -1;

	while ((a = bms_next_member(cols, a)) >= 0)
	{
		const int	leaf = leaf_of(ctx, a + FirstLowInvalidHeapAttributeNumber);
		const double b = leaf >= 0 && view != NULL && (uint32) leaf < view->ncols ? view->cols[leaf].avg_dec_bytes_row : 0;

		bytes += b > 0 ? b : SIFT_WORD_BYTES;
	}
	return bytes;
}

static int
index_leaf(const void *ix, int col)
{
	return col > 0 ? sift_v6_card_leaf((const IndexOptInfo *) ix, col - 1) : -1;
}

static int
heap_leaf(const void *ix, int attno)
{
	const int	col = ix != NULL ? agg_index_col((const IndexOptInfo *) ix, attno) : -1;

	return col >= 0 ? sift_v6_card_leaf((const IndexOptInfo *) ix, col) : -1;
}

static Bitmapset *
spec_cols(const SiftV6Spec *spec, bool filter)
{
	Bitmapset  *cols = NULL;

	if (filter)
	{
		pull_varattnos((Node *) spec->indexqual, INDEX_VAR, &cols);
		pull_varattnos((Node *) spec->filter, INDEX_VAR, &cols);
		return cols;
	}
	for (int i = 0; i < spec->nkeys + spec->naggs; i++)
	{
		const SiftV6Col c = i < spec->nkeys ? spec->key[i] : spec->agg[i - spec->nkeys].arg;

		if (c.attno > 0)
			cols = bms_add_member(cols, c.attno - FirstLowInvalidHeapAttributeNumber);
	}
	return cols;
}

/* what a scan pays once before its rows: the open of the source and of each part it reads (agg/cost.h) */
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
	double		f_prune;
	double		surv;
	double		filter_row;		/* bytes of a row of the columns of the quals, decoded on the rows of the granules the zones leave */
	double		measure_row;	/* ... and of the others, on the rows that pass the quals */
} ScanBytes;

static void
scan_bytes(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, ScanBytes *b)
{
	b->view = index_view(ix);
	b->table_rows = b->view != NULL ? (double) Max(b->view->total_live_rows, 1) : Max(rel->tuples, 1.0);
	b->granules = b->view != NULL ? (double) Max(b->view->total_granules, 1) : Max(1.0, ceil(b->table_rows / SIFT_GRANULE_ROWS));
	b->surv = Min(Max(rel->rows, 1.0), b->table_rows);
	b->f_prune = f_prune(root, rel, ix, b->view, b->granules);
}

static SiftCostInputs
scan_inputs(const ScanBytes *b)
{
	const double decoded = Min(b->table_rows, Max(b->f_prune * b->granules * SIFT_GRANULE_ROWS, b->surv));

	return (SiftCostInputs) {
		.g_eff = decoded / SIFT_GRANULE_ROWS,
		.b_dec = decoded * b->filter_row + b->surv * b->measure_row,
		.n_groups = 1.0,
		.f_join = 1.0,
		.r_bw_cpu = SIFT_COST_R_BW_CPU,
		.f_cold = 1.0,
	};
}

static double
scan_parts(const ScanBytes *b)
{
	return b->view != NULL ? Max(1.0, (double) b->view->nfiles) : 1.0;
}

SiftV6AggPrice
sift_v6_agg_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups)
{
	ScanBytes	b;
	SiftV6AggPrice price;
	Bitmapset  *filter = spec_cols(spec, true),
			   *measure = spec_cols(spec, false);

	scan_bytes(root, rel, ix, &b);
	b.filter_row = cols_bytes(b.view, filter, index_leaf, ix);
	b.measure_row = cols_bytes(b.view, measure, index_leaf, ix);
	price.in = scan_inputs(&b);
	price.in.n_groups = Max(groups, 1.0);
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
	b.filter_row = cols_bytes(b.view, filter, heap_leaf, ix);
	b.measure_row = cols_bytes(b.view, measure, heap_leaf, ix);
	in = scan_inputs(&b);
	*open = open_cost(scan_parts(&b));
	sift_agg_choose_workers(&in, Min(b.surv / (in.g_eff * SIFT_GRANULE_ROWS), 1.0), false, false, *open, 0, &startup, total);
}
