/*
 * table/f2/col_delta/mapcache.c - the map of the patches of a whole part, kept from one statement to the next (f2_col_delta.h).
 *
 * A slot is made for the attributes of the first ask: the pages of the chain that patch none of them are not read, and the slot remembers the attributes of those pages. An ask whose attributes meet
 * them makes the slot over, for the attributes of both; every other ask, whatever its attributes, is answered by the slot with one look at the newest page of the chain.
 *
 * A map is what a snapshot sees of the records of one part: it depends on the snapshot only through the writers whose fate the snapshot decides. A record whose writer finished before the
 * oldest transaction the snapshot can see (or, for a snapshot that takes any row, whose writer is not running) is committed for every later snapshot or never visible to any, so a map
 * made of such records alone is the map of the next statement too, and a statement that appended records to the chain only adds them: pages are only ever added at the newest end of a
 * chain, and the page that grows is compared by what it held. The records the map covers and the pages it was read from are what the slot remembers; a chain that was folded or rewritten is made
 * of pages of other generations, and the slot goes. A snapshot that cannot tell the writer of a new record yet (it is running, or it is newer than the snapshot) is not answered: its statement
 * builds the map of its own, as it did before the cache.
 *
 * check-memory: allow-allocs a slot holds the block list of one chain and its last page, and the budget of core/cachebudget.h bounds the slots and the maps they keep
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "core/f2/col_delta_anchor.h"
#include "core/f2/meta.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/tbl.h"

#define MAP_SLOTS		128
#define ALL_ATTRS		(~UINT64CONST(0))

typedef struct MapSlot
{
	MemoryContext cx;			/* NULL: free; holds blks and head */
	RelFileLocator loc;
	Oid			relid;
	uint32		relhash;
	uint64		part_id;
	uint64		part_gen;
	SiftColDeltaPartMap *map;	/* the slot's reference; NULL: the part has no patch the records show */
	SiftColDeltaWatch watch;	/* the pages of the chain the map covers, the newest as far as its copy says */
	SiftColDeltaAnchor anchor;	/* the chain the watch covers as the manifest named it */
	uint64		built_bloom;	/* the attributes the pages were chosen for; ALL_ATTRS: every page */
	uint64		skipped_bloom;	/* the attributes of the pages left out */
	TransactionId max_xmin;		/* the newest writer of a record the map covers */
	bool		has_xmin;
	bool		declined;		/* the chain as it ended at declined_pages showed a writer the snapshot of declined_xmin could not settle */
	int			declined_pages;
	TransactionId declined_xmin;
	uint64		tick;
} MapSlot;

static MemoryContext mapcache_cx = NULL;
static MapSlot slots[MAP_SLOTS];

/* ---- which writers the cache can answer for ---- */

static bool
snapshot_supported(const SiftTblVis *vis)
{
	SnapshotType t = vis->snap->snapshot_type;

	return t == SNAPSHOT_MVCC || t == SNAPSHOT_ANY || t == SNAPSHOT_DIRTY;
}

/* Whether the fate of the writer is settled for the snapshot and every later one: committed, or never visible */
static bool
settled_xid(const SiftTblVis *vis, TransactionId xmin)
{
	if (vis->snap->snapshot_type == SNAPSHOT_MVCC)
		return TransactionIdPrecedes(xmin, vis->snap->xmin);
	return !TransactionIdIsCurrentTransactionId(xmin) && !TransactionIdIsInProgress(xmin);
}

typedef struct Settled
{
	SiftTblVis *vis;
	TransactionId last;
	bool		last_settled;
	bool		unsettled;
	TransactionId newest;
	bool		any;
} Settled;

static bool
settled_visible(void *ctx, TransactionId xmin, CommandId cid)
{
	Settled    *s = ctx;

	if (!TransactionIdIsNormal(xmin))
		return sift_tbl_f2_delta_visible(s->vis, xmin, cid);
	if (xmin != s->last)
	{
		s->last = xmin;
		s->last_settled = settled_xid(s->vis, xmin);
	}
	if (!s->last_settled)
	{
		s->unsettled = true;
		return false;
	}
	if (!sift_tbl_f2_delta_visible(s->vis, xmin, cid))
		return false;
	if (!s->any || TransactionIdFollows(xmin, s->newest))
		s->newest = xmin;
	s->any = true;
	return true;
}

/* Whether the records the slot covers are settled for the snapshot too: a snapshot that is older than the newest of their writers would not see them all */
static bool
slot_settled_for(const MapSlot *s, const SiftTblVis *vis)
{
	return !s->has_xmin || vis->snap->snapshot_type != SNAPSHOT_MVCC || TransactionIdPrecedes(s->max_xmin, vis->snap->xmin);
}

/* ---- the slots ---- */

static void
slot_drop(MapSlot *s)
{
	sift_col_delta_part_map_unref(s->map);
	if (s->cx != NULL)
		MemoryContextDelete(s->cx);
	memset(s, 0, sizeof(*s));
}

static uint64
mapcache_oldest(void)
{
	uint64		oldest = UINT64_MAX;

	for (int i = 0; i < MAP_SLOTS; i++)
		if (slots[i].cx != NULL && !sift_cache_used_in_xact(slots[i].tick))
			oldest = Min(oldest, slots[i].tick);
	return oldest;
}

static bool
mapcache_evict_one(void)
{
	MapSlot    *lru = NULL;

	for (int i = 0; i < MAP_SLOTS; i++)
		if (slots[i].cx != NULL && !sift_cache_used_in_xact(slots[i].tick) && (lru == NULL || slots[i].tick < lru->tick))
			lru = &slots[i];
	if (lru == NULL)
		return false;
	slot_drop(lru);
	return true;
}

static void
mapcache_evict_all(void)
{
	for (int i = 0; i < MAP_SLOTS; i++)
		if (slots[i].cx != NULL && !sift_cache_used_in_xact(slots[i].tick))
			slot_drop(&slots[i]);
}

static const SiftCacheKind mapcache_kind = {"table_f2_col_delta_map_cache", mapcache_oldest, mapcache_evict_one, mapcache_evict_all};

static void
mapcache_invalidate(uint32 hashvalue)
{
	for (int i = 0; i < MAP_SLOTS; i++)
		if (slots[i].cx != NULL && sift_f2_inval_hits(slots[i].relhash, hashvalue))
			slot_drop(&slots[i]);
}

static void
mapcache_init(void)
{
	mapcache_cx = sift_cache_create_context("sift cache f2 column delta map slots");
	sift_cache_register(&mapcache_kind);
	sift_f2_invalidator_register(mapcache_invalidate);
}

static MapSlot *
slot_find(Relation rel, uint64 part_id, uint64 part_gen)
{
	for (int i = 0; i < MAP_SLOTS; i++)
		if (slots[i].cx != NULL && slots[i].part_id == part_id && slots[i].part_gen == part_gen && RelFileLocatorEquals(slots[i].loc, rel->rd_locator))
			return &slots[i];
	return NULL;
}

static MapSlot *
slot_open(Relation rel, uint64 part_id, uint64 part_gen, uint64 want)
{
	MapSlot    *s = &slots[0];

	for (int i = 0; i < MAP_SLOTS; i++)
	{
		if (slots[i].cx == NULL)
		{
			s = &slots[i];
			break;
		}
		if (slots[i].tick < s->tick)
			s = &slots[i];
	}
	slot_drop(s);
	s->cx = sift_mem_allocset_create(mapcache_cx, "sift cache f2 column delta map slot");
	s->loc = rel->rd_locator;
	s->relid = RelationGetRelid(rel);
	s->relhash = sift_f2_rel_hash(rel);
	s->part_id = part_id;
	s->part_gen = part_gen;
	s->built_bloom = want;
	sift_col_delta_watch_init(&s->watch, s->cx);
	return s;
}

/* The slot made over for the attributes of both its first ask and this one: no map, no page seen */
static void
slot_widen(MapSlot *s, uint64 want)
{
	sift_col_delta_part_map_unref(s->map);
	s->map = NULL;
	sift_col_delta_watch_reset(&s->watch);
	s->built_bloom |= want;
	s->skipped_bloom = 0;
	s->max_xmin = InvalidTransactionId;
	s->has_xmin = false;
	s->declined = false;
}

/* ---- the chain the slot covers, and what it gained ---- */

typedef enum Advance
{
	ADVANCED,
	UNSETTLED,					/* a writer of a new record is not settled for the snapshot: the slot is as it was */
	STALE						/* the chain is not the slot's with pages added */
} Advance;

typedef struct News
{
	SiftColDeltaCollector *c;
	SiftColDeltaOps ops;
	uint64		part_gen;
	uint32		ngranules;
} News;

static bool
collect_news(void *arg, const char *page, uint32 from_used)
{
	News	   *n = arg;
	const char *why = NULL;

	return sift_col_delta_collect_page(n->c, page, from_used, n->part_gen, n->ngranules, &n->ops, &why);
}

static void
slot_cover(MapSlot *s, const SiftColDeltaAnchor *a, const SiftColDeltaWatchRead *r, SiftColDeltaPartMap *map, const Settled *st)
{
	sift_col_delta_part_map_unref(s->map);
	s->map = map;
	sift_col_delta_watch_set(&s->watch, a, r);
	s->anchor = *a;
	s->skipped_bloom |= r->skipped_bloom;
	if (st->any && (!s->has_xmin || TransactionIdFollows(st->newest, s->max_xmin)))
		s->max_xmin = st->newest;
	s->has_xmin |= st->any;
	s->declined = false;
}

/* The map of the slot with the new records over it, in a context of its own that the cache adopts; the slot's own when there are none */
static SiftColDeltaPartMap *
map_with(MapSlot *s, SiftColDeltaCollector *c, uint32 ngranules)
{
	MemoryContext map_cx;
	MemoryContext old;
	SiftColDeltaPartMap *map;

	if (sift_col_delta_collected(c) == 0)
		return sift_col_delta_part_map_ref(s->map);
	map_cx = sift_col_delta_part_map_context(CurrentMemoryContext);
	old = MemoryContextSwitchTo(map_cx);
	map = sift_col_delta_part_map_merge(s->map, c, s->part_gen, ngranules);
	MemoryContextSwitchTo(old);
	return sift_col_delta_part_map_share(map_cx, map);
}

/* The slot made to cover the chain of the anchor: its map the old one with the new records over it */
static Advance
slot_advance(Relation rel, const SiftPartitionEntry *entry, MapSlot *s, uint32 ngranules, const SiftColDeltaAnchor *a, SiftTblVis *vis)
{
	Settled		st = {.vis = vis};
	News		news = {.c = sift_col_delta_collector_new(),.part_gen = s->part_gen,.ngranules = ngranules};
	SiftColDeltaPrune prune = {.part_gen = s->part_gen,.attnum_bloom = s->built_bloom == ALL_ATTRS ? 0 : s->built_bloom | sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP)};
	SiftColDeltaWatchRead r;
	Advance		adv = STALE;

	sift_col_delta_ops_make(&news.ops, &st, settled_visible);
	if (sift_col_delta_watch_read(rel, entry, a, &s->watch, &prune, collect_news, &news, &r))
	{
		adv = st.unsettled ? UNSETTLED : ADVANCED;
		if (adv == ADVANCED)
			slot_cover(s, a, &r, map_with(s, news.c, ngranules), &st);
		else if (vis->snap->snapshot_type == SNAPSHOT_MVCC)
		{
			s->declined = true;
			s->declined_pages = a->npages;
			s->declined_xmin = vis->snap->xmin;
		}
		sift_col_delta_watch_release(&r);
	}
	sift_col_delta_collector_free(news.c);
	return adv;
}

/* ---- the ask ---- */

typedef struct HeadStamp
{
	BlockNumber blk;
	uint64		gen;
	XLogRecPtr	lsn;
	uint32		used;
	uint32		crc;
} HeadStamp;

static HeadStamp
stamp_of(const char *page, BlockNumber blk)
{
	return (HeadStamp) {blk, SiftPageGetOpaque((Page) page)->gen, PageGetLSN((Page) page), SiftPageGetUsed((Page) page), *(const uint32 *) (SiftPageGetData((Page) page) + SIFT_PAGE_CRC_OFF)};
}

/* The newest page of the chain as the scan looked at it first: a scan asks for each part of a partition in turn, and its snapshot sees no record written after its first look */
static SiftColDeltaLook *
look_of(SiftColDeltaLooks *looks, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a, BlockNumber blk)
{
	for (int i = 0; i < SIFT_COL_DELTA_LOOKS; i++)
		if (looks->v[i].set && looks->v[i].part_id == entry->part_id && looks->v[i].blk == blk && memcmp(&looks->v[i].anchor, a, sizeof(*a)) == 0)
			return &looks->v[i];
	return NULL;
}

static HeadStamp
head_now(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a, const SiftTblVis *vis, SiftColDeltaLooks *looks, BlockNumber blk)
{
	bool		scoped = looks != NULL && vis->snap->snapshot_type == SNAPSHOT_MVCC;
	SiftColDeltaLook *l = scoped ? look_of(looks, entry, a, blk) : NULL;
	Buffer		buf;
	HeadStamp	st;

	if (l != NULL)
		return (HeadStamp) {l->blk, l->gen, l->lsn, l->used, l->crc};
	buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	st = stamp_of(BufferGetPage(buf), blk);
	UnlockReleaseBuffer(buf);
	if (scoped)
	{
		looks->v[looks->next] = (SiftColDeltaLook) {.set = true,.part_id = entry->part_id,.anchor = *a,.blk = blk,.gen = st.gen,.lsn = st.lsn,.used = st.used,.crc = st.crc};
		looks->next = (looks->next + 1) % SIFT_COL_DELTA_LOOKS;
	}
	return st;
}

/* The newest page of the chain is the one the slot saw, with the records it held and no more: with the same anchor, the chain is the slot's */
static bool
chain_unchanged(Relation rel, const SiftPartitionEntry *entry, const MapSlot *s, const SiftColDeltaAnchor *a, const SiftTblVis *vis, SiftColDeltaLooks *looks)
{
	HeadStamp	seen,
				now;

	if (s->watch.npages == 0 || memcmp(&s->anchor, a, sizeof(*a)) != 0)
		return false;
	seen = (HeadStamp) {s->watch.blks[s->watch.npages - 1], s->watch.head.gen, s->watch.head.lsn, s->watch.head.used, s->watch.head.crc};
	now = head_now(rel, entry, a, vis, looks, seen.blk);
	return seen.gen == now.gen && seen.lsn == now.lsn && seen.used == now.used && seen.crc == now.crc;
}

bool
sift_col_delta_mapcache_get(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 attnum_bloom, SiftColDeltaLooks *looks, SiftColDeltaPartMap **map)
{
	SiftColDeltaAnchor a;
	MapSlot    *s;
	uint64		want = attnum_bloom == 0 ? ALL_ATTRS : attnum_bloom;
	Advance		r = ADVANCED;

	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 || RecoveryInProgress() || !snapshot_supported(vis))
		return false;
	a = sift_manifest_read_col_delta(rel, entry);
	if (!sift_col_delta_anchor_sane(&a))
		return false;
	if (mapcache_cx == NULL)
		mapcache_init();
	s = slot_find(rel, entry->part_id, part_gen);
	if (s != NULL && (s->watch.npages > a.npages || !slot_settled_for(s, vis)))
	{
		if (s->watch.npages > a.npages)
			slot_drop(s);
		return false;
	}
	if (s != NULL && s->declined && s->declined_pages == a.npages && vis->snap->snapshot_type == SNAPSHOT_MVCC && !TransactionIdFollows(vis->snap->xmin, s->declined_xmin))
		return false;
	if (s != NULL && (want & s->skipped_bloom) != 0)
		slot_widen(s, want);
	if (s == NULL)
		s = slot_open(rel, entry->part_id, part_gen, want);
	if (!chain_unchanged(rel, entry, s, &a, vis, looks))
		r = slot_advance(rel, entry, s, ngranules, &a, vis);
	if (r == STALE && s->watch.npages > 0)
	{
		want = s->built_bloom;
		slot_drop(s);
		s = slot_open(rel, entry->part_id, part_gen, want);
		r = slot_advance(rel, entry, s, ngranules, &a, vis);
	}
	if (r != ADVANCED)
	{
		if (r == STALE || (s->watch.npages == 0 && !s->declined))
			slot_drop(s);
		return false;
	}
	s->tick = sift_cache_tick();
	*map = sift_col_delta_part_map_ref(s->map);
	if ((s->map != NULL && MemoryContextMemAllocated(s->map->cx, true) > (Size) sift_backend_cache_kb * 1024 / 2) || !sift_cache_commit())
		slot_drop(s);
	return true;
}
