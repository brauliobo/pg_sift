/*
 * col/f2/f2_scope_cache.c - the opened summaries of the scopes of the indexes a backend reads (f2_scope_cache.h).
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "col/f2/f2.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/part_format.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"
#include "core/page_io.h"

#ifdef SIFT_TESTING
SiftScopeCacheStats sift_f2_scope_cache_stats;
#define SCOPE_STAT(f)	(sift_f2_scope_cache_stats.f++)
#else
#define SCOPE_STAT(f)	((void) 0)
#endif

typedef struct ScopeKey
{
	RelFileLocator locator;
	uint64		meta_gen;
	int32		ord;
	int32		pad;
} ScopeKey;

typedef struct ScopeEntry
{
	ScopeKey	key;			/* first: the hash key */
	SiftScopeMeta meta;
	MemoryContext cx;
	struct SiftF2ScopeSnap *overlay;	/* the settled change of the summary the statements read last (f2_scope_snap.h), in cx */
	int			pins;			/* a statement that reads it: only an entry nobody pins is dropped */
	bool		private;		/* the cache had no room: it is the statement's own, and goes with it */
	uint64		used;
	dlist_node	node;
} ScopeEntry;

typedef struct Unpin
{
	MemoryContextCallback cb;
	ScopeEntry *entry;
} Unpin;

static HTAB *cache;
static MemoryContext cache_cx;
static dlist_head lru = DLIST_STATIC_INIT(lru);

static void
drop(ScopeEntry *e)
{
	ScopeKey	key = e->key;

	dlist_delete(&e->node);
	MemoryContextDelete(e->cx);
	hash_search(cache, &key, HASH_REMOVE, NULL);
}

static ScopeEntry *
victim(void)
{
	dlist_iter	it;

	dlist_foreach(it, &lru)
	{
		ScopeEntry *e = dlist_container(ScopeEntry, node, it.cur);

		if (e->pins == 0)
			return e;
	}
	return NULL;
}

static uint64
scope_oldest(void)
{
	ScopeEntry *e = victim();

	return e ? e->used : UINT64_MAX;
}

static bool
scope_evict_one(void)
{
	ScopeEntry *e = victim();

	if (e == NULL)
		return false;
	drop(e);
	return true;
}

static void
scope_evict_all(void)
{
	while (scope_evict_one())
		;
}

static const SiftCacheKind scope_kind = {"col_f2_scope_cache", scope_oldest, scope_evict_one, scope_evict_all};

static void
cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(ScopeKey),.entrysize = sizeof(ScopeEntry)};

	cache_cx = sift_cache_create_context("sift cache scope summaries");
	ctl.hcxt = cache_cx;
	cache = hash_create("sift cache scope summaries", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&scope_kind);
}

static void
unpin(void *arg)
{
	((Unpin *) arg)->entry->pins--;
}

static ScopeEntry *
pin(ScopeEntry *e, MemoryContext mcxt)
{
	Unpin	   *u = MemoryContextAlloc(mcxt, sizeof(Unpin));

	e->pins++;
	e->used = sift_cache_tick();
	dlist_move_tail(&lru, &e->node);
	u->cb = (MemoryContextCallback) {unpin, u, NULL};
	u->entry = e;
	MemoryContextRegisterResetCallback(mcxt, &u->cb);
	return e;
}

const SiftManifestScopeRef *
sift_f2_scope_cache_ref(const SiftPlanSnapshot *plan, int ord)
{
	for (int i = 0; i < plan->nscopes; i++)
		if (plan->scopes[i].scope_ord == ord && (plan->scopes[i].flags & SIFT_SCOPEREF_ACTIVE))
			return &plan->scopes[i];
	return NULL;
}

/* The summaries of an older generation of the same scope are of no use any more: the directory names the new one */
static void
drop_older(const ScopeKey *key)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &lru)
	{
		ScopeEntry *e = dlist_container(ScopeEntry, node, it.cur);

		if (e->pins == 0 && e->key.ord == key->ord && RelFileLocatorEquals(e->key.locator, key->locator) && e->key.meta_gen < key->meta_gen)
			drop(e);
	}
}

static ScopeEntry *
load(Relation index, const SiftManifestScopeRef *ref, const ScopeKey *key, MemoryContext mcxt)
{
	ScopeEntry	fresh = {.key = *key};
	ScopeEntry *e;
	MemoryContext old;
	uint64		len;
	char	   *bytes;

	fresh.cx = sift_mem_allocset_create(mcxt, "sift cache scope summary");
	old = MemoryContextSwitchTo(fresh.cx);
	bytes = sift_f2_scope_read_object(index, sift_part_open(index, ref->header_blk, ref->meta_gen, 0), &len);
	if (bytes == NULL || !sift_scope_meta_open_ex(&fresh.meta, bytes, len, SIFT_SMETA_OPEN_LAZY_TOPK))
		SIFT_CORRUPT(index, "The summary of scope %d is damaged.", key->ord);
	MemoryContextSwitchTo(old);
	if (!sift_cache_room(MemoryContextMemAllocated(fresh.cx, true)))
	{
		e = MemoryContextAlloc(fresh.cx, sizeof(fresh));
		*e = fresh;
		e->private = true;
		SCOPE_STAT(private_loads);
		return e;
	}
	drop_older(key);
	MemoryContextSetParent(fresh.cx, cache_cx);
	e = hash_search(cache, key, HASH_ENTER, NULL);
	*e = fresh;
	dlist_push_tail(&lru, &e->node);
	SCOPE_STAT(loads);
	return e;
}

const SiftScopeMeta *
sift_f2_scope_cache_get(Relation index, const SiftPlanSnapshot *plan, int ord, MemoryContext mcxt)
{
	const SiftManifestScopeRef *ref = sift_f2_scope_cache_ref(plan, ord);
	ScopeKey	key = {.locator = index->rd_locator,.ord = ord};
	ScopeEntry *e;

	if (ref == NULL)
		return NULL;
	key.meta_gen = ref->meta_gen;
	if (cache == NULL)
		cache_init();
	e = hash_search(cache, &key, HASH_FIND, NULL);
	if (e != NULL)
		SCOPE_STAT(hits);
	else
		e = load(index, ref, &key, mcxt);
	if (!e->private)
	{
		pin(e, mcxt);
		(void) sift_cache_commit();
	}
	return &e->meta;
}

const SiftScopeMeta *
sift_f2_scope_cache_peek(Relation index, const SiftPlanSnapshot *plan, int ord)
{
	const SiftManifestScopeRef *ref = sift_f2_scope_cache_ref(plan, ord);
	ScopeKey	key = {.locator = index->rd_locator,.ord = ord};
	ScopeEntry *e;

	if (ref == NULL || cache == NULL)
		return NULL;
	key.meta_gen = ref->meta_gen;
	e = hash_search(cache, &key, HASH_FIND, NULL);
	return e != NULL ? &e->meta : NULL;
}

bool
sift_f2_scope_cache_rings(const SiftScopeMeta *m)
{
	SiftScopeMeta *entry_meta = (SiftScopeMeta *) m;

	if (m->topk_packed)
		SCOPE_STAT(ring_unpacks);
	return sift_scope_meta_ensure_topk(entry_meta);
}

struct SiftF2ScopeSnap **
sift_f2_scope_cache_overlay(const SiftScopeMeta *m, MemoryContext *entry_cx)
{
	ScopeEntry *e = (ScopeEntry *) ((char *) m - offsetof(ScopeEntry, meta));

	*entry_cx = e->cx;
	return &e->overlay;
}
