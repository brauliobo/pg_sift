/*
 * sift_prefix.c - the documents of a prefix with more terms than sift.max_expansions.
 *
 * Such a prefix cannot be an OR of cursors with one score weight per term, which is what a leaf with
 * few expansions is (and why those are limited). It is read as the one term the index would hold
 * with prefix_index >= its length: the documents that have a word starting with it, tf the number of
 * those words (what an edge n-gram analyzer indexes). The postings of every expanded term are
 * gathered per superblock, merged in key order and encoded as the blob of a regular term, in memory,
 * so the cursors, bounds and scoring of the ranker need no special case. The cost is the postings of
 * the expansion, read once per segment and bind, unless a recent bind of this backend left the blob in
 * the cache: a segment never changes (its liveness is applied by the readers), so its generation names
 * the blob for good.
 */
#include "postgres.h"

#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_kernels.h"
#include "sift_prefix.h"

int			sift_prefix_cache_size = 8192;

/* A posting's slot in its superblock: page * 512 + offset - 1 */
#define SLOT_BITS		17
#define SLOT_OFFS		512
#define SLOT(key)		((uint32) ((((key) >> 16) & (SIFT_SB_PAGES - 1)) * SLOT_OFFS + ((key) & 0xFFFF) - 1))

/* The postings gathered for one superblock, slot << 8 | tf, in arrival order */
typedef struct SbPostings
{
	uint32	   *e;
	uint32		n;
	uint32		cap;
} SbPostings;

typedef struct Union
{
	SiftSegment *seg;
	const SiftTermMatcher *m;
	bool		scoring;
	MemoryContext cx;			/* the gathered postings */
	MemoryContext tcxt;			/* the cursor of one term */
	SbPostings *sb;				/* [sb_lo .. sb_hi] of the segment */
	uint64		total;
	uint8	   *acc;			/* tf of every slot of the superblock being merged */
	sift_tid   *tids;
	uint8	   *tf;
} Union;

static void
corrupt_union(const SiftSegment *seg, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("index \"%s\" is corrupted: segment %llu: %s", RelationGetRelationName(seg->index),
					(unsigned long long) seg->entry.gen, what)));
}

static void
union_add(Union *u, uint32 gid, SiftKey key, uint8 tf)
{
	const SiftSegHeaderData *h = u->seg->hdr;
	SbPostings *b;

	if (gid < h->sb_lo || gid > h->sb_hi || (key & 0xFFFF) == 0 || (key & 0xFFFF) > SLOT_OFFS)
		corrupt_union(u->seg, "a posting lies outside the segment's superblocks or has no offset");
	b = &u->sb[gid - h->sb_lo];
	if (b->n == b->cap)
	{
		b->cap = Max(64, 2 * b->cap);
		b->e = b->e ? repalloc_huge(b->e, b->cap * sizeof(uint32)) : MemoryContextAllocHuge(u->cx, b->cap * sizeof(uint32));
	}
	b->e[b->n++] = SLOT(key) << 8 | tf;
	u->total++;
}

static bool
union_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Union	   *u = arg;
	SiftPostingCursor *c;
	sift_cur   *cur;

	if (!sift_matcher_match(u->m, term, len))
		return true;
	if (info->flags & SIFT_TERM_INLINE)
	{
		for (uint32 i = 0; i < info->df; i++)
			union_add(u, SIFT_GID(info->inline_keys[i]), info->inline_keys[i], info->inline_tf[i]);
		return true;
	}
	c = sift_post_open(&u->seg->sr, info, u->tcxt);
	cur = sift_post_cursor(c);
	for (const sift_grp *g = sift_next(cur); g; g = sift_next(cur))
	{
		uint32		k = sift_grp_tids(g, NULL, u->tids);

		if (u->scoring)
			sift_post_group_tf(c, u->tf);
		for (uint32 j = 0; j < k; j++)
			union_add(u, g->gid, u->tids[j], u->scoring ? u->tf[j] : 1);
	}
	MemoryContextReset(u->tcxt);
	return true;
}

/* The dl of every key (ascending, all in superblock gid) from the docmap */
static void
union_dl(Union *u, uint32 gid, const SiftKey *keys, int n, uint16 *dl)
{
	SiftDocChunk dc;

	if (!sift_seg_docs(u->seg, gid, &dc))
		corrupt_union(u->seg, "a posting lies in a superblock without documents");
	for (int j = 0; j < n;)
	{
		BlockNumber blk = sift_key_block(keys[j]);
		uint64		present[SIFT_OFFMASK_WORDS];
		const uint16 *row;

		sift_seg_page(u->seg, &dc, blk & (SIFT_SB_PAGES - 1), present, &row);
		for (; j < n && sift_key_block(keys[j]) == blk; j++)
		{
			int			off = (int) (keys[j] & 0xFFFF) - 1;

			if (row == NULL || off < 0 || off >= SIFT_OFFMASK_WORDS * 64 || !((present[off / 64] >> (off % 64)) & 1))
				corrupt_union(u->seg, "a posting is not a document of the segment");
			dl[j] = row[off];
		}
	}
}

/*
 * Merges the superblock's postings into the writer, in key order: a document that has several words
 * sums their tf. The slots are summed in a dense array, which the scan also clears.
 */
static void
union_emit(Union *u, SiftPostingWriter *w, uint32 gid, SiftKey *keys, uint8 *tf, uint16 *dl)
{
	SbPostings *b = &u->sb[gid - u->seg->hdr->sb_lo];
	uint64	   *acc = (uint64 *) u->acc;
	int			n = 0;

	if (b->n == 0)
		return;
	for (uint32 j = 0; j < b->n; j++)
		u->acc[b->e[j] >> 8] = u->scoring ? Min(u->acc[b->e[j] >> 8] + (b->e[j] & 0xFF), SIFT_MAX_TF) : 1;
	pfree(b->e);
	b->e = NULL;
	for (uint32 wd = 0; wd < (1 << SLOT_BITS) / 8; wd++)
	{
		if (acc[wd] == 0)
			continue;
		for (int i = 0; i < 8; i++)
		{
			uint32		slot = wd * 8 + i;

			if (u->acc[slot] == 0)
				continue;
			keys[n] = (SiftKey) gid << (SIFT_SB_SHIFT + 16) | (SiftKey) (slot / SLOT_OFFS) << 16 | (slot % SLOT_OFFS + 1);
			tf[n++] = u->acc[slot];
			u->acc[slot] = 0;
		}
	}
	if (u->scoring)
		union_dl(u, gid, keys, n, dl);
	sift_postw_add(w, keys, tf, dl, n);
}

static const uint8 *
union_build(SiftSegment *seg, const SiftTermMatcher *m, const SiftScoreRef *ref, bool scoring, MemoryContext mcxt,
			SiftTermInfo *info)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift prefix union", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Union		u = {.seg = seg,.m = m,.scoring = scoring,.cx = cx};
	uint32		nsb = seg->hdr->sb_hi - seg->hdr->sb_lo + 1;
	const char *lo,
			   *hi;
	int			lolen,
				hilen;
	const uint8 *blob = NULL;

	u.tcxt = AllocSetContextCreate(cx, "sift prefix term", ALLOCSET_DEFAULT_SIZES);
	u.sb = palloc0(nsb * sizeof(SbPostings));
	u.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	u.tf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	u.acc = palloc0(1 << SLOT_BITS);
	sift_matcher_bounds(m, &lo, &lolen, &hi, &hilen);
	sift_dict_scan(seg->dict, lo, lolen, hi, hilen, union_term, &u);
	sift_sr_release(&seg->sr);
	memset(info, 0, sizeof(SiftTermInfo));
	if (u.total > 0)
	{
		SiftPostingWriter *w = sift_postw_begin(NULL, ref, cx);
		SiftKey    *keys = palloc(SIFT_GRP_MAXTIDS * sizeof(SiftKey));
		uint8	   *tf = palloc(SIFT_GRP_MAXTIDS);
		uint16	   *dl = palloc0(SIFT_GRP_MAXTIDS * sizeof(uint16));	/* without scoring no bound is read: one dl for all */

		for (uint32 i = 0; i < nsb; i++)
			union_emit(&u, w, seg->hdr->sb_lo + i, keys, tf, dl);
		blob = sift_postw_end_term_mem(w, info, mcxt);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return blob;
}

/* ---------- cache ---------- */

/* A blob is named by the segment (index file and generation) and the term it stands for */
typedef struct CacheKey
{
	RelFileLocator rel;
	uint64		gen;
	BlockNumber header_blk;
	uint16		keylen;
	bool		scoring;
	char		key[SIFT_MAX_TERM_BYTES + 2];
} CacheKey;

typedef struct CacheEnt
{
	CacheKey	key;			/* first: the hash key */
	SiftTermInfo info;
	uint8	   *blob;			/* with its padding; NULL for an inline term */
	Size		size;
	uint64		used;
} CacheEnt;

static HTAB *cache;
static MemoryContext cache_cx;
static Size cache_bytes;
static uint64 cache_clock;

static void
cache_key(const SiftSegment *seg, const char *key, int keylen, bool scoring, CacheKey *k)
{
	memset(k, 0, sizeof(CacheKey));
	k->rel = seg->index->rd_locator;
	k->gen = seg->entry.gen;
	k->header_blk = seg->entry.header_blk;
	k->keylen = keylen;
	k->scoring = scoring;
	memcpy(k->key, key, keylen);
}

static uint8 *
blob_copy(MemoryContext mcxt, const uint8 *blob, const SiftTermInfo *info)
{
	uint8	   *c;

	if (blob == NULL)
		return NULL;
	c = MemoryContextAllocAligned(mcxt, (Size) info->post_len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	memcpy(c, blob, (Size) info->post_len + SIFT_KERN_PAD);
	return c;
}

static void
cache_evict(Size need)
{
	while (cache_bytes + need > (Size) sift_prefix_cache_size * 1024 && hash_get_num_entries(cache) > 0)
	{
		HASH_SEQ_STATUS seq;
		CacheEnt   *e,
				   *lru = NULL;

		hash_seq_init(&seq, cache);
		while ((e = hash_seq_search(&seq)) != NULL)
			if (lru == NULL || e->used < lru->used)
				lru = e;
		cache_bytes -= lru->size;
		if (lru->blob)
			pfree(lru->blob);
		hash_search(cache, &lru->key, HASH_REMOVE, NULL);
	}
}

static void
cache_put(const CacheKey *k, const SiftTermInfo *info, const uint8 *blob)
{
	Size		size = sizeof(CacheEnt) + (blob ? (Size) info->post_len + SIFT_KERN_PAD : 0);
	CacheEnt   *e;

	if (size > (Size) sift_prefix_cache_size * 1024 / 2)
		return;
	if (cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(CacheKey),.entrysize = sizeof(CacheEnt)};

		cache_cx = AllocSetContextCreate(TopMemoryContext, "sift prefix cache", ALLOCSET_DEFAULT_SIZES);
		ctl.hcxt = cache_cx;
		cache = hash_create("sift prefix cache", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	cache_evict(size);
	e = hash_search(cache, k, HASH_ENTER, NULL);
	e->info = *info;
	e->blob = blob_copy(cache_cx, blob, info);
	e->size = size;
	e->used = ++cache_clock;
	cache_bytes += size;
}

/* A cached blob answers a scoring request, and the other way round only with its own kind */
static CacheEnt *
cache_find(const SiftSegment *seg, const char *key, int keylen, bool scoring)
{
	CacheKey	k;
	CacheEnt   *e = NULL;

	if (cache == NULL)
		return NULL;
	cache_key(seg, key, keylen, true, &k);
	e = hash_search(cache, &k, HASH_FIND, NULL);
	if (e == NULL && !scoring)
	{
		cache_key(seg, key, keylen, false, &k);
		e = hash_search(cache, &k, HASH_FIND, NULL);
	}
	if (e)
		e->used = ++cache_clock;
	return e;
}

const uint8 *
sift_prefix_union(SiftSegment *seg, const SiftTermMatcher *m, const char *key, int keylen, const SiftScoreRef *ref,
				  bool scoring, MemoryContext mcxt, SiftTermInfo *info)
{
	CacheEnt   *e = sift_prefix_cache_size > 0 ? cache_find(seg, key, keylen, scoring) : NULL;
	const uint8 *blob;
	CacheKey	k;

	if (e)
	{
		*info = e->info;
		return blob_copy(mcxt, e->blob, info);
	}
	blob = union_build(seg, m, ref, scoring, mcxt, info);
	if (sift_prefix_cache_size > 0)
	{
		cache_key(seg, key, keylen, scoring, &k);
		cache_put(&k, info, blob);
	}
	return blob;
}
