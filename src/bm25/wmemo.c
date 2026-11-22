/*
 * bm25/wmemo.c - the memo of a wide leaf.
 *
 * Weighing a wide leaf merges the dictionaries of every segment over the leaf's whole range, one distinct term at a time, for the blended df and the largest bound
 * of its terms, and a ranked search of it reads the few terms with the largest bounds before it finds that the others can matter. Both are functions of the index as it is:
 * a segment never changes (its generation names it, its live count says which deletes it has), so the memo is named by the segments, the leaf and the document count
 * of the plan, and a statement that finds the same name takes the answers from it. The K-th score of the last search of the same leaf tells the next one which terms
 * can reach it, so it reads those and no others; the search verifies the score it ends with against the terms the leaf left out, so a score that is not the K-th score of
 * the statement costs a second search, never a result.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/segment.h"
#include "bm25/wmemo.h"
#include "core/cachebudget.h"

typedef struct MemoPart
{
	uint64		gen;
	uint64		live_ndocs;
	uint64		header_blk;
} MemoPart;

typedef struct MemoEnt
{
	struct MemoEnt *newer;		/* the entries by use: the list ends are the newest and the oldest */
	struct MemoEnt *older;
	uint64		hash;
	uint64		used;			/* sift_cache_tick() of its last use */
	SiftWideMemo memo;
	uint32		namelen;
	int			nsegs;
	uint8	   *all_mb;			/* [nsegs] */
	uint8	   *name;
} MemoEnt;

typedef struct MemoSlot
{
	uint64		hash;			/* first: the hash key */
	MemoEnt    *ent;
} MemoSlot;

static HTAB *memo_tab;
static MemoryContext memo_cx;
static MemoEnt *memo_newest;
static MemoEnt *memo_lru;

static void
memo_unlink(MemoEnt *e)
{
	if (e->newer)
		e->newer->older = e->older;
	else
		memo_newest = e->older;
	if (e->older)
		e->older->newer = e->newer;
	else
		memo_lru = e->newer;
}

static void
memo_link_newest(MemoEnt *e)
{
	e->newer = NULL;
	e->older = memo_newest;
	if (memo_newest)
		memo_newest->newer = e;
	else
		memo_lru = e;
	memo_newest = e;
	e->used = sift_cache_tick();
}

static uint64
memo_oldest(void)
{
	return memo_lru ? memo_lru->used : UINT64_MAX;
}

static void
memo_drop(MemoEnt *e)
{
	memo_unlink(e);
	hash_search(memo_tab, &e->hash, HASH_REMOVE, NULL);
	pfree(e);
}

static bool
memo_evict_one(void)
{
	if (memo_lru == NULL)
		return false;
	memo_drop(memo_lru);
	return true;
}

static void
memo_evict_all(void)
{
	while (memo_evict_one())
		;
}

static const SiftCacheKind memo_kind = {"bm25_wide_memo", memo_oldest, memo_evict_one, memo_evict_all};

static bool
memo_eligible(const SiftPlan *p, const EvalLeaf *l)
{
	return sift_backend_cache_kb > 0 && p->nleaves == 1 && l->deferred && l->wide_key != NULL && p->nsegs > 0 &&
		(l->mtab == NULL || hash_get_num_entries(l->mtab) == 0);
}

/* The name of the index state and the leaf: the index file, the document count of the plan, every segment and the leaf's own name */
static uint8 *
memo_name(const SiftPlan *p, const EvalLeaf *l, uint32 *len)
{
	Size		n = sizeof(RelFileLocator) + sizeof(double) + 2 * sizeof(int32) + p->nsegs * sizeof(MemoPart) + l->wide_keylen;
	uint8	   *b = palloc0(n),
			   *w = b;
	RelFileLocator rel = p->seg[0]->index->rd_locator;
	double		ndocs = p->stats.ndocs;
	int32		nsegs = p->nsegs,
				keylen = l->wide_keylen;

	memcpy(w, &rel, sizeof(rel));
	w += sizeof(rel);
	memcpy(w, &ndocs, sizeof(ndocs));
	w += sizeof(ndocs);
	memcpy(w, &nsegs, sizeof(nsegs));
	w += sizeof(nsegs);
	memcpy(w, &keylen, sizeof(keylen));
	w += sizeof(keylen);
	for (int s = 0; s < p->nsegs; s++)
	{
		MemoPart	mp = {.gen = p->seg[s]->entry.gen,.header_blk = p->seg[s]->entry.header_blk};
		uint64		sum_dl;

		sift_seg_live_totals(p->seg[s], &mp.live_ndocs, &sum_dl);
		memcpy(w, &mp, sizeof(mp));
		w += sizeof(mp);
	}
	memcpy(w, l->wide_key, l->wide_keylen);
	*len = n;
	return b;
}

static uint64
memo_hash(const uint8 *b, uint32 len)
{
	uint64		h = UINT64CONST(0xcbf29ce484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ b[i]) * UINT64CONST(0x100000001b3);
	return h;
}

/* The entry of this name; the caller holds the cache lock */
static MemoEnt *
memo_find(const uint8 *name, uint32 len, uint64 hash)
{
	MemoSlot   *slot = memo_tab ? hash_search(memo_tab, &hash, HASH_FIND, NULL) : NULL;

	if (slot == NULL || slot->ent->namelen != len || memcmp(slot->ent->name, name, len) != 0)
		return NULL;
	return slot->ent;
}

bool
sift_wide_memo_find(const SiftPlan *p, const EvalLeaf *l, SiftWideMemo *out, uint8 *all_mb)
{
	uint32		len;
	uint8	   *name;
	uint64		hash;
	MemoEnt    *e;

	if (!memo_eligible(p, l))
		return false;
	name = memo_name(p, l, &len);
	hash = memo_hash(name, len);
	sift_host_cache_lock();
	e = memo_find(name, len, hash);
	if (e)
	{
		*out = e->memo;
		memcpy(all_mb, e->all_mb, e->nsegs);
		memo_unlink(e);
		memo_link_newest(e);
	}
	sift_host_cache_unlock();
	pfree(name);
	return e != NULL;
}

void
sift_wide_memo_store(const SiftPlan *p, const EvalLeaf *l, double df_blend, const uint8 *all_mb)
{
	uint32		len;
	uint8	   *name;
	uint64		hash;
	Size		size;
	MemoSlot   *slot;
	MemoEnt    *e;
	bool		found;

	if (!memo_eligible(p, l))
		return;
	name = memo_name(p, l, &len);
	hash = memo_hash(name, len);
	size = sizeof(MemoEnt) + p->nsegs + len;
	sift_host_cache_lock();
	if (memo_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(MemoSlot)};

		memo_cx = sift_cache_create_context("sift cache wide memo");
		ctl.hcxt = memo_cx;
		memo_tab = hash_create("sift wide memo", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		sift_cache_register(&memo_kind);
	}
	if (sift_cache_room(size))
	{
		if ((slot = hash_search(memo_tab, &hash, HASH_FIND, NULL)) != NULL)
			memo_drop(slot->ent);
		e = MemoryContextAlloc(memo_cx, size);
		*e = (MemoEnt) {.hash = hash,.memo = {df_blend, -INFINITY, 0},.namelen = len,.nsegs = p->nsegs};
		e->all_mb = (uint8 *) (e + 1);
		e->name = e->all_mb + p->nsegs;
		memcpy(e->all_mb, all_mb, p->nsegs);
		memcpy(e->name, name, len);
		slot = hash_search(memo_tab, &hash, HASH_ENTER, &found);
		slot->ent = e;
		memo_link_newest(e);
	}
	sift_host_cache_unlock();
	pfree(name);
}

void
sift_wide_memo_hint(const SiftPlan *p, const EvalLeaf *l, int64 k, float4 theta)
{
	uint32		len;
	uint8	   *name;
	uint64		hash;
	MemoEnt    *e;

	if (!memo_eligible(p, l))
		return;
	name = memo_name(p, l, &len);
	hash = memo_hash(name, len);
	sift_host_cache_lock();
	if ((e = memo_find(name, len, hash)) != NULL)
	{
		e->memo.k = k;
		e->memo.theta = theta;
	}
	sift_host_cache_unlock();
	pfree(name);
}

/*
 * A deferred leaf the memo has a K-th score for is weighed without a pass over the dictionaries: it keeps no top terms (as if none were), its bound over every term
 * is the memo's, and sift_plan_warm_begin reads the terms that reach the score, or sift_plan_install_top chooses the top terms when the search is another one.
 */
bool
sift_wide_memo_warm(SiftPlan *p, EvalLeaf *l, double *df_blend)
{
	SiftWideMemo m;
	uint8	   *mb = palloc(p->nsegs);
	bool		hit = sift_wide_memo_find(p, l, &m, mb) && m.theta > -INFINITY;

	if (hit)
	{
		l->tail_mb = memcpy(MemoryContextAlloc(p->mcxt, p->nsegs), mb, p->nsegs);
		l->all_mb = memcpy(MemoryContextAlloc(p->mcxt, p->nsegs), mb, p->nsegs);
		l->warm = true;
		l->warm_theta = m.theta;
		l->warm_k = m.k;
		*df_blend = m.df_blend;
	}
	pfree(mb);
	return hit;
}
