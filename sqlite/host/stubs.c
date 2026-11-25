/*
 * stubs.c - the collaborators of src/bm25/eval/eval.c that PostgreSQL provides and the embedded hosts (SQLite, MariaDB) do not
 * need, the same on both: docmap directories are not kept between queries and there are no write buffers (the live
 * segment of a query is an immutable one built from the queue). Options, visibility and the TID bitmap are the host's own.
 */
#include "core/host.h"

#include "bm25/analyze/analyze.h"
#include "bm25/hitspill.h"
#include "bm25/wbuf.h"
#include "bm25/segment/segment.h"

/* ---------- the heap (core/host.h): the embedded hosts leave it to the system ---------- */

void
sift_host_pin_heap(void)
{
}

/* ---------- the sort of the hits of an unbounded ranked search (bm25/hitspill.h): the hosts have none, so such a search fails past its memory ---------- */

SiftHitSpill *
sift_hitspill_begin(MemoryContext mcxt, int work_mem_kb)
{
	return NULL;
}

void
sift_hitspill_put(SiftHitSpill *s, SiftKey key, float4 score)
{
}

void
sift_hitspill_sort(SiftHitSpill *s)
{
}

bool
sift_hitspill_next(SiftHitSpill *s, SiftKey *key, float4 *score)
{
	return false;
}

void
sift_hitspill_end(SiftHitSpill *s)
{
}

/* ---------- the name of an index in a hint (core/store/store.h): a table of the host has no schema ---------- */

const char *
sift_index_ident(Relation index)
{
	return RelationGetRelationName(index);
}

/* ---------- text search configurations (bm25/analyze/ts.c): a PostgreSQL catalog object, so the hosts have none ---------- */

SiftTsStem *
sift_ts_open(const char *config, MemoryContext mcxt)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unknown sift stemmer \"%s\"", config),
					errhint("Valid stemmers are \"en\" and \"none\".")));
	return NULL;
}

int
sift_ts_stem(SiftTsStem *ts, char *w, int len)
{
	return len;
}

/* ---------- INCLUDE columns (bm25/attr.c): the hosts keep none ---------- */

SiftAttrSrc *
sift_attr_src(SiftWriteTarget *wt, MemoryContext mcxt)
{
	return NULL;
}

/*
 * ---------- bytes of a segment (bm25/segcache.c) and docmap directories (bm25/docdir.c): kept for the next query ----------
 *
 * A segment never changes: what its relation file (relNumber) and generation hold is named for good, so the SB directory of
 * its docmap and the blob of a term are kept under what names them, up to the cache budget of the process. Every thread of the
 * host shares the cache under sift_host_cache_lock. A reader pins the entry it uses until its memory context goes (a callback);
 * the victim is the head of the list of the entries nobody pins, so eviction and release are O(1). An entry is filled by the
 * thread that made it, outside the lock, and is handed out once ready: a thread that finds it filling reads its own copy.
 * The live segment of a MariaDB query (generation from SIFT_EPHEMERAL_GEN) is never kept.
 */
#include <stdlib.h>

#include "bm25/analyze/analyze.h"
#include "core/cachebudget.h"

#define CACHE_BUCKETS		8192
#define SIFT_EPHEMERAL_GEN	((uint64) 0xFFFF << 48)

typedef struct CacheEnt
{
	Oid			rel;
	uint64		gen,
				id;
	Size		size;
	int			kind;
	void	   *obj;
	int			pins;
	bool		ready;			/* filled by its first user */
	bool		listed;			/* in the hash table: a dropped entry that is still pinned is freed by its last unpin */
	struct CacheEnt *prev,
			   *next;			/* the list it is on: pinned, or lru (least recently released first) */
	struct CacheEnt *chain;
} CacheEnt;

typedef struct CacheList
{
	CacheEnt   *head,
			   *tail;
} CacheList;

typedef struct Unpin
{
	MemoryContextCallback cb;
	CacheEnt   *ent;
} Unpin;

int			sift_docdir_min_sb = 128;

static CacheEnt *buckets[CACHE_BUCKETS];
static CacheList lru,
			pinned;
static Size cache_bytes;

static void
list_push(CacheList *l, CacheEnt *e)
{
	e->prev = l->tail;
	e->next = NULL;
	if (l->tail)
		l->tail->next = e;
	else
		l->head = e;
	l->tail = e;
}

static void
list_remove(CacheList *l, CacheEnt *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		l->head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		l->tail = e->prev;
}

static CacheEnt **
bucket_of(Oid rel, uint64 gen, uint64 id, int kind)
{
	return &buckets[(rel * 0x9E3779B1u ^ gen * 0x85EBCA77u ^ id * 0xC2B2AE3Du ^ (uint64) kind) % CACHE_BUCKETS];
}

static CacheEnt *
ent_find(Oid rel, uint64 gen, uint64 id, Size size, int kind)
{
	for (CacheEnt *e = *bucket_of(rel, gen, id, kind); e; e = e->chain)
		if (e->rel == rel && e->gen == gen && e->id == id && e->size == size && e->kind == kind)
			return e;
	return NULL;
}

static void
ent_unhash(CacheEnt *e)
{
	CacheEnt  **p = bucket_of(e->rel, e->gen, e->id, e->kind);

	while (*p != e)
		p = &(*p)->chain;
	*p = e->chain;
	e->listed = false;
}

static void
ent_free(CacheEnt *e)
{
	if (e->listed)
		ent_unhash(e);
	cache_bytes -= e->size;
	free(e->obj);
	free(e);
}

static void
ent_unpin(void *arg)
{
	CacheEnt   *e = ((Unpin *) arg)->ent;

	sift_host_cache_lock();
	list_remove(&pinned, e);
	if (--e->pins > 0)
		list_push(&pinned, e);
	else if (e->ready && e->listed)
		list_push(&lru, e);
	else
		ent_free(e);
	sift_host_cache_unlock();
}

/* Makes room for size more bytes by evicting the entries nobody pins, oldest first */
static bool
cache_room(Size size)
{
	Size		limit = (Size) sift_backend_cache_kb * 1024;

	while (cache_bytes + size > limit && lru.head)
	{
		CacheEnt   *victim = lru.head;

		list_remove(&lru, victim);
		ent_free(victim);
	}
	return cache_bytes + size <= limit;
}

bool
sift_segcache_get(const SiftStreamReader *sr, int kind, uint64 id, Size size, MemoryContext mcxt, void **obj, void **handle)
{
	Oid			rel = sr->index->rd_locator.relNumber;
	CacheEnt   *e = NULL;
	Unpin	   *u;
	bool		ready = false;

	*handle = NULL;
	if (size > 0 && size <= (Size) sift_backend_cache_kb * 1024 / 4 && sr->gen < SIFT_EPHEMERAL_GEN)
	{
		sift_host_cache_lock();
		e = ent_find(rel, sr->gen, id, size, kind);
		if (e && !e->ready)
			e = NULL;				/* being filled by another thread: this one reads its own copy */
		else if (e)
		{
			if (e->pins == 0)
				list_remove(&lru, e);
			else
				list_remove(&pinned, e);
		}
		else if (cache_room(size) && posix_memalign(obj, SIFT_SIMD_ALIGN, size) == 0)
		{
			CacheEnt  **b = bucket_of(rel, sr->gen, id, kind);

			e = calloc(1, sizeof(CacheEnt));
			*e = (CacheEnt) {.rel = rel,.gen = sr->gen,.id = id,.size = size,.kind = kind,.obj = *obj,.listed = true,.chain = *b};
			*b = e;
			cache_bytes += size;
		}
		if (e)
		{
			ready = e->ready;
			e->pins++;
			list_push(&pinned, e);
		}
		sift_host_cache_unlock();
	}
	if (e == NULL)
	{
		*obj = MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
		return false;
	}
	u = MemoryContextAlloc(mcxt, sizeof(Unpin));
	u->cb = (MemoryContextCallback) {ent_unpin, u, NULL};
	u->ent = e;
	MemoryContextRegisterResetCallback(mcxt, &u->cb);
	*obj = e->obj;
	*handle = e;
	return ready;
}

bool
sift_segcache_has(const SiftStreamReader *sr, int kind, uint64 id, Size size)
{
	CacheEnt   *e;
	bool		has;

	sift_host_cache_lock();
	e = ent_find(sr->index->rd_locator.relNumber, sr->gen, id, size, kind);
	has = e != NULL && e->ready;
	sift_host_cache_unlock();
	return has;
}

void
sift_segcache_ready(void *handle)
{
	if (handle)
	{
		sift_host_cache_lock();
		((CacheEnt *) handle)->ready = true;
		sift_host_cache_unlock();
	}
}

/* The relation is gone or its cache epoch moved on: nothing kept under rel is reachable again; a pinned entry goes at its last unpin */
void
sift_host_segcache_drop_rel(Oid rel)
{
	sift_host_cache_lock();
	for (int i = 0; i < CACHE_BUCKETS; i++)
		for (CacheEnt *e = buckets[i], *next; e; e = next)
		{
			next = e->chain;
			if (e->rel != rel)
				continue;
			if (e->pins == 0)
			{
				list_remove(&lru, e);
				ent_free(e);
			}
			else
				ent_unhash(e);
		}
	sift_host_cache_unlock();
}

bool
sift_docdir_get(SiftSegment *seg, uint32 nsb, SiftDocSBRef **dir, uint64 **rank_base, void **handle)
{
	void	   *obj;
	bool		ready;

	*handle = NULL;
	if (nsb < sift_docdir_min_sb)
	{
		*dir = MemoryContextAllocHuge(seg->mcxt, nsb * sizeof(SiftDocSBRef));
		*rank_base = MemoryContextAllocHuge(seg->mcxt, (nsb + 1) * sizeof(uint64));
		return false;
	}
	ready = sift_segcache_get(&seg->sr, SIFT_SEGCACHE_DOCDIR, seg->entry.header_blk,
							  (Size) nsb * (sizeof(SiftDocSBRef) + sizeof(uint64)) + sizeof(uint64), seg->mcxt, &obj, handle);
	*rank_base = obj;
	*dir = (SiftDocSBRef *) (*rank_base + nsb + 1);
	return ready;
}

void
sift_docdir_ready(void *handle)
{
	sift_segcache_ready(handle);
}

/* ---------- write buffers (bm25/wbuf.c): the directory never lists one ---------- */

bool
sift_wbuf_bloom_has(const uint64 *bloom, int nwords, uint32 hash)
{
	return false;
}

SiftWbufReader *
sift_wbuf_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftWbufPageFilter filter, void *filter_arg)
{
	elog(ERROR, "sift: this build has no write buffers");
	return NULL;
}

bool
sift_wbuf_next(SiftWbufReader *r, SiftWbufDoc *doc)
{
	return false;
}

void
sift_wbuf_note_walked(const SiftWbufReader *r)
{
}

void
sift_wbuf_totals(const SiftWbufReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_wbuf_close(SiftWbufReader *r)
{
}
