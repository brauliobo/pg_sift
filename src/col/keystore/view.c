/*
 * col/keystore/view.c - the view a backend keeps of each scope key store it read, under the budget of sift.backend_cache_kb (core/cachebudget.h)
 *
 * A view keeps every string of its store. One that no open handle pins is dropped, least recently used first, when another cache needs room, and read again by
 * the next statement that needs it; one in use is never dropped.
 */
#include "postgres.h"

#include "access/table.h"
#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "core/cachebudget.h"
#include "col/v1/seg/int.h"
#include "col/keystore/keystore.h"
#include "col/keystore/int.h"

static HTAB *views;
static bool stores_changed;
static dlist_head views_lru = DLIST_STATIC_INIT(views_lru);

static void
view_unlink(KeyStoreView *v)
{
	if (v->on_lru)
		dlist_delete(&v->lru);
	v->on_lru = false;
}

static void
stores_relcache_changed(Datum arg, int cacheid, uint32 hashvalue)
{
	stores_changed = true;
}

/* The views of the stores that were dropped: a store is one relation, and its view holds the strings of all its codes; one a handle pins stays, and the next call looks again */
static void
views_drop_gone(Oid keep)
{
	HASH_SEQ_STATUS seq;
	KeyStoreView *v;

	stores_changed = false;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		if (v->key.store != keep && !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(v->key.store)))
		{
			if (v->pins > 0)
			{
				stores_changed = true;
				continue;
			}
			view_unlink(v);
			MemoryContextDelete(v->cx);
			hash_search(views, &v->key, HASH_REMOVE, NULL);
		}
}

static void
view_clear(KeyStoreView *v)
{
	view_unlink(v);
	MemoryContextReset(v->cx);
	v->slots = NULL;
	v->nslots = 0;
	v->indexed = 0;
	v->piece = NULL;
	v->npiece = v->cap = 0;
	v->total = 0;
	v->loaded = false;
}

/* the loaded views are in the order of their last open: the first of them that no handle pins is the least recently used */
static KeyStoreView *
views_victim(void)
{
	dlist_iter it;

	dlist_foreach(it, &views_lru)
	{
		KeyStoreView *v = dlist_container(KeyStoreView, lru, it.cur);

		if (v->pins == 0)
			return v;
	}
	return NULL;
}

static uint64
views_oldest(void)
{
	KeyStoreView *v = views_victim();

	return v ? v->used : UINT64_MAX;
}

static bool
views_evict_one(void)
{
	KeyStoreView *v = views_victim();

	if (v == NULL)
		return false;
	view_clear(v);
	return true;
}

static void
views_evict_all(void)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &views_lru)
	{
		KeyStoreView *v = dlist_container(KeyStoreView, lru, it.cur);

		if (v->pins == 0)
			view_clear(v);
	}
}

static const SiftCacheKind views_kind = {"col_keystore_view", views_oldest, views_evict_one, views_evict_all};

static void
view_unpin(const ViewKey *key)
{
	KeyStoreView *v = hash_search(views, key, HASH_FIND, NULL);

	if (v != NULL)
		v->pins--;
}

static void
view_pin_release(void *arg)
{
	ViewPin    *p = arg;

	if (p->held)
		view_unpin(&p->key);
	p->held = false;
}

static KeyStoreView *
view_get(const ViewKey *key)
{
	bool		found;
	KeyStoreView *v;

	if (views == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(ViewKey), .entrysize = sizeof(KeyStoreView), .hcxt = sift_cache_root_context()};

		views = hash_create("sift scope key store views", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterSyscacheCallback(RELOID, stores_relcache_changed, (Datum) 0);
		sift_cache_register(&views_kind);
	}
	if (stores_changed)
		views_drop_gone(key->store);
	v = hash_search(views, key, HASH_ENTER, &found);
	if (!found)
	{
		v->cx = sift_cache_create_context("sift cache scope key store view");
		v->slots = NULL;
		v->nslots = 0;
		v->indexed = 0;
		v->loaded = false;
		v->piece = NULL;
		v->npiece = v->cap = 0;
		v->total = 0;
		v->pins = 0;
		v->on_lru = false;
	}
	return v;
}

void
kview_reset(KeyStoreView *v)
{
	MemoryContextReset(v->cx);
	v->slots = NULL;
	v->nslots = 0;
	v->indexed = 0;
	v->piece = NULL;
	v->npiece = v->cap = 0;
	v->total = 0;
	v->loaded = true;
	if (!v->on_lru)
	{
		dlist_push_tail(&views_lru, &v->lru);
		v->on_lru = true;
	}
}

void
kview_add(KeyStoreView *v, uint64 lo, uint64 dbase, SiftCDict *d)
{
	if (v->npiece == v->cap)
	{
		v->cap = Max(8, 2 * v->cap);
		v->piece = v->piece ? repalloc(v->piece, v->cap * sizeof(ViewPiece)) : MemoryContextAlloc(v->cx, v->cap * sizeof(ViewPiece));
	}
	v->piece[v->npiece++] = (ViewPiece) {lo, dbase, d};
	v->total = lo + d->n - (lo - dbase);
}

/* The view of a store, pinned until kview_release or the end of the current memory context, whichever comes first */
KeyStoreView *
kview_open(Oid store, uint64 epoch, ViewPin **pin)
{
	ViewKey		key = {.store = store,.pad = 0,.epoch = epoch};
	KeyStoreView *v = view_get(&key);
	ViewPin    *p = palloc(sizeof(ViewPin));

	v->pins++;
	v->used = sift_cache_tick();
	if (v->on_lru)
		dlist_move_tail(&views_lru, &v->lru);
	*p = (ViewPin) {{view_pin_release, p, NULL}, key, true};
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &p->cb);
	sift_cache_commit();
	*pin = p;
	return v;
}

void
kview_release(ViewPin *pin)
{
	view_pin_release(pin);
}

#ifdef SIFT_TESTING
void
sift_keystore_forget_views(void)
{
	HASH_SEQ_STATUS seq;
	KeyStoreView *v;

	if (views == NULL)
		return;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
	{
		view_unlink(v);
		v->loaded = false;
	}
}
#endif
