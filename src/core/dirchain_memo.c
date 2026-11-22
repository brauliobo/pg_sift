/*
 * core/dirchain_memo.c - the whole chain of a directory at a version, memoized per backend (see core/dirchain_int.h).
 *
 * A directory update stores a new version and a chain is immutable under one, so the entries of an index at a version are the same wherever they were read: a statement that
 * asks for the directory again (each of its writers does, to find a segment, to open the tombstone log, to add a segment) copies the memo instead of reading and checking every
 * page of the chain. One entry per index, dropped by the budget the backend's caches share (core/cachebudget.h) and by a newer version, least recently used first.
 */
#include "core/host.h"

#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "core/cachebudget.h"
#include "core/dirchain_int.h"

static HTAB *memos;
static MemoryContext memo_cx;
static dlist_head memo_lru = DLIST_STATIC_INIT(memo_lru);

static DirMemo *
memo_oldest_entry(void)
{
	return dlist_is_empty(&memo_lru) ? NULL : dlist_container(DirMemo, lru, dlist_head_node(&memo_lru));
}

static uint64
memo_oldest(void)
{
	DirMemo    *e = memo_oldest_entry();

	return e ? e->used : UINT64_MAX;
}

static void
memo_drop(DirMemo *e)
{
	RelFileLocator rel = e->rel;

	dlist_delete(&e->lru);
	pfree(e->data);
	hash_search(memos, &rel, HASH_REMOVE, NULL);
}

static bool
memo_evict_one(void)
{
	DirMemo    *e = memo_oldest_entry();

	if (e == NULL)
		return false;
	memo_drop(e);
	return true;
}

static void
memo_evict_all(void)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &memo_lru)
		memo_drop(dlist_container(DirMemo, lru, it.cur));
}

static const SiftCacheKind memo_kind = {"core_dirchain", memo_oldest, memo_evict_one, memo_evict_all};

static void
memo_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(DirMemo)};

	memo_cx = sift_cache_create_context("sift cache directories");
	ctl.hcxt = memo_cx;
	memos = hash_create("sift chained directories", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&memo_kind);
}

/* Where the arrays of a chain of nsegs entries lie in one allocation: entries, leaves, interior pages, blocks */
static Size
memo_bytes_of(int nsegs, const SiftDirChain *c)
{
	return nsegs * sizeof(SiftSegEntry) + c->nleaves * sizeof(SiftChainRef) + c->ninner * sizeof(SiftDirInner) + c->nblocks * sizeof(BlockNumber);
}

DirMemo *
sift_dirchain_memo_keep(Relation index, uint64 version, const SiftSegEntry *segs, int nsegs, const SiftDirChain *chain)
{
	Size		bytes = memo_bytes_of(nsegs, chain);
	DirMemo    *e;
	bool		found;
	char	   *at;

	if (memos == NULL)
		memo_init();
	if ((e = hash_search(memos, &index->rd_locator, HASH_FIND, NULL)) != NULL)
		memo_drop(e);
	if (!sift_cache_room(bytes))
		return NULL;
	e = hash_search(memos, &index->rd_locator, HASH_ENTER, &found);
	e->root = chain->root;
	e->version = version;
	e->used = sift_cache_tick();
	dlist_push_tail(&memo_lru, &e->lru);
	e->bytes = bytes;
	e->nsegs = nsegs;
	e->data = at = MemoryContextAlloc(memo_cx, Max(bytes, 1));
	e->segs = (SiftSegEntry *) at;
	memcpy(at, segs, nsegs * sizeof(SiftSegEntry));
	at += nsegs * sizeof(SiftSegEntry);
	e->chain = *chain;
	e->chain.leaf = (SiftChainRef *) at;
	memcpy(at, chain->leaf, chain->nleaves * sizeof(SiftChainRef));
	at += chain->nleaves * sizeof(SiftChainRef);
	e->chain.inner = (SiftDirInner *) at;
	memcpy(at, chain->inner, chain->ninner * sizeof(SiftDirInner));
	at += chain->ninner * sizeof(SiftDirInner);
	e->chain.blocks = (BlockNumber *) at;
	memcpy(at, chain->blocks, chain->nblocks * sizeof(BlockNumber));
	e->chain.maxblocks = chain->nblocks;
	return e;
}

DirMemo *
sift_dirchain_memo_find(Relation index, BlockNumber root, uint64 version)
{
	DirMemo    *e = memos == NULL ? NULL : hash_search(memos, &index->rd_locator, HASH_FIND, NULL);

	if (e == NULL || e->root != root || e->version != version)
		return NULL;
	e->used = sift_cache_tick();
	dlist_delete(&e->lru);
	dlist_push_tail(&memo_lru, &e->lru);
	return e;
}

