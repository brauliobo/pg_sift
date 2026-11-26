/*
 * col/v1_engine/store_card.c - SiftCardStatsView of a Format 1 index (agg/cost/card_est.h), folded from the statistics of its leaves (SiftSrcStats) when the planner asks: the rows, the
 * NULLs, the range and the distinct values of each leaf, and the zones of the sample of units the planner already keeps. A segment records its stored bytes as a whole, not by leaf: the bytes
 * of a row of a leaf are the share of the index's bytes per row that the width of its values (its range, its dictionary, a float's 8 bytes) gives it.
 */
#include "postgres.h"

#include <math.h>

#include "agg/cost/card_est.h"
#include "col/v1/store_ops.h"

#define SAMPLE_UNITS	SIFT_CARD_SAMPLE_GRANULES

/* the bits a value of the leaf takes as stored: the span of an integer, the codes of a dictionary, a float whole */
static double
value_bits(const SiftSrcLeaf *l, const SiftSrcStats *st)
{
	const int	i = l->id;

	if (l->kind != SCL_SCALAR)
		return 8;
	if (l->vtype == SCV_F64)
		return 64;
	if (l->vtype == SCV_CODE)
		return Max(1.0, ceil(log2(Max(st->ndistinct[i], 2.0))));
	if (st->max[i] > st->min[i])
		return Max(1.0, ceil(log2(st->max[i] - st->min[i] + 2.0)));
	return l->vtype == SCV_I32 ? 32 : 64;
}

static bool
has_range(const SiftSrcLeaf *l, const SiftSrcStats *st)
{
	return l->kind == SCL_SCALAR && !l->wide && (l->vtype == SCV_I32 || l->vtype == SCV_I64) && st->max[l->id] > st->min[l->id];
}

/* the bytes of a row of each leaf: the bytes of the index per row, shared by the width of the values of each leaf (a child's per element, an element count's per row), and no more than that width: the
 * headers of a few segments are no cost of a row */
static void
leaf_bytes(const SiftSrcLayout *lay, const SiftSrcStats *st, double *per_row)
{
	double		total = 0;

	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];
		const double per = l->parent >= 0 ? Max(st->elems_per_row[l->parent], 1.0) : 1.0;

		per_row[i] = value_bits(l, st) / 8.0 * per;
		total += per_row[i];
	}
	for (int i = 0; i < lay->nleaves && total > 0 && st->bytes > 0 && st->nrows > 0; i++)
		per_row[i] = Min(per_row[i] * st->bytes / st->nrows / total, per_row[i]);
}

static SiftColCardStats
leaf_stats(const SiftSrcLeaf *l, const SiftSrcStats *st, double rows, double bytes_row)
{
	const int	i = l->id;
	const double vals = l->parent >= 0 ? rows * Max(st->elems_per_row[l->parent], 1.0) : rows;
	SiftColCardStats cs = {.col = (uint16) i,.total_rows = (uint64) vals,.null_rows = (uint64) (st->null_frac[i] * vals),
		.avg_comp_bytes_row = bytes_row,.avg_dec_bytes_row = Max(8.0, bytes_row)};

	if (l->kind == SCL_SCALAR && st->ndistinct[i] > 0)
	{
		cs.flags |= SIFT_CSTAT_HAS_NDV;
		cs.ndv_est = st->ndistinct[i];
	}
	if (has_range(l, st))
	{
		cs.flags |= SIFT_CSTAT_HAS_RANGE;
		cs.min_i64 = (int64) st->min[i];
		cs.max_i64 = (int64) st->max[i];
	}
	return cs;
}

/* the zone of unit k of the sample in leaf i: a unit with no value is all NULL, a unit whose zone overflowed spans the range of the leaf, and the others keep the NULLs of the leaf on average */
static SiftCardZone
sample_zone(const SiftSrcLeaf *l, const SiftSrcStats *st, uint32 k, uint32 unit_rows)
{
	const SiftSrcZones *z = st->zones;
	const int	i = l->id;
	const uint32 nulls = (uint32) (st->null_frac[i] * unit_rows);

	if (z->lo[i] == NULL)
		return (SiftCardZone) {.min = INT64_MIN,.max = INT64_MAX,.nnull = nulls};
	if (z->lo[i][k] > z->hi[i][k])
		return (SiftCardZone) {.min = INT64_MIN,.max = INT64_MAX,.nnull = unit_rows};
	if (z->lo[i][k] == INT64_MIN && z->hi[i][k] == INT64_MAX && has_range(l, st))
		return (SiftCardZone) {.min = (int64) st->min[i],.max = (int64) st->max[i],.nnull = nulls};
	return (SiftCardZone) {.min = z->lo[i][k],.max = z->hi[i][k],.nnull = nulls};
}

const SiftCardStatsView *
sift_v1_card_view(const SiftSrcLayout *lay, const SiftSrcStats *st)
{
	const uint32 nsample = st->zones != NULL ? Min(st->zones->n, SAMPLE_UNITS) : 0;
	const double rows = Max(st->nrows, 1.0);
	const uint32 unit_rows = (uint32) Min(rows / Max(st->nunits, 1.0), PG_UINT16_MAX);
	char	   *base = palloc0(MAXALIGN(sizeof(SiftCardStatsView)) + MAXALIGN(lay->nleaves * sizeof(SiftColCardStats)) + MAXALIGN(nsample * sizeof(SiftSampleGranule)) +
							MAXALIGN((Size) lay->nleaves * nsample * sizeof(SiftCardZone)));
	SiftCardStatsView *v = (SiftCardStatsView *) base;
	SiftColCardStats *cols = (SiftColCardStats *) (base + MAXALIGN(sizeof(SiftCardStatsView)));
	SiftSampleGranule *sample = (SiftSampleGranule *) ((char *) cols + MAXALIGN(lay->nleaves * sizeof(SiftColCardStats)));
	SiftCardZone *zones = (SiftCardZone *) ((char *) sample + MAXALIGN(nsample * sizeof(SiftSampleGranule)));
	double	   *bytes = palloc(Max(lay->nleaves, 1) * sizeof(double));

	leaf_bytes(lay, st, bytes);
	*v = (SiftCardStatsView) {.total_live_rows = (uint64) rows,.total_granules = (uint64) Max(st->nunits, 1.0),.total_bytes = (uint64) st->bytes,.ncols = lay->nleaves,
		.nfiles = 1,.rows_from_pg = true,.n_sample = nsample,.cols = cols,.sample = sample,.sample_zones = zones};
	for (int i = 0; i < lay->nleaves; i++)
		cols[i] = leaf_stats(&lay->leaf[i], st, rows, bytes[i]);
	for (uint32 g = 0; g < nsample; g++)
	{
		const uint32 k = (uint32) ((uint64) g * st->zones->n / nsample);

		sample[g] = (SiftSampleGranule) {.nrows = (uint16) unit_rows,.weight = (float4) (rows / ((double) nsample * Max(unit_rows, 1)))};
		for (int i = 0; i < lay->nleaves; i++)
			zones[(size_t) i * nsample + g] = sample_zone(&lay->leaf[i], st, k, unit_rows);
	}
	pfree(bytes);
	return v;
}

/* the first leaf of column col (0-based) of the index: the leaf the statistics of an attribute are read from */
int
sift_v1_card_leaf(const SiftSrcLayout *lay, int col)
{
	for (int i = 0; i < lay->nleaves; i++)
		if (lay->leaf[i].attno == col + 1 && lay->leaf[i].parent < 0)
			return i;
	return -1;
}
