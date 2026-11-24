/*
 * agg/v6/v6_cost.c - the price of a SiftAggV6 scan (v6_cost.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/sysattr.h"
#include "nodes/bitmapset.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "agg/v6/v6_cost_model.h"
#include "agg/v6/v6_cost.h"

static void
add_col(Bitmapset **cols, SiftV6Col c)
{
	if (c.attno > 0)
		*cols = bms_add_member(*cols, c.attno - FirstLowInvalidHeapAttributeNumber);
}

int
sift_v6_spec_ncols_read(const SiftV6Spec *spec)
{
	Bitmapset  *cols = NULL;
	int			n;

	for (int i = 0; i < spec->nkeys; i++)
		add_col(&cols, spec->key[i]);
	for (int j = 0; j < spec->naggs; j++)
		if (sift_v6_kind_reads_column(spec->agg[j].kind))
		{
			add_col(&cols, spec->agg[j].arg);
			add_col(&cols, spec->agg[j].arg2);
			for (int c = 0; c < spec->agg[j].nxcols; c++)
				add_col(&cols, spec->agg[j].xcol[c]);
		}
	pull_varattnos((Node *) spec->indexqual, INDEX_VAR, &cols);
	pull_varattnos((Node *) spec->filter, INDEX_VAR, &cols);
	n = bms_num_members(cols);
	bms_free(cols);
	return n;
}

void
sift_v6_scan_cost(const SiftV6CostIn *in, Cost *startup, Cost *total)
{
	const double granules = Max(1.0, ceil(in->tuples / SIFT_GRANULE_ROWS));
	const double decoded = Min(Max(in->tuples, 1.0), Max(in->f_prune * granules, 1.0) * SIFT_GRANULE_ROWS);
	const SiftCostInputs costs = {
		.g_eff = decoded / SIFT_GRANULE_ROWS,
		.b_dec = decoded * Max(in->ncols_read, 1) * sizeof(int64),
		.b_io = decoded * Max(in->ncols_read, 1) * sizeof(int64),
		.n_groups = Max(in->groups, 1.0),
		.f_join = 1.0,
		.r_bw_cpu = SIFT_COST_R_BW_CPU,
		.f_cold = sift_agg_cold_share(in->pages),
	};

	sift_agg_estimate_cost(&costs, Min(Max(in->rows, 1.0) / decoded, 1.0), false, false, SIFT_OPEN_OPS * cpu_operator_cost, 0, startup, total);
}
