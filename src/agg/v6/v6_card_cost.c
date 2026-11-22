/*
 * agg/v6/v6_cost.c - what the planner knows of a scan of an index of format major 2 from the statistics its parts folded (agg/v6/v6_card.h): the rows of the relation, the groups of its keys, and the
 * cost of the aggregate scan from the granules the clauses leave and the bytes of the columns it reads (agg/cost.h, the five inputs and the settings of PostgreSQL itself). The open of the index is
 * not in it: every plan that reads the index pays it, so it does not tell them apart.
 */
#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"

#include "agg/cost.h"
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
		if (ix->indpred == NIL && ix->indexprs == NIL && sift_v6_card_rel(root, rel, ix, index_view(ix)))
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

/* the bytes a row of the columns the scan reads decodes: those of the view, or a word for a column it has no size of */
static double
row_bytes(const SiftCardStatsView *view, const SiftV6Spec *spec)
{
	double		bytes = 0;

	for (int i = 0; i < spec->nkeys + spec->naggs; i++)
	{
		const int	att = i < spec->nkeys ? spec->key[i].attno : spec->agg[i - spec->nkeys].arg.attno;
		const double b = att > 0 && (uint32) att <= view->ncols ? view->cols[att - 1].avg_dec_bytes_row : 0;

		bytes += b > 0 ? b : sizeof(int64);
	}
	return bytes;
}

void
sift_v6_agg_cost(const RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total)
{
	const SiftCardStatsView *view = index_view(ix);
	const double table_rows = view != NULL ? (double) Max(view->total_live_rows, 1) : Max(rel->tuples, 1.0);
	const double granules = view != NULL ? (double) Max(view->total_granules, 1) : Max(1.0, ceil(table_rows / SIFT_GRANULE_ROWS));
	const double surv = Min(Max(rel->rows, 1.0), table_rows);
	const double f_prune = sift_f_prune(surv, table_rows, 1.0, granules);
	const double decoded = Min(table_rows, Max(f_prune * granules * SIFT_GRANULE_ROWS, surv));
	const SiftCostInputs in = {
		.g_eff = f_prune * granules,
		.b_dec = decoded * (view != NULL ? row_bytes(view, spec) : (spec->nkeys + spec->naggs) * sizeof(int64)),
		.n_groups = Max(groups, 1.0),
		.f_join = 1.0,
		.r_bw_cpu = SIFT_COST_R_BW_CPU,
		.f_cold = 1.0,
	};

	sift_agg_choose_workers(&in, Min(surv / decoded, 1.0), false, false, 0.0, 0, startup, total);
}
