/*
 * col/f2/f2_v6_card.c - SiftCardStatsView of an index of format major 2 (agg/plan/card_est.h), folded from what its parts already hold: the partition directory of the plan snapshot, the
 * column statistics (SEC_COL_STATS) and the zones (SEC_ZONES) of the largest SIFT_V6_CARD_PARTS parts. The view is built once for a manifest version and kept with the snapshot, so a plan
 * after the first reads nothing; a part that was not read leaves the facts that need every part (the range of a leaf, the documents that hold a key) unclaimed.
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "core/memtrack.h"
#include "col/f2/col_stats.h"
#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/f2_v6_bridge.h"
#include "col/granule/granule.h"

#define SIFT_V6_CARD_PARTS	16

typedef struct CardPart
{
	int			pi;
	const SiftPartDesc *d;
	uint64		part_id;
	SiftPart   *part;
	SiftGranuleDesc *dir;
	SiftColStats cs;
	bool		has_cs;
	uint32		ng;
	int			seq;			/* the place of the part in the manifest */
} CardPart;

static void
card_swap(CardPart *a, CardPart *b)
{
	CardPart	t = *a;

	*a = *b;
	*b = t;
}

typedef struct CardSample
{
	int			part;			/* of the chosen parts */
	uint32		g;
} CardSample;

/* The order of the chosen parts: the most rows first, the earlier of the manifest first among equals */
static bool
ranks_before(const CardPart *a, const CardPart *b)
{
	return a->d->nrows != b->d->nrows ? a->d->nrows > b->d->nrows : a->seq < b->seq;
}

static int
by_rank(const void *x, const void *y)
{
	return ranks_before(x, y) ? -1 : ranks_before(y, x) ? 1 : 0;
}

/* out[0 .. n) as a heap whose root is the part that ranks last of those kept: a part ranking before it takes its place */
static void
heap_down(CardPart *out, int n, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					last = i;

		if (l < n && ranks_before(&out[last], &out[l]))
			last = l;
		if (l + 1 < n && ranks_before(&out[last], &out[l + 1]))
			last = l + 1;
		if (last == i)
			return;
		card_swap(&out[i], &out[last]);
		i = last;
	}
}

static int
choose_parts(Relation index, const SiftPlanSnapshot *plan, CardPart *out, bool *all, uint64 *read_rows)
{
	int			n = 0,
				total = 0;

	for (int i = 0; i < plan->npartitions; i++)
		for (int j = 0; j < plan->partlists[i].nparts; j++)
		{
			CardPart	c = {.pi = i,.d = &plan->partlists[i].parts[j],.part_id = plan->partitions[i].part_id,.seq = total++};

			if (n < SIFT_V6_CARD_PARTS)
			{
				out[n++] = c;
				if (n == SIFT_V6_CARD_PARTS)
					for (int k = n / 2 - 1; k >= 0; k--)
						heap_down(out, n, k);
			}
			else if (ranks_before(&c, &out[0]))
			{
				out[0] = c;
				heap_down(out, n, 0);
			}
		}
	qsort(out, n, sizeof(CardPart), by_rank);
	*all = n == total;
	*read_rows = 0;
	for (int k = 0; k < n; k++)
	{
		CardPart   *p = &out[k];

		p->part = sift_part_open_desc(index, p->d, (uint32) p->part_id);
		p->ng = p->part->hdr.ngranules;
		p->dir = sift_part_granule_dir(index, p->part);
		*read_rows += p->d->nrows;
	}
	for (int k = 0; k < n; k++)
	{
		uint64		len;
		void	   *blob = sift_part_read_section(index, out[k].part, SEC_COL_STATS, &len);

		out[k].has_cs = blob != NULL && sift_col_stats_open(blob, len, out[k].part->hdr.ncols, out[k].ng, &out[k].cs) == NULL;
	}
	return n;
}

static SiftZone *
part_zones(Relation index, const CardPart *p, int leaf)
{
	const SiftSecDirEntry *zs = sift_part_section(p->part, SEC_ZONES);
	SiftZone   *z;

	if (zs == NULL || zs->len != (uint64) p->part->hdr.ncols * p->ng * sizeof(SiftZone))
		return NULL;
	z = palloc(p->ng * sizeof(SiftZone));
	sift_part_read_section_bytes(index, p->part, zs, (uint64) leaf * p->ng * sizeof(SiftZone), (uint64) p->ng * sizeof(SiftZone), z);
	return z;
}

static SiftCardZone
card_zone(const SiftZone *z)
{
	if (z->flags & SIFT_ZONE_NONE)
		return (SiftCardZone) {.min = INT64_MIN,.max = INT64_MAX,.nnull = z->nnull};
	return (SiftCardZone) {.min = z->min,.max = z->max,.nnull = z->nnull,
	.flags = (z->flags & SIFT_ZONE_KEY_ALL_ABSENT ? SIFT_CZONE_KEY_NONE : 0) | (z->flags & SIFT_ZONE_KEY_ALL_PRESENT ? SIFT_CZONE_KEY_ALL : 0)};
}

typedef struct LeafFold
{
	SiftCardZone *z;			/* the zones of every granule of the chosen parts, in order */
	uint32	   *nrows;
	uint32		n;
	bool		ints;			/* every zone has a range */
	bool		zones;			/* every chosen part keeps zones */
	uint64		key_rows;
	bool		key_known;
} LeafFold;

static void
fold_leaf(Relation index, const CardPart *parts, int nparts, int leaf, LeafFold *lf, SiftF2ZoneFold *zf)
{
	lf->ints = lf->zones = lf->key_known = true;
	lf->n = 0;
	lf->key_rows = 0;
	for (int k = 0; k < nparts; k++)
	{
		const SiftZone *z = part_zones(index, &parts[k], leaf);

		if (z == NULL)
		{
			lf->zones = false;
			continue;
		}
		for (uint32 g = 0; g < parts[k].ng; g++)
		{
			const uint32 nr = parts[k].dir[g].nrows;

			lf->z[lf->n] = card_zone(&z[g]);
			lf->nrows[lf->n++] = nr;
			lf->ints &= !(z[g].flags & SIFT_ZONE_NONE) || z[g].nnull >= nr;
			if (z[g].flags & SIFT_ZONE_NONE)
				zf->rows += nr, zf->null_rows += Min(z[g].nnull, nr);
			else
				sift_f2_zone_fold_add(zf, &lf->z[lf->n - 1], nr);
			if (z[g].flags & SIFT_ZONE_KEY_ALL_PRESENT)
				lf->key_rows += nr;
			else
				lf->key_known &= (z[g].flags & SIFT_ZONE_KEY_ALL_ABSENT) != 0;
		}
		pfree((SiftZone *) z);
	}
}

static void
fill_ndv(SiftColCardStats *cs, const CardPart *parts, int nparts, int leaf, uint64 read_rows, uint64 total_rows)
{
	double		max_ndv = 0,
				sum_ndv = 0;
	uint64		bytes = 0,
				nulls = 0;

	for (int k = 0; k < nparts; k++)
	{
		SiftPartColStat st;

		if (!parts[k].has_cs)
			return;
		st = sift_col_stats_get(&parts[k].cs, leaf);
		max_ndv = Max(max_ndv, st.est_ndv);
		sum_ndv += st.est_ndv;
		bytes += st.total_bytes;
		nulls += sift_col_stats_nulls(&parts[k].cs, leaf);
	}
	cs->flags |= SIFT_CSTAT_HAS_NDV;
	cs->ndv_est = sift_f2_ndv_union(max_ndv, sum_ndv, (double) read_rows, (double) total_rows);
	cs->avg_comp_bytes_row = read_rows > 0 ? (double) bytes / read_rows : 0;
	cs->null_rows = Min((uint64) ((double) nulls * total_rows / Max(read_rows, 1)), total_rows);
}

static void
fill_leaf(SiftColCardStats *cs, const LeafFold *lf, const SiftF2ZoneFold *zf, bool all_parts, uint64 total_rows, SiftF2MidW *scratch)
{
	cs->total_rows = total_rows;
	if (lf->zones && all_parts)
	{
		cs->null_rows = zf->null_rows;
		if (lf->ints && zf->any)
		{
			cs->flags |= SIFT_CSTAT_HAS_RANGE;
			cs->min_i64 = zf->min;
			cs->max_i64 = zf->max;
		}
		if (lf->key_known)
		{
			cs->flags |= SIFT_CSTAT_HAS_KEY_PRESENT;
			cs->key_present_rows = lf->key_rows;
		}
	}
	if (lf->zones && lf->ints && zf->any)
	{
		sift_f2_zone_shape(lf->z, lf->nrows, lf->n, zf->min, zf->max, &cs->zone_overlap_ratio, &cs->zone_monotonicity);
		if (all_parts && sift_f2_zone_hist(lf->z, lf->nrows, lf->n, zf->min, zf->max, cs->hist_bounds, scratch) > 0)
		{
			cs->flags |= SIFT_CSTAT_HAS_HIST;
			cs->n_hist = SIFT_CARD_HIST_BUCKETS;
		}
	}
	cs->avg_dec_bytes_row = Max(8.0, cs->avg_comp_bytes_row);
}

static Size
view_size(uint32 ncols, uint32 nparts, uint32 nsample)
{
	return MAXALIGN(sizeof(SiftCardStatsView)) + MAXALIGN(ncols * sizeof(SiftColCardStats)) + MAXALIGN(nparts * sizeof(SiftCardPartition)) +
		MAXALIGN(nsample * sizeof(SiftSampleGranule)) + MAXALIGN((Size) ncols * nsample * sizeof(SiftCardZone));
}

/* The scope roots of the summary the backend holds for the first scope (its keys ascending, as the roots are), and the sum of their rows once for every estimate that asks for the mean root */
static void
fold_scope(SiftCardStatsView *v, Relation index, const SiftPlanSnapshot *plan)
{
	const SiftScopeMeta *m = sift_f2_scope_cache_peek(index, plan, 0);
	SiftCardScopeRoot *roots;

	if (m == NULL || m->hdr.nkeys == 0)
		return;
	/* check-memory: allow one root of the summary the cache already holds, whose roots array is as long */
	roots = MemoryContextAlloc(plan->cx, m->hdr.nkeys * sizeof(SiftCardScopeRoot));
	for (uint32 i = 0; i < m->hdr.nkeys; i++)
	{
		roots[i] = (SiftCardScopeRoot) {.key = m->roots[i].scope_key,.nrows = m->roots[i].row_count};
		v->scope_total_rows += m->roots[i].row_count;
	}
	v->scope = roots;
	v->n_scope = m->hdr.nkeys;
}

static SiftCardStatsView *
build_view(Relation index, const SiftF2Rel *f, const SiftPlanSnapshot *plan)
{
	/* MEM[MEM-COL-30]: transient card view work ctx (small sizes; the zones of each leaf are freed as they are folded; the view itself lives in plan->cx) */
	MemoryContext work = AllocSetContextCreateInternal(CurrentMemoryContext, "sift card view", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	MemoryContext old = MemoryContextSwitchTo(work);
	const uint64 live = plan->total_rows - plan->total_dead;
	CardPart	chosen[SIFT_V6_CARD_PARTS];
	bool		all_parts;
	uint64		read_rows;
	const int	nchosen = choose_parts(index, plan, chosen, &all_parts, &read_rows);
	uint64		granules = 0;
	uint32		stride,
				nsample = 0;
	CardSample *at;
	LeafFold	lf;
	SiftCardStatsView *v;
	Size		off;
	char	   *base;
	SiftF2MidW *scratch;

	for (int k = 0; k < nchosen; k++)
		granules += chosen[k].ng;
	stride = sift_f2_sample_stride(granules, SIFT_CARD_SAMPLE_GRANULES);
	nsample = (uint32) ((granules + stride - 1) / stride);
	at = palloc(Max(nsample, 1) * sizeof(CardSample));
	for (uint32 s = 0, k = 0, g0 = 0; s < nsample; s++)
	{
		const uint64 want = (uint64) s * stride;

		while (want >= g0 + chosen[k].ng)
			g0 += chosen[k++].ng;
		at[s] = (CardSample) {.part = (int) k,.g = (uint32) (want - g0)};
	}
	lf.z = palloc(Max(granules, 1) * sizeof(SiftCardZone));
	lf.nrows = palloc(Max(granules, 1) * sizeof(uint32));
	scratch = palloc(Max(granules, 1) * sizeof(SiftF2MidW));
	MemoryContextSwitchTo(plan->cx);
	base = palloc0(view_size(f->ncols, plan->npartitions, nsample));
	MemoryContextSwitchTo(work);
	v = (SiftCardStatsView *) base;
	off = MAXALIGN(sizeof(SiftCardStatsView));
	v->cols = (SiftColCardStats *) (base + off);
	off += MAXALIGN(f->ncols * sizeof(SiftColCardStats));
	v->parts = (SiftCardPartition *) (base + off);
	off += MAXALIGN(plan->npartitions * sizeof(SiftCardPartition));
	v->sample = (SiftSampleGranule *) (base + off);
	off += MAXALIGN(nsample * sizeof(SiftSampleGranule));
	v->sample_zones = (SiftCardZone *) (base + off);
	v->total_live_rows = live;
	v->total_granules = granules;
	v->ncols = f->ncols;
	v->nparts = plan->npartitions;
	v->n_sample = nsample;
	for (int i = 0; i < plan->npartitions; i++)
	{
		const SiftPartitionEntry *e = &plan->partitions[i];
		const bool	timed = f->spec.time_cols[0] > 0;

		((SiftCardPartition *) v->parts)[i] = (SiftCardPartition) {.rows = e->total_rows - e->total_dead,
		.lo = timed ? e->time_min[0] : INT64_MIN,.hi = timed ? e->time_max[0] : INT64_MAX};
		v->has_l0 |= e->l0_slab_blk != InvalidBlockNumber;
	}
	for (uint32 s = 0; s < nsample; s++)
		((SiftSampleGranule *) v->sample)[s] = (SiftSampleGranule) {.part_idx = chosen[at[s].part].pi,.nrows = chosen[at[s].part].dir[at[s].g].nrows,
		.weight = (float4) ((double) stride * (double) live / Max((double) read_rows, 1.0))};
	for (int c = 0; c < f->ncols; c++)
	{
		SiftColCardStats *cs = &((SiftColCardStats *) v->cols)[c];
		SiftF2ZoneFold zf = {0};

		cs->col = (uint16) c;
		fold_leaf(index, chosen, nchosen, c, &lf, &zf);
		fill_ndv(cs, chosen, nchosen, c, read_rows, live);
		fill_leaf(cs, &lf, &zf, all_parts, live, scratch);
		for (uint32 s = 0; s < nsample; s++)
			((SiftCardZone *) v->sample_zones)[(size_t) c * nsample + s] = lf.zones && lf.n == granules ? lf.z[(uint64) s * stride] : (SiftCardZone) {.min = INT64_MIN,.max = INT64_MAX};
	}
	if (sift_f2_scoped(index))
		fold_scope(v, index, plan);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(work);
	return v;
}

const SiftCardStatsView *
sift_f2_v6_card_view(Oid indexoid)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	const SiftPlanSnapshot *plan = sift_plan_cache_get(index, CurrentMemoryContext);

	if (!sift_plan_card_built(plan))
		sift_plan_card_put(plan, plan->total_rows > plan->total_dead ? build_view(index, sift_f2_rel(index), plan) : NULL);
	index_close(index, AccessShareLock);
	return sift_plan_card_get(plan);
}
