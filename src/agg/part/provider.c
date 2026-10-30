/*
 * agg/part/provider.c - a partitioned table as one column source: the provider
 * The provider table behind the seam of agg/colsrc.h and its callbacks: the unit of the
 * partition in turn, the reads, the top-N and the counters.
 */
#include "postgres.h"

#include "access/table.h"

#include "core/cmp.h"
#include "agg/part/int.h"

/*
 * The partitions of a partitioned table as one column source (agg/part.h, docs/AGG.md "Partitioned tables").
 *
 * The wrapper opens the source of every partition (sift_colsrc_open_part) and is a provider of its own behind the seam of agg/colsrc.h. Its
 * units are those of the partitions one after the other: begin cuts the range of superblocks it is given into the ranges of the partitions it
 * overlaps (the superblocks of the partitions are numbered one after the other) and starts them one at a time, and every call about the
 * unit the source is at goes to the partition that returned it. Nothing is merged: the dictionaries of the units are their own already, and the
 * executor translates by value. The one thing the wrapper changes of what a partition says is the id of a dictionary (SiftSrcDict.id): a
 * partition numbers its dictionaries by itself, so two of them can hold the same id for different dictionaries.
 */


/* Starts the next partition that has superblocks in [lo, hi); false when there is none */
static bool
start_next(PartSource *ps)
{
	for (int i = ps->next; i < ps->n && ps->off[i] < ps->hi; i++)
	{
		uint32		from = Max(ps->lo, ps->off[i]),
					to = Min(ps->hi, ps->off[i + 1]);

		if (from >= to)
			continue;
		if (ps->cur >= 0)
			sift_colsrc_unpin(ps->src[ps->cur]);	/* a scan of many partitions would pin the pages of all of them */
		/* a worker has the offsets from the leader and opened the first partition only: the others when its items reach them */
		open_part(ps, i);
		ps->cur = i;
		ps->next = i + 1;
		sift_colsrc_begin(ps->src[i], from - ps->off[i], to - ps->off[i], ps->prune, ps->nprune);
		return true;
	}
	ps->next = ps->n;
	return false;
}

static void
part_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	PartSource *ps = impl;
	int			lo,
				hi;

	count_sbs(ps);
	ps->lo = sb_lo;
	ps->hi = sb_hi;
	ps->prune = prune;
	ps->nprune = nprune;
	/* the first partition that ends past sb_lo (the offsets only grow): a scan of thousands of partitions begins once for each item */
	lo = 0;
	hi = ps->n;
	SIFT_BISECT(int, mid, lo, hi, ps->off[mid + 1] <= sb_lo);
	ps->next = lo;
	if (ps->cur >= 0)
		sift_colsrc_unpin(ps->src[ps->cur]);
	ps->cur = -1;
}

static bool
part_next(void *impl, SiftSrcUnit *unit)
{
	PartSource *ps = impl;

	for (;;)
	{
		if (ps->cur >= 0 && sift_colsrc_next(ps->src[ps->cur], &ps->cunit))
		{
			*unit = (SiftSrcUnit) {.sb = ps->off[ps->cur] + ps->cunit.sb,.nrow = ps->cunit.nrow,.priv = ps};
			return true;
		}
		if (!start_next(ps))
			return false;
	}
}

/* the dictionary of a read as the wrapper presents it: the id is the partition's own, made unique among the partitions (a new dictionary gets a new id) */
const SiftSrcDict *
presented(PartSource *ps, int slot, const SiftSrcDict *d)
{
	PartId	   *m = &ps->id[slot];

	if (d == NULL)
		return NULL;
	if (m->child != ps->cur + 1 || m->in != d->id)
	{
		m->child = ps->cur + 1;
		m->in = d->id;
		m->out = (const void *) (uintptr_t) (PART_DICT_ID_BASE + ++ps->idseq);
	}
	ps->dict[slot] = *d;
	ps->dict[slot].id = m->out;
	return &ps->dict[slot];
}

static void
part_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	sift_colsrc_read(cur_of(impl), unit_of(impl), leaf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	sift_colsrc_read_into(cur_of(impl), unit_of(impl), leaf, buf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	sift_colsrc_read_sel(cur_of(impl), unit_of(impl), leaf, bits, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static int64
part_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_leaf_n(cur_of(impl), unit_of(impl), leaf);
}

static void
part_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_live(cur_of(impl), unit_of(impl), bits);
}

static void
part_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_vet(cur_of(impl), unit_of(impl), bits);
}

static bool
part_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_vet_needed(cur_of(impl), unit_of(impl));
}

/* ---- the tests the partitions apply on what they store: every partition gets them ---- */

/* ---- pre-aggregates: each unit answers from its partition ---- */

/* ---- the Vulkan path ---- */

/* ---- the top-N: the rows of a unit are the partition's, its TIDs are the partition's heap's ---- */

static void
part_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	sift_colsrc_tids(cur_of(impl), unit_of(impl), rows, n, tids);
}

static void
part_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z)
{
	sift_colsrc_zone(cur_of(impl), unit_of(impl), leaf, z);
}

/* the zone over the partitions of the superblocks [lo, hi) together: the units they hold add up, the range is the union of the ranges that have a value */
static uint32
part_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	PartSource *ps = impl;
	uint32		units = 0;

	count_sbs(ps);
	*z = (SiftSrcZone) {.min = PG_INT64_MAX,.max = PG_INT64_MIN};
	for (int i = 0; i < ps->n && ps->off[i] < hi; i++)
	{
		uint32		from = Max(lo, ps->off[i]),
					to = Min(hi, ps->off[i + 1]);
		SiftSrcZone pz;

		if (from >= to)
			continue;
		open_part(ps, i);
		units += sift_colsrc_zone_range(ps->src[i], from - ps->off[i], to - ps->off[i], leaf, &pz);
		z->nvals += pz.nvals;
		z->nnull += pz.nnull;
		z->inexact |= pz.inexact;
		if (pz.nnull < pz.nvals)
		{
			z->min = Min(z->min, pz.min);
			z->max = Max(z->max, pz.max);
		}
	}
	return units;
}

static void
part_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	sift_colsrc_read_rows(cur_of(impl), unit_of(impl), leaf, rows, n, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

int
sift_aggpart_count(SiftSrcSource *src)
{
	return ps_of(src->impl)->n;
}

int
sift_aggpart_current(SiftSrcSource *src)
{
	return ps_of(src->impl)->cur;
}

Relation
sift_aggpart_heap(SiftSrcSource *src, int i)
{
	PartSource *ps = src->impl;

	if (ps->heap[i] == NULL)
		ps->heap[i] = table_open(ps->heapoid[i], AccessShareLock);
	return ps->heap[i];
}

/*
 * No match set (select): the planner never gives a partitioned table a search. The others are the partitions' own; a provider that lacks
 * one of them has no partitions (sift_colsrc_can_part), or no top-N (sift_colsrc_can_tids).
 */
const SiftSrcProvider part_provider = {
	.close = part_close,
	.source_layout = part_source_layout,
	.nsb = part_nsb,
	.sb_heap = part_sb_heap,
	.begin = part_begin,
	.next = part_next,
	.read = part_read,
	.live = part_live,
	.vet = part_vet,
	.vet_needed = part_vet_needed,
	.counters = part_counters,
	.leaf_n = part_leaf_n,
	.read_into = part_read_into,
	.overflow = part_overflow,
	.set_filters = part_set_filters,
	.narrow = part_narrow,
	.filter = part_filter,
	.read_sel = part_read_sel,
	.filter_types = part_filter_types,
	.tolerate_ovf = part_tolerate_ovf,
	.set_raw = part_set_raw,
	.set_numsum = part_set_numsum,
	.set_recheck = part_set_recheck,
	.overflow_raise = part_overflow_raise,
	.recheck_rows = part_recheck_rows,
	.tids = part_tids,
	.zone = part_zone,
	.zone_range = part_zone_range,
	.read_rows = part_read_rows,
	.read_types = part_read_types,
	.read_jnull = part_read_jnull,
	.prune_str = part_prune_str,
	.filter_str = part_filter_str,
	.set_wide = part_set_wide,
	.filter_wide = part_filter_wide,
	.unit_exact = part_unit_exact,
	.unit_dead = part_unit_dead,
	.unit_except = part_unit_except,
	.unit_agg = part_unit_agg,
	.unit_hist = part_unit_hist,
	.unit_rollup_tuples = part_unit_rollup_tuples,
	.unit_rollup = part_unit_rollup,
	.unit_rollup_miss = part_unit_rollup_miss,
	.gpu_unit = part_gpu_unit,
	.gpu_dense = part_gpu_dense,
	.gpu_chunk = part_gpu_chunk,
	.gpu_dict = part_gpu_dict,
};

/* ---- planner side ---- */

