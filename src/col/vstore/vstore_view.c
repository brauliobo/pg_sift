/*
 * col/vstore/vstore_view.c - the view a backend keeps of each shared value store it read, under the budget of sift.backend_cache_kb (core/cachebudget.h)
 *
 * A view keeps every string of its store. One that no open handle pins is dropped, least recently used first, when another cache needs room, and read again by
 * the next statement that needs it; one in use is never dropped.
 */
#include "postgres.h"

#include "access/table.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "core/cachebudget.h"
#include "col/seg_int.h"
#include "col/vstore/vstore.h"
#include "col/vstore/vstore_int.h"

static HTAB *views;
static bool stores_changed;

static void
stores_relcache_changed(Datum arg, int cacheid, uint32 hashvalue)
{
	stores_changed = true;
}

/* The views of the stores that were dropped: a store is one relation, and its view holds the strings of all its codes */
static void
views_drop_gone(Oid keep)
{
	HASH_SEQ_STATUS seq;
	VStoreView *v;

	stores_changed = false;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		if (v->store != keep && !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(v->store)))
		{
			MemoryContextDelete(v->cx);
			hash_search(views, &v->store, HASH_REMOVE, NULL);
		}
}

static void
view_clear(VStoreView *v)
{
	MemoryContextReset(v->cx);
	v->slots = NULL;
	v->nslots = 0;
	v->indexed = 0;
	v->piece = NULL;
	v->npiece = v->cap = 0;
	v->total = 0;
	v->loaded = false;
	v->epoch = 0;
}

static Size
views_bytes(void)
{
	HASH_SEQ_STATUS seq;
	VStoreView *v;
	Size		total = 0;

	if (views == NULL)
		return 0;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		total += MemoryContextMemAllocated(v->cx, true);
	return total;
}

static VStoreView *
views_victim(void)
{
	HASH_SEQ_STATUS seq;
	VStoreView *v,
			   *lru = NULL;

	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		if (v->pins == 0 && v->loaded && (lru == NULL || v->used < lru->used))
			lru = v;
	return lru;
}

static uint64
views_oldest(void)
{
	VStoreView *v = views_victim();

	return v ? v->used : UINT64_MAX;
}

static void
views_evict_oldest(void)
{
	view_clear(views_victim());
}

static const SiftCacheKind views_kind = {views_bytes, views_oldest, views_evict_oldest};

static void
view_unpin(Oid store)
{
	VStoreView *v = hash_search(views, &store, HASH_FIND, NULL);

	if (v != NULL)
		v->pins--;
}

static void
view_pin_release(void *arg)
{
	ViewPin    *p = arg;

	if (p->held)
		view_unpin(p->store);
	p->held = false;
}

static VStoreView *
view_get(Oid store)
{
	bool		found;
	VStoreView *v;

	if (views == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(VStoreView), .hcxt = TopMemoryContext};

		views = hash_create("sift value store views", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterSyscacheCallback(RELOID, stores_relcache_changed, (Datum) 0);
		sift_cache_register(&views_kind);
	}
	if (stores_changed)
		views_drop_gone(store);
	v = hash_search(views, &store, HASH_ENTER, &found);
	if (!found)
	{
		v->cx = AllocSetContextCreate(TopMemoryContext, "sift cache value store view", ALLOCSET_DEFAULT_SIZES);
		v->slots = NULL;
		v->nslots = 0;
		v->indexed = 0;
		v->loaded = false;
		v->piece = NULL;
		v->npiece = v->cap = 0;
		v->total = 0;
		v->epoch = 0;
		v->pins = 0;
	}
	return v;
}

void
vview_reset(VStoreView *v, uint64 epoch)
{
	MemoryContextReset(v->cx);
	v->slots = NULL;
	v->nslots = 0;
	v->indexed = 0;
	v->piece = NULL;
	v->npiece = v->cap = 0;
	v->total = 0;
	v->loaded = true;
	v->epoch = epoch;
}

void
vview_add(VStoreView *v, uint64 lo, uint64 dbase, SiftCDict *d)
{
	if (v->npiece == v->cap)
	{
		v->cap = Max(8, 2 * v->cap);
		v->piece = v->piece ? repalloc(v->piece, v->cap * sizeof(ViewPiece)) : MemoryContextAlloc(v->cx, v->cap * sizeof(ViewPiece));
	}
	v->piece[v->npiece++] = (ViewPiece) {lo, dbase, d};
	v->total = lo + d->n - (lo - dbase);
}

/* The view of a store, pinned until vview_release or the end of the current memory context, whichever comes first */
VStoreView *
vview_open(Oid store, ViewPin **pin)
{
	VStoreView *v = view_get(store);
	ViewPin    *p = palloc(sizeof(ViewPin));

	v->pins++;
	v->used = sift_cache_tick();
	*p = (ViewPin) {{view_pin_release, p, NULL}, store, true};
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &p->cb);
	sift_cache_room(0);
	*pin = p;
	return v;
}

void
vview_release(ViewPin *pin)
{
	view_pin_release(pin);
}

#ifdef SIFT_TESTING
void
sift_vstore_forget_views(void)
{
	HASH_SEQ_STATUS seq;
	VStoreView *v;

	if (views == NULL)
		return;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		v->loaded = false;
}
#endif
