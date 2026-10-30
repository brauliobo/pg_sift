/*
 * col/am/cache.c - the caches a column index keeps per relation: the zones, the leaf
 * statistics and the metadata of its segments
 * built on demand and dropped with the relation
 */
#include "postgres.h"
#include "access/table.h"
#include "commands/defrem.h"
#include "agg/stats.h"
#include "col/leafshm.h"
#include "core/limits.h"
#include "core/visibility.h"
#include "col/am/am_int.h"

/*
 * The leaf statistics of an index's immutable segments, memoized per backend: the key of an entry is the index and the generations of
 * its immutable segments, so a build, merge or promotion makes the next call read the zones again and nothing else does. A plan reads
 * every partition of a table (4096 at most for the pushdown), so the memo holds as many indexes, and is dropped whole when it grows past them.
 */
#define LEAF_STATS_CACHE	8192

/*
 * The planner needs the share of the units that hold each pre-aggregate section, not the count: reading the sections of every unit costs
 * a few buffers each (2.5 for an index of seven leaves and a cube), on the first plan of every backend. The units of an index are counted
 * up to PRE_CENSUS_UNITS together, shared out over its segments in proportion to their units and at least PRE_CENSUS_MIN of each, spread
 * evenly; an index of fewer units is counted whole.
 */
#define PRE_CENSUS_UNITS	256

#define PRE_CENSUS_MIN		2

typedef struct LeafEntry
{
	Oid			index;			/* hash key */
	uint64		stamp;
	uint64		checked;		/* the planning (leaf_plan) that validated the stamp */
	int			ncols;
	SiftColLeafStats *st;
	SiftColZones *zones;
	SiftColPreStats *pre;
}			LeafEntry;

static HTAB *leaf_cache;

static void
zones_free(SiftColZones *zs)
{
	for (int c = 0; c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
			pfree(zs->lo[c]), pfree(zs->hi[c]);
	pfree(zs->lo);
	pfree(zs);
}

/* the sample of the integer scalar leaves, in the memory that stays (TopMemoryContext) */
static SiftColZones *
zones_new(const SiftColIndexMeta *d)
{
	SiftColZones *zs = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColZones));

	zs->ncols = d->ncols;
	zs->stride = 1;
	zs->lo = MemoryContextAllocZero(TopMemoryContext, 2 * d->ncols * sizeof(int64 *));
	zs->hi = zs->lo + d->ncols;
	for (int c = 0; c < d->ncols; c++)
		if (d->cols[c].store == SIFT_COLS_INT && d->cols[c].kind != SIFT_COLK_GROUP)
		{
			zs->lo[c] = MemoryContextAlloc(TopMemoryContext, SIFT_COL_ZONE_SAMPLE * sizeof(int64));
			zs->hi[c] = MemoryContextAlloc(TopMemoryContext, SIFT_COL_ZONE_SAMPLE * sizeof(int64));
		}
	return zs;
}

/* a sample is filled once: the arrays keep the units sampled, not the room for all of them (an index of a few units is 16 bytes a leaf) */
static void
zones_shrink(SiftColZones *zs)
{
	for (int c = 0; c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
		{
			zs->lo[c] = repalloc(zs->lo[c], Max(zs->n, 1) * sizeof(int64));
			zs->hi[c] = repalloc(zs->hi[c], Max(zs->n, 1) * sizeof(int64));
		}
}

static LeafEntry *
leaf_cache_entry(Relation index, const SiftDirSnapshot *snap)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftDirSnapshot own;
	const SiftDirSnapshot *dir = snap;
	uint64		stamp = index->rd_locator.relNumber;	/* REINDEX starts the generations over */
	Oid			key = RelationGetRelid(index);
	LeafEntry  *e;

	if (am_leaf_plan != 0 && dir == NULL && leaf_cache != NULL)
	{
		e = hash_search(leaf_cache, &key, HASH_FIND, NULL);
		if (e != NULL && e->checked == am_leaf_plan && e->ncols == d->ncols)
			return e;
	}
	sift_stat_add(SIFT_STAT_LEAF_CHECKED, 1);
	stamp = stamp * 31 + sift_col_pagg_hash(index);	/* the sections of the units are counted against the options */
	if (dir == NULL)
	{
		sift_dir_read(index, &own);
		dir = &own;
	}
	for (int i = 0; i < dir->nsegs; i++)
		stamp = stamp * 31 + dir->segs[i].gen * 8 + dir->segs[i].format + 1;	/* a rewrite of a segment changes its format, and the kinds it keeps */
	if (leaf_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(LeafEntry), .hcxt = TopMemoryContext};

		leaf_cache = hash_create("sift_col leaf statistics", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(leaf_cache, &key, HASH_FIND, NULL);
	if (e != NULL && e->stamp == stamp && e->ncols == d->ncols)
	{
		e->checked = am_leaf_plan;
		return e;
	}
	if (e != NULL)
	{
		pfree(e->st);
		pfree(e->pre);
		zones_free(e->zones);
		hash_search(leaf_cache, &key, HASH_REMOVE, NULL);
	}
	else if (hash_get_num_entries(leaf_cache) >= LEAF_STATS_CACHE)
	{
		HASH_SEQ_STATUS seq;

		hash_seq_init(&seq, leaf_cache);
		while ((e = hash_seq_search(&seq)) != NULL)
		{
			pfree(e->st);
			pfree(e->pre);
			zones_free(e->zones);
			hash_search(leaf_cache, &e->index, HASH_REMOVE, NULL);
		}
	}
	{
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col leaf statistics", ALLOCSET_DEFAULT_SIZES);
		SiftColLeafStats *st = MemoryContextAlloc(TopMemoryContext, d->ncols * sizeof(SiftColLeafStats));
		SiftColPreStats *pre = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColPreStats));
		const SiftCPaggSpec *spec = &sift_col_rel(index)->pagg;
		SiftColZones *zs = zones_new(d);
		uint64		nunits = sift_cseg_dir_units(dir),
					census_units = sift_limit(pre_census_units, PRE_CENSUS_UNITS),
					shared_stamp;

		for (int c = 0; c < d->ncols; c++)
			sift_col_leaf_stats_init(&st[c]);
		/* the statistics of the segments are folded once for the server: the first backend that plans keeps them for the others (col/leafshm.c) */
		shared_stamp = stamp * 31 + census_units;
		if (sift_col_leafshm_read(index->rd_locator.relNumber, shared_stamp, d->ncols, st, pre, zs))
			sift_stat_add(SIFT_STAT_LEAF_SHARED, 1);
		else
		{
			for (int i = 0; i < dir->nsegs; i++)
			{
				SiftColSegment *seg;
				SiftCPaggCensus census;
				double		w;

				if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
					continue;
				seg = sift_cseg_open(index, &dir->segs[i], cx);
				for (int c = 0; c < d->ncols; c++)
					sift_cseg_leaf_stats(seg, c, &st[c]);
				sift_cseg_zone_sample(seg, zs);
				sift_cseg_pre_census(seg, spec, sift_col_format_minor, Max(PRE_CENSUS_MIN, (uint64) census_units * seg->nsb / Max(nunits, 1)), &census);
				/* the units counted stand for the units of the segment */
				w = census.units > 0 ? (double) seg->nsb / census.units : 0.0;
				pre->units += census.units * w;
				for (int k = 0; k < spec->ndims; k++)
				{
					pre->dim[spec->dim[k].leaf] += census.dim[k] * w;
					pre->mm[spec->dim[k].leaf] += census.mm[k] * w;
				}
				for (int k = 0; k < spec->nrollups; k++)
					if (spec->rollup[k].list >= 0)
						pre->roll[spec->rollup[k].list] += census.roll[k] * w;
					else
						pre->rows += census.roll[k] * w;
				sift_cseg_close(seg);
			}
			for (int i = 0; i < dir->nsegs; i++)
				if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE && dir->segs[i].format < SIFT_COL_FORMAT_KINDS)
					for (int c = 0; c < d->ncols; c++)
						st[c].nokinds |= sift_col_def_has_kinds(&d->cols[c]);
			sift_stat_add(SIFT_STAT_LEAF_BUILT, 1);
			sift_col_leafshm_write(index->rd_locator.relNumber, shared_stamp, d->ncols, st, pre, zs);
		}
		MemoryContextDelete(cx);
		zones_shrink(zs);
		e = hash_search(leaf_cache, &key, HASH_ENTER, NULL);
		e->stamp = stamp;
		e->checked = am_leaf_plan;
		e->ncols = d->ncols;
		e->st = st;
		e->zones = zs;
		e->pre = pre;
		return e;
	}
}

const SiftColPreStats *
sift_col_pre_stats(Relation index)
{
	return leaf_cache_entry(index, NULL)->pre;
}

const SiftColLeafStats *
sift_col_leaf_stats(Relation index)
{
	return leaf_cache_entry(index, NULL)->st;
}

const SiftColZones *
sift_col_zones(Relation index)
{
	return leaf_cache_entry(index, NULL)->zones;
}

SiftColIndexMeta *
sift_col_index_meta(Relation index, SiftColLeafMemo *memo)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftColIndexMeta *m = palloc(sizeof(SiftColIndexMeta));
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Relation	heap;
	BlockNumber relpages;

	*m = *d;
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			m->stats.nrows += e->ndocs;
			m->stats.bytes += e->bytes;
			m->stats.nsegments++;
		}
		else
			m->stats.nwbuf++;
	}
	m->stats.nunits = sift_cseg_dir_units(snap);
	m->stats.wbuf_ovf = sift_cwbuf_overflow(snap);
	m->stats.wbuf_jnull = sift_cwbuf_jnull(snap);
	heap = table_open(d->heapoid, AccessShareLock);
	relpages = RelationGetNumberOfBlocks(heap);
	m->stats.heap_pages = relpages;
	m->stats.allvisible_frac = relpages > 0 ? (double) sift_vis_count_allvisible(heap, relpages) / relpages : 0;
	table_close(heap, AccessShareLock);
	if (memo != NULL)
	{
		LeafEntry  *e = leaf_cache_entry(index, snap);

		memo->leaves = e->st;
		memo->zones = e->zones;
		memo->pre = e->pre;
	}
	pfree(snap);
	return m;
}

bool
sift_col_is_index(Relation index)
{
	return index->rd_rel->relam == sift_col_am_oid();
}

Oid
sift_col_am_oid(void)
{
	static Oid	am = InvalidOid;

	if (!OidIsValid(am))
		am = get_index_am_oid("sift_col", true);
	return am;
}
