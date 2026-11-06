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
 * a few buffers each (2.5 for an index of seven leaves and a cube), on the first plan of every backend. Up to PRE_CENSUS_UNITS units of
 * each segment are counted, spread evenly; a segment of fewer units is counted whole. The count of a segment is a function of the segment
 * alone, so that the statistics of an index are the same whether a backend folded all its segments or only those a record of the shared
 * ring (col/leafshm.c) did not cover.
 */
#define PRE_CENSUS_UNITS	16

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

/* the state a fold starts from */
static void
fold_reset(int ncols, SiftColLeafStats *st, SiftColPreStats *pre, SiftColZones *zs)
{
	for (int c = 0; c < ncols; c++)
		sift_col_leaf_stats_init(&st[c]);
	memset(pre, 0, sizeof(*pre));
	zs->n = 0;
	zs->units = 0;
	zs->stride = 1;
}

/*
 * The statistics of the immutable segments of the directory, folded into st, pre and zs (reset here): the sections of each segment, in the order of the directory. The fold
 * is the same whichever segments a record of the shared ring covers: it stands for the first k, and only the others are opened. fs[j] is the stamp of the first j
 * immutable segments (the index, its options and what each segment's generation and format are); the record is under the stamp of all of them, and the one of the index, under
 * any older stamp, is taken as the first k segments when that stamp is that of a prefix of the directory: a promotion adds a segment and the next backend that plans reads
 * its sections, not those of the segments it has seen. shared false folds them all and neither reads nor writes the ring. *folded: the segments opened.
 */
static void
leaf_fold(Relation index, const SiftDirSnapshot *dir, SiftColLeafStats *st, SiftColPreStats *pre, SiftColZones *zs, bool shared, int *folded)
{
	const int	ncols = sift_col_index_defs(index)->ncols;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col leaf statistics", ALLOCSET_DEFAULT_SIZES);
	const SiftColRel *rel = sift_col_rel_private(index, cx);	/* the loops open segments, which read catalogs and reload the cached state */
	const SiftColIndexMeta *d = &rel->meta;
	const SiftCPaggSpec *spec = &rel->pagg;
	uint64	   *fs = palloc((dir->nsegs + 1) * sizeof(uint64));
	int			nimm = 0,
				k0 = 0,
				nfold = 0;
	uint64		prefix;
	bool		whole;

	fs[0] = (uint64) index->rd_locator.relNumber * 31 + sift_col_pagg_hash(index);	/* the sections of the units are counted against the options */
	fs[0] = fs[0] * 31 + sift_limit(pre_census_units, PRE_CENSUS_UNITS);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			fs[nimm + 1] = fs[nimm] * 31 + dir->segs[i].gen * 8 + dir->segs[i].format + 1;	/* a rewrite of a segment changes its format, and the kinds it keeps */
			nimm++;
		}
	fold_reset(ncols, st, pre, zs);
	/* the statistics of the segments are folded once for the server: the first backend that plans keeps them for the others (col/leafshm.c) */
	whole = shared && sift_col_leafshm_read(index->rd_locator.relNumber, fs[nimm], ncols, st, pre, zs);
	if (whole)
	{
		sift_stat_add(SIFT_STAT_LEAF_SHARED, 1);
		k0 = nimm;
	}
	else if (shared && sift_col_leafshm_peek(index->rd_locator.relNumber, &prefix))
	{
		int			k = nimm - 1;

		while (k > 0 && fs[k] != prefix)
			k--;
		if (k > 0 && sift_col_leafshm_read(index->rd_locator.relNumber, prefix, ncols, st, pre, zs))
			k0 = k;
		else
			fold_reset(ncols, st, pre, zs);
	}
	if (!whole)
	{
		int			j = 0;

		for (int i = 0; i < dir->nsegs; i++)
		{
			SiftColSegment *seg;
			SiftCPaggCensus census;
			double		w;

			if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || j++ < k0)
				continue;
			seg = sift_cseg_open(index, &dir->segs[i], cx);
			for (int c = 0; c < ncols; c++)
				sift_cseg_leaf_stats(seg, c, &st[c]);
			sift_cseg_zone_sample(seg, zs);
			sift_cseg_pre_census(seg, spec, sift_col_format_minor, sift_limit(pre_census_units, PRE_CENSUS_UNITS), &census);
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
			nfold++;
		}
		sift_stat_add(SIFT_STAT_LEAF_BUILT, 1);
		if (shared)
			sift_col_leafshm_write(index->rd_locator.relNumber, fs[nimm], ncols, st, pre, zs);
	}
	/* the share of the rows in entries that do not say their string roots are flagged (SIFT_SEG_ROOTS): the test doc ? 'k' reads their NULL slots again from the heap */
	{
		double		total = 0,
					rootless = 0;

		for (int i = 0; i < dir->nsegs; i++)
			if (dir->segs[i].kind != SIFT_SEG_TOMB)
			{
				total += dir->segs[i].ndocs;
				if (!(dir->segs[i].flags & SIFT_SEG_ROOTS))
					rootless += dir->segs[i].ndocs;
			}
		for (int c = 0; c < ncols; c++)
			st[c].rootless = sift_col_def_has_kinds(&d->cols[c]) && total > 0 ? rootless / total : 0;
	}
	/* what the segments that are not immutable (a write buffer, a segment of an older format) say is not in the record: it is of the directory in hand */
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE && dir->segs[i].kind != SIFT_SEG_TOMB && dir->segs[i].format < SIFT_COL_FORMAT_KINDS)
			for (int c = 0; c < ncols; c++)
				st[c].nokinds |= sift_col_def_has_kinds(&d->cols[c]);
	MemoryContextDelete(cx);
	zones_shrink(zs);
	if (folded != NULL)
		*folded = nfold;
}

static LeafEntry *
leaf_cache_entry(Relation index, const SiftDirSnapshot *snap)
{
	const int	ncols = sift_col_index_defs(index)->ncols;	/* the cached state is freed by a reload: nothing of it is held across a read of the directory or a segment */
	SiftDirSnapshot own;
	const SiftDirSnapshot *dir = snap;
	uint64		stamp = index->rd_locator.relNumber;	/* REINDEX starts the generations over */
	Oid			key = RelationGetRelid(index);
	LeafEntry  *e;

	if (am_leaf_plan != 0 && dir == NULL && leaf_cache != NULL)
	{
		e = hash_search(leaf_cache, &key, HASH_FIND, NULL);
		if (e != NULL && e->checked == am_leaf_plan && e->ncols == ncols)
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
		stamp = stamp * 31 + dir->segs[i].gen * 8 + dir->segs[i].format + 1 + ((dir->segs[i].flags & SIFT_SEG_ROOTS) ? 1000003 : 0);	/* a rewrite of a segment changes its format, and the kinds it keeps */
	if (leaf_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(LeafEntry), .hcxt = TopMemoryContext};

		leaf_cache = hash_create("sift_col leaf statistics", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(leaf_cache, &key, HASH_FIND, NULL);
	if (e != NULL && e->stamp == stamp && e->ncols == ncols)
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
		SiftColLeafStats *st = MemoryContextAllocZero(TopMemoryContext, ncols * sizeof(SiftColLeafStats));
		SiftColPreStats *pre = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColPreStats));
		SiftColZones *zs = zones_new(sift_col_index_defs(index));

		leaf_fold(index, dir, st, pre, zs, true, NULL);
		e = hash_search(leaf_cache, &key, HASH_ENTER, NULL);
		e->stamp = stamp;
		e->checked = am_leaf_plan;
		e->ncols = ncols;
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
	BlockNumber relpages,
				visible,
				clean;

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
			if (sift_cseg_keyed(e))
				m->stats.keyed_frac += e->ndocs;
		}
		else if (e->kind != SIFT_SEG_TOMB)
			m->stats.nwbuf++;
	}
	if (m->stats.nrows > 0)
		m->stats.keyed_frac /= m->stats.nrows;
	m->stats.nunits = sift_cseg_dir_units(snap);
	m->stats.wbuf_ovf = sift_cwbuf_overflow(snap);
	m->stats.wbuf_jnull = sift_cwbuf_jnull(snap);
	heap = table_open(d->heapoid, AccessShareLock);
	relpages = RelationGetNumberOfBlocks(heap);
	m->stats.heap_pages = relpages;
	sift_vis_count(heap, relpages, &visible, &clean);
	m->stats.allvisible_frac = relpages > 0 ? (double) visible / relpages : 0;
	m->stats.clean_frac = relpages > 0 ? (double) clean / ((relpages + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES) : 0;
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

#ifdef SIFT_TESTING
static bool
zones_equal(const SiftColZones *a, const SiftColZones *b)
{
	if (a->ncols != b->ncols || a->n != b->n || a->units != b->units || a->stride != b->stride)
		return false;
	for (int c = 0; c < a->ncols; c++)
		if ((a->lo[c] == NULL) != (b->lo[c] == NULL) ||
			(a->lo[c] != NULL && (memcmp(a->lo[c], b->lo[c], a->n * sizeof(int64)) != 0 || memcmp(a->hi[c], b->hi[c], a->n * sizeof(int64)) != 0)))
			return false;
	return true;
}

bool
sift_col_leaf_fold_check(Relation index, int *incremental, int *full)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftColLeafStats *st[2];
	SiftColPreStats *pre[2];
	SiftColZones *zs[2];
	bool		same;

	sift_dir_read(index, dir);
	for (int k = 0; k < 2; k++)
	{
		st[k] = MemoryContextAllocZero(TopMemoryContext, d->ncols * sizeof(SiftColLeafStats));
		pre[k] = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColPreStats));
		zs[k] = zones_new(d);
	}
	leaf_fold(index, dir, st[0], pre[0], zs[0], true, incremental);
	leaf_fold(index, dir, st[1], pre[1], zs[1], false, full);
	same = memcmp(st[0], st[1], d->ncols * sizeof(SiftColLeafStats)) == 0 && memcmp(pre[0], pre[1], sizeof(SiftColPreStats)) == 0 && zones_equal(zs[0], zs[1]);
	for (int k = 0; k < 2; k++)
	{
		pfree(st[k]);
		pfree(pre[k]);
		zones_free(zs[k]);
	}
	pfree(dir);
	return same;
}
#endif							/* SIFT_TESTING */
