/*
 * agg/par/items.c - the items of a parallel scan
 * The shared state of a scan, the superblocks the zones keep, the scan of an item and the counter the participants take items from.
 */
#include "postgres.h"

#include "agg/part.h"
#include "agg/par/int.h"

/*
 * The superblocks are split in items that the participants take from a counter. Float sums are order dependent, so that a result
 * does not depend on the number of workers: such a scan has SIFT_AGG_LANES items, lane l being the superblocks l, l + SIFT_AGG_LANES, ..., and
 * a participant keeps the float sums of each lane it scanned apart (AggAcc.lv), in the one table it scans into; the leader adds the lanes in lane
 * order, whoever scanned them. The other aggregates are exact in any order: items are MORSEL_SBS superblocks.
 */
#define MORSEL_SBS	2
#define MORSEL_SUPER_DIV	4

Size
shared_size(const SiftAggSpec *s, uint32 norder)
{
	return offsetof(ScanShared, partoff) + (s->nparts + 1 + norder) * sizeof(uint32);
}

uint32 *
shared_order(ScanShared *sh, const SiftAggSpec *s)
{
	return sh->partoff + s->nparts + 1;
}

/*
 * A top-N takes windows of superblocks in the visit order the leader's probes chose: one superblock each in the walk's order (norder
 * superblocks, copied), else the superblocks of a match set's chunk (the set is made for a chunk), else one, from the last when the sort
 * column grows toward it.
 */
static void
topn_shared_init(ScanShared *sh, AggExec *ex)
{
	sh->topn_reverse = sift_topn_reverse(ex->topn);
	sh->topn_walk = ex->nvorder > 0;
	sh->topn_item = ex->search ? sift_search_chunk_sbs : 1;
	sh->nitems = sh->topn_walk ? sh->nsb : (sh->nsb + sh->topn_item - 1) / sh->topn_item;
	sift_topn_bound_init(&sh->topn_bound);
	if (sh->topn_walk)
		memcpy(shared_order(sh, ex->spec), ex->vorder, ex->nvorder * sizeof(uint32));
}

/* a serial scan of a table partitioned by (part of) the group key: an item is a partition, whose groups are final when its units end */
bool
part_flush_on(const AggExec *ex)
{
	return ex->spec->part_final && ex->spec->nparts > 0 && ex->spec->nworkers == 0;
}

void
shared_init(ScanShared *sh, AggExec *ex)
{
	sh->nsb = sift_colsrc_nsb(ex->src);
	if (ex->spec->nparts > 0)
		sift_aggpart_offsets(ex->src, sh->partoff);
	sh->ordered = sift_aggspec_ordered(ex->spec);
	sh->morsel = sift_aggspec_parallel_ok(ex->spec) ? MORSEL_SBS : Max(sh->nsb, 1);
	/* a superunit is taken whole by one item (a scan that has workers shares the span of four, so that the participants find more than two items) */
	if (ex->super_on || ex->whole_on)
		sh->morsel = Max(sh->morsel, sift_colsrc_super_span(ex->src) / (ex->spec->nworkers > 0 ? MORSEL_SUPER_DIV : 1));
	/* a segment is taken whole by a serial scan only when no item cuts it (a count that the segment answers is of the whole segment too) */
	if ((ex->whole_on || ex->pcount) && ex->spec->nworkers == 0)
		sh->morsel = Max(sh->nsb, 1);
	sh->nitems = sh->ordered ? SIFT_AGG_LANES : (sh->nsb + sh->morsel - 1) / sh->morsel;
	if (part_flush_on(ex))
		sh->nitems = ex->spec->nparts;
	sh->gpart_n = 0;
	pg_atomic_init_u32(&sh->next, 0);
	pg_atomic_init_flag(&sh->kept_counted);
	if (ex->skip != NULL)			/* the leader's map, made to decide on workers, counted */
		pg_atomic_test_set_flag(&sh->kept_counted);
	if (ex->topn)
		topn_shared_init(sh, ex);
}

SkipMap *
skip_map(AggExec *ex)
{
	SkipMap    *m = ex->skip;

	if (m == NULL)
	{
		uint32		nsb = sift_colsrc_nsb(ex->src);

		m = ex->skip = MemoryContextAllocZero(ex->cxt, sizeof(SkipMap));
		m->nsb = nsb;
		m->kept = MemoryContextAllocZero(ex->cxt, (nsb + 63) / 64 * sizeof(uint64));
		m->ok = sift_colsrc_kept(ex->src, ex->prune, ex->nzone, nsb, m->kept, &m->rows, ex->shared == NULL || pg_atomic_test_set_flag(&ex->shared->kept_counted));
	}
	return m;
}

/* Is there no superblock of [lo, hi) the zones leave a unit in? */
static bool
range_skipped(AggExec *ex, uint32 lo, uint32 hi)
{
	SkipMap    *m = skip_map(ex);

	hi = Min(hi, m->nsb);
	if (!m->ok || lo >= hi)
		return false;
	for (uint32 w = lo / 64; w < (hi + 63) / 64; w++)
	{
		uint64		mask = ~UINT64CONST(0);

		if (w == lo / 64)
			mask &= ~UINT64CONST(0) << (lo % 64);
		if (w == (hi - 1) / 64 && hi % 64)
			mask &= (UINT64CONST(1) << (hi % 64)) - 1;
		if (m->kept[w] & mask)
			return false;
	}
	return true;
}

/* The first superblock from sb, a step apart, that the zones leave a unit in (nsb or more when there is none; sb itself without the map) */
static uint32
next_kept(AggExec *ex, uint32 sb, uint32 step)
{
	SkipMap    *m = skip_map(ex);

	if (!m->ok)
		return sb;
	while (sb < m->nsb)
	{
		uint64		rest = m->kept[sb / 64] >> (sb % 64);

		if (rest & 1)
			return sb;
		if (rest == 0)			/* nothing is kept to the end of the word: the first superblock of the next word on the same step */
			sb += ((sb / 64 + 1) * 64 - sb + step - 1) / step * step;
		else
			sb += step;
	}
	return sb;
}

void
scan_units(AggExec *ex, uint32 lo, uint32 hi)
{
	lo = Max(lo, ex->tid_sb_lo);
	hi = Min(hi, ex->tid_sb_hi);
	if (lo >= hi || ex->pass_stop || range_skipped(ex, lo, hi))
		return;
	sift_colsrc_begin(ex->src, lo, hi, ex->prune, ex->nzone);
	while (sift_colsrc_next(ex->src, &ex->unit))
	{
		process_unit(ex);
		if (ex->spill)
			spill_check(ex);
		if (ex->pass_stop || sift_aggbe_stopped(ex->be, ex->be_state))
			break;				/* the scan is run again without the backend (backend_finish) */
	}
}

/* With a match set, the source keeps the matching rows of the units of the range only (agg/search.h) */
static void
scan_range(AggExec *ex, uint32 lo, uint32 hi)
{
	if (ex->search == NULL)
	{
		scan_units(ex, lo, hi);
		return;
	}
	if (!ex->sb_heap)
	{
		/* units that are not superblocks of the heap hold rows of any block: the match set is the whole table's, made once by a participant */
		if (ex->search_all == NULL)
			ex->search_all = sift_search_range(ex->search, 0, sift_colsrc_nsb(ex->src));
		sift_colsrc_select(ex->src, ex->search_all);
		scan_units(ex, lo, hi);
		return;
	}
	for (uint32 a = lo; a < hi; a += sift_search_chunk_sbs)
	{
		sift_colsrc_select(ex->src, sift_search_range(ex->search, a, Min(a + sift_search_chunk_sbs, hi)));
		scan_units(ex, a, Min(a + sift_search_chunk_sbs, hi));
	}
}

/* a grouping set without columns has its group even when no row reached it, as the plain aggregate has its row */
void
empty_set_groups(AggExec *ex)
{
	for (int set = 0; set < ex->spec->nsets; set++)
		if (ex->spec->setmask[set] == (uint32) 1 << (ex->spec->nkeys - 1))
		{
			ex->curset = set;
			ex->arm_mask = ex->spec->setmask[set];
			compute_gids(ex, 1);
			ex->seen[ex->gid[0]] = Max(ex->seen[ex->gid[0]], 1);
		}
	ex->arm_mask = ~(uint32) 0;
}

static void
scan_item(AggExec *ex, uint32 item)
{
	ScanShared *sh = ex->shared;

	if (part_flush_on(ex))
		scan_range(ex, sh->partoff[item], sh->partoff[item + 1]);
	else if (sh->ordered)
		for (uint32 sb = next_kept(ex, item, SIFT_AGG_LANES); sb < sh->nsb; sb = next_kept(ex, sb + SIFT_AGG_LANES, SIFT_AGG_LANES))
			scan_range(ex, sb, sb + 1);
	else
		scan_range(ex, item * sh->morsel, Min((item + 1) * sh->morsel, sh->nsb));
}

/*
 * The first item from `item` whose units the zones leave a unit in (nitems when none): an item of a morsel of superblocks that holds none costs its pass of
 * the loop of the scan, which an index of thousands of units pays once for each pair of them, whatever the zones keep (a lookup of one key of tens of
 * thousands: 2.4 million instructions at 15,638 units, 0.4 at 500).
 */
static uint32
next_item(AggExec *ex, uint32 item)
{
	const ScanShared *sh = ex->shared;
	uint32		sb;

	if (sh->ordered || part_flush_on(ex) || item >= sh->nitems)
		return item;
	sb = next_kept(ex, item * sh->morsel, 1);
	return sb >= sh->nsb ? sh->nitems : sb / sh->morsel;
}

bool
take_item(AggExec *ex, uint32 *item)
{
	*item = pg_atomic_fetch_add_u32(&ex->shared->next, 1);
	return *item < ex->shared->nitems;
}

/* take_item for the items of scan_items, which skips the ones without a kept unit */
static bool
take_kept_item(AggExec *ex, uint32 *item)
{
	pg_atomic_uint32 *next = &ex->shared->next;
	uint32		cur = pg_atomic_read_u32(next);

	do
	{
		*item = next_item(ex, cur);
		if (*item >= ex->shared->nitems)
			return false;
	} while (!pg_atomic_compare_exchange_u32(next, &cur, *item + 1));
	return true;
}

static void
item_done(AggExec *ex, uint32 item)
{
	/* the cells of the last row are groups of the item's table */
	arms_flush(ex);
	if (part_flush_on(ex))
		part_flush(ex);
	if (ex->lane_serial)
		lane_fold(ex);
	else if (ex->lanes)
		lane_keep(ex, item);
	if (ex->par)
		par_poll(ex);
}

bool		sift_test_par_leader_wait = false;

/* Testing: the leader takes one item and holds it until the workers have taken all the others, so that the scan is split between the participants whoever is quicker */
static void
test_leader_wait(AggExec *ex)
{
	for (int i = 0; sift_test_par_leader_wait && ex->par != NULL && i < 30000 && pg_atomic_read_u32(&ex->shared->next) < ex->shared->nitems; i++)
	{
		pg_usleep(1000);
		CHECK_FOR_INTERRUPTS();
	}
}

void
scan_items(AggExec *ex)
{
	uint32		item;

	/* the participants that split the groups by a key read all the items, each for its share of the keys */
	if (ex->gp_mine)
	{
		for (item = next_item(ex, 0); item < ex->shared->nitems && !ex->pass_stop; item = next_item(ex, item + 1))
		{
			scan_item(ex, item);
			item_done(ex, item);
		}
		return;
	}
	while (!ex->pass_stop && !sift_aggbe_stopped(ex->be, ex->be_state) && take_kept_item(ex, &item))
	{
		test_leader_wait(ex);
		scan_item(ex, item);
		item_done(ex, item);
	}
}
