/*
 * agg/v6/v6_cost.c - the price of a SiftAggV6 scan (v6_cost.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/sysattr.h"
#include "nodes/bitmapset.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "agg/cost.h"
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

double
sift_v6_scan_cost(const SiftV6CostIn *in)
{
	const double granules = ceil(in->tuples / SIFT_GRANULE_ROWS);
	const double io_granules = Max(1.0, granules * in->sel);
	const double col_frac = (double) Max(1, in->ncols_read) / Max(1, in->ncols_host);
	const double n_surv = in->tuples * in->sel;
	const double n_vet = in->table ? 0.0 : n_surv * (1.0 - in->allvisfrac);

	return io_granules * col_frac * seq_page_cost + n_surv * 0.05 * cpu_tuple_cost + n_vet * cpu_tuple_cost + in->groups * cpu_tuple_cost;
}
