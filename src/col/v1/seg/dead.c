/*
 * col/v1/seg/dead.c - the heap addresses of the dead rows of a unit of a segment, kept for the next scan of the backend
 */
#include "postgres.h"
#include "miscadmin.h"
#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "col/am.h"
#include "col/v1/seg/int.h"
#include "core/cachebudget.h"

/*
 * A scan that skips entries of the global order (an OFFSET) asks of each whether its row is dead, which takes the list of the dead rows of its unit: the head of the unit and
 * the offsets of its pages, about 6 instructions a dead row and a block a unit in 30. The list is kept by (index, segment, unit) with the count of cleared rows the segment had when it was
 * made, and used while the segment has no more:
 * - rows are cleared only by sift_live_clear, which sets the bits of a unit and adds to the count in one WAL record, so the count only grows and equal counts are the same bits;
 * - the count is read before the list is made, so a clear that came between the two leaves a count lower than the bits, and the next scan makes the list again;
 * - a merge or a rewrite makes new segments with new generations, and the lists of the old ones are never asked for again (they leave with the others when the bytes pass the bound);
 * - the all-visible map is not kept: a scan reads it before it asks for the list, as it does the bits.
 */
#ifdef SIFT_TESTING
uint64		sift_col_gdead_stat[3];
#define DEAD_STAT(i)	(sift_col_gdead_stat[i]++)
#else
#define DEAD_STAT(i)	((void) 0)
#endif

typedef struct DeadKey
{
	Oid			db;
	Oid			rel;
	uint64		gen;
	uint32		unit;
	uint32		pad;
} DeadKey;

typedef struct DeadEnt
{
	DeadKey		key;
	uint32		stamp;
	uint32		n;
	SiftKey    *dead;
	MemoryContext cx;
	uint64		tick;
	dlist_node	lru;			/* dead_lru: the lists in the order of their last use, the oldest first */
} DeadEnt;

static MemoryContext dead_cxt;
static HTAB *dead_tab;
static dlist_head dead_lru = DLIST_STATIC_INIT(dead_lru);

static DeadEnt *
dead_victim(void)
{
	return dlist_is_empty(&dead_lru) ? NULL : dlist_head_element(DeadEnt, lru, &dead_lru);
}

static uint64
dead_oldest(void)
{
	DeadEnt    *e = dead_victim();

	return e ? e->tick : UINT64_MAX;
}

static void
dead_drop(DeadEnt *e)
{
	DeadKey		key = e->key;

	dlist_delete(&e->lru);
	MemoryContextDelete(e->cx);
	hash_search(dead_tab, &key, HASH_REMOVE, NULL);
}

static bool
dead_evict_one(void)
{
	DeadEnt    *e = dead_victim();

	if (e == NULL)
		return false;
	dead_drop(e);
	return true;
}

static void
dead_evict_all(void)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &dead_lru)
		dead_drop(dlist_container(DeadEnt, lru, it.cur));
}

static const SiftCacheKind dead_kind = {"col_dead_cxt", dead_oldest, dead_evict_one, dead_evict_all};

static void
dead_put(const DeadKey *key, uint32 stamp, const SiftKey *dead, uint32 n)
{
	DeadEnt    *e;
	MemoryContext cx;

	if (dead_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(DeadKey), .entrysize = sizeof(DeadEnt)};

		dead_cxt = sift_cache_create_context("sift cache dead rows");
		ctl.hcxt = dead_cxt;
		dead_tab = hash_create("sift cache dead rows", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		sift_cache_register(&dead_kind);
	}
	if ((e = hash_search(dead_tab, key, HASH_FIND, NULL)) != NULL)
		dead_drop(e);
	if (!sift_cache_room((Size) n * sizeof(SiftKey) + 4096))
		return;
	cx = AllocSetContextCreate(dead_cxt, "sift cache dead rows entry", ALLOCSET_SMALL_SIZES);
	e = hash_search(dead_tab, key, HASH_ENTER, NULL);
	e->stamp = stamp;
	e->n = n;
	e->cx = cx;
	e->tick = sift_cache_tick();
	dlist_push_tail(&dead_lru, &e->lru);
	e->dead = MemoryContextAlloc(cx, Max(n, 1) * sizeof(SiftKey));
	memcpy(e->dead, dead, n * sizeof(SiftKey));
}

bool
sift_cseg_dead_fits(SiftColSegment *seg)
{
	return (uint64) sift_live_ndead(&PRIV(seg)->live) * sizeof(SiftKey) <= (uint64) work_mem * 1024;
}

/*
 * Appends the heap addresses of the dead rows of unit u to *dead (grown in mcxt, *room entries, *ndead used), in the order of the unit's rows; false when the unit cannot be
 * viewed.
 */
bool
sift_cseg_unit_dead(SiftColSegment *seg, uint32 u, MemoryContext mcxt, SiftKey **dead, uint32 *ndead, uint32 *room)
{
	SiftCUnitView v;
	DeadKey		key = {seg->index->rd_locator.dbOid, seg->index->rd_locator.relNumber, seg->entry.gen, u, 0};
	DeadEnt    *e = NULL;
	bool		scan_bound;
	uint32		stamp,
				n = seg->sbdir[u].nrows,
				nd = 0,
			   *ords = NULL;
	uint64	   *live = NULL;

	if (!unit_dirty(seg, u))
		return true;
	stamp = sift_live_ndead(&PRIV(seg)->live);
	/* the records of a scan are of its snapshot: a list made with them is for that scan alone, and one made without them is not for it */
	scan_bound = seg->dv != NULL && seg->dv->unit(seg->dv, seg, u) != NULL;
	if (dead_tab && !scan_bound)
		e = hash_search(dead_tab, &key, HASH_FIND, NULL);
	if (e && e->stamp != stamp)
		e = NULL;
	if (e)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&dead_lru, &e->lru);
	}
	if (e == NULL && !sift_cseg_unit_view(seg, u, &v))
		return false;
	if (e)
		nd = e->n;
	else
	{
		ords = palloc(n * sizeof(uint32));
		live = palloc(SIFT_COL_NWORDS(n) * sizeof(uint64));
		sift_cseg_live(seg, u, live);
		for (uint32 w = 0; w < (n + 63) / 64; w++)
			for (uint64 b = ~live[w] & (w == n / 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0)); b; b &= b - 1)
				ords[nd++] = w * 64 + pg_rightmost_one_pos64(b);
	}
	if (*ndead + nd > *room)
	{
		*room = Max(*room * 2, *ndead + nd);
		SIFT_REGROW_HUGE_IN(mcxt, *dead, *room * sizeof(SiftKey));
	}
	if (e)
	{
		memcpy(*dead + *ndead, e->dead, nd * sizeof(SiftKey));
		DEAD_STAT(0);
	}
	else
	{
		sift_cunit_row_keys(&v, v.keyed ? 0 : sift_cseg_unit_sb(seg, u), ords, nd, *dead + *ndead);
		dead_put(&key, stamp, *dead + *ndead, nd);
		pfree(ords);
		pfree(live);
		DEAD_STAT(1);
	}
	*ndead += nd;
	return true;
}
