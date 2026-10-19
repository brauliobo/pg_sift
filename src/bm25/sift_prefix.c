/*
 * sift_prefix.c - the documents of a leaf with more terms than sift.max_expansions.
 *
 * Such a leaf cannot be an OR of cursors with one score weight per term, which is what a leaf with
 * few expansions is (and why those are limited). It is read as the one term the index would hold
 * with prefix_index >= its length: the documents that have a word the leaf matches (starting with
 * it, for a prefix), tf the number of those words (what an edge n-gram analyzer indexes). The postings of every expanded term are
 * gathered per superblock, merged in key order and encoded as the blob of a regular term, in memory,
 * so the cursors, bounds and scoring of the ranker need no special case. The cost is the postings of
 * the expansion, read once per segment and bind, unless a recent bind of this backend left the blob in
 * the cache: a segment never changes (its liveness is applied by the readers), so its generation names
 * the blob for good.
 */
#include "sift_host.h"

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

/*
 * One pass over the expansion's terms builds the superblocks [from, to]. A pass over the whole segment
 * gathers what it finds until it holds sift_prefix_gather_kb, then counts the postings of every superblock
 * instead, and the superblocks are built in windows that fit the budget; a window of one superblock
 * (a dense one) sums the tf of its postings as they come, so a pass holds the budget at most.
 */
typedef struct Union
{
	SiftSegment *seg;
	const SiftTermMatcher *m;
	bool		scoring;
	MemoryContext cx;			/* the gathered postings */
	MemoryContext tcxt;			/* the cursor of one term */
	SbPostings *sb;				/* [sb_lo .. sb_hi] of the segment */
	uint64		total;			/* postings of the pass */
	bool		any;			/* some superblock was built */
	SiftPostingWriter *w;		/* begun by the first one */
	const SiftScoreRef *ref;
	uint64		budget;			/* postings a pass may gather */
	uint32		from;			/* the superblocks of the pass */
	uint32		to;
	uint64	   *count;			/* counting: postings of every superblock of the segment, else NULL */
	uint8	   *acc;			/* tf of every slot of the superblock being merged */
	sift_tid   *tids;
	uint8	   *tf;
	SiftKey    *keys;			/* the merged superblock: allocated by the first */
	uint8	   *mtf;
	uint16	   *dl;
} Union;

int			sift_prefix_gather_kb = 131072;

#ifdef SIFT_TESTING
SiftPrefixStats sift_prefix_stats;
#define UNION_STAT(cond, f, v)	do { if (cond) sift_prefix_stats.f = (v); } while (0)
#else
#define UNION_STAT(cond, f, v)	((void) 0)
#endif

static void
corrupt_union(const SiftSegment *seg, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("index \"%s\" is corrupted: segment %llu: %s", RelationGetRelationName(seg->index),
					(unsigned long long) seg->entry.gen, what)));
}

static inline void
union_check(const Union *u, uint32 gid, SiftKey key)
{
	const SiftSegHeaderData *h = u->seg->hdr;

	if (gid < h->sb_lo || gid > h->sb_hi || (key & 0xFFFF) == 0 || (key & 0xFFFF) > SLOT_OFFS)
		corrupt_union(u->seg, "a posting lies outside the segment's superblocks or has no offset");
}

static inline bool
union_direct(const Union *u)
{
	return u->from == u->to;
}

static inline void
union_sum(const Union *u, uint32 slot, uint8 tf)
{
	u->acc[slot] = u->scoring ? Min(u->acc[slot] + tf, SIFT_MAX_TF) : 1;
}

/* Gathers a posting of the window (its tf is summed on the spot in a window of one superblock) */
static void
union_add(Union *u, uint32 gid, SiftKey key, uint8 tf)
{
	SbPostings *b;

	union_check(u, gid, key);
	if (gid < u->from || gid > u->to)
		return;
	u->total++;
	if (union_direct(u))
		return union_sum(u, SLOT(key), tf);
	b = &u->sb[gid - u->seg->hdr->sb_lo];
	if (b->n == b->cap)
	{
		b->cap = Max(64, 2 * b->cap);
		b->e = b->e ? repalloc_huge(b->e, b->cap * sizeof(uint32)) : MemoryContextAllocHuge(u->cx, b->cap * sizeof(uint32));
	}
	b->e[b->n++] = SLOT(key) << 8 | tf;
}

/* Over budget: forgets what was gathered and counts the postings of every superblock from here on */
static void
union_count_from_here(Union *u)
{
	uint32		nsb = u->seg->hdr->sb_hi - u->seg->hdr->sb_lo + 1;

	u->count = MemoryContextAllocZero(u->cx, nsb * sizeof(uint64));
	for (uint32 i = 0; i < nsb; i++)
	{
		u->count[i] = u->sb[i].n;
		if (u->sb[i].e)
			pfree(u->sb[i].e);
		u->sb[i] = (SbPostings) {0};
	}
	UNION_STAT(u->total > sift_prefix_stats.peak, peak, u->total);
	u->total = 0;
}

/* A window of several superblocks holds what fits the budget; the pass over all of them counts what exceeds it */
static void
union_over_budget(Union *u, uint64 more)
{
	if (u->count == NULL && !union_direct(u) && (u->total + more) * sizeof(uint32) > u->budget)
		union_count_from_here(u);
}

static void
union_count(Union *u, uint32 gid, uint64 n)
{
	union_check(u, gid, 1);
	u->count[gid - u->seg->hdr->sb_lo] += n;
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
			if (u->count)
				union_count(u, SIFT_GID(info->inline_keys[i]), 1);
			else
				union_add(u, SIFT_GID(info->inline_keys[i]), info->inline_keys[i], info->inline_tf[i]);
		union_over_budget(u, 0);
		return true;
	}
	c = sift_post_open(&u->seg->sr, info, u->tcxt);
	sift_sr_unlock(&u->seg->sr);
	cur = sift_post_cursor(c);
	for (const sift_grp *g = sift_seek(cur, u->from); g && g->gid <= u->to; g = sift_next(cur))
	{
		uint64		n = sift_grp_count(g);
		uint32		k;

		CHECK_FOR_INTERRUPTS();
		union_over_budget(u, n);
		if (u->count)
		{
			union_count(u, g->gid, n);
			continue;
		}
		k = sift_grp_tids(g, NULL, u->tids);
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

/* The postings of docs that VACUUM has not removed: a vacuumed doc counts in no df and no score, as in the merged segment */
static int
union_live(Union *u, SiftKey *keys, uint8 *tf, int n)
{
	int			k = 0;

	for (int i = 0; i < n; i++)
		if (sift_seg_key_live(u->seg, keys[i]))
		{
			keys[k] = keys[i];
			tf[k++] = tf[i];
		}
	return k;
}

/*
 * Merges the superblock's postings into the writer, in key order: a document that has several words
 * sums their tf. The slots are summed in a dense array, which the scan also clears.
 */
static void
union_emit(Union *u, uint32 gid)
{
	SbPostings *b = &u->sb[gid - u->seg->hdr->sb_lo];
	uint64	   *acc = (uint64 *) u->acc;
	int			n = 0;

	SiftKey    *keys;
	uint8	   *tf;

	if (b->n == 0 && !union_direct(u))
		return;
	if (u->keys == NULL)
	{
		u->keys = MemoryContextAlloc(u->cx, SIFT_GRP_MAXTIDS * sizeof(SiftKey));
		u->mtf = MemoryContextAlloc(u->cx, SIFT_GRP_MAXTIDS);
		u->dl = MemoryContextAllocZero(u->cx, SIFT_GRP_MAXTIDS * sizeof(uint16));	/* without scoring no bound is read: one dl for all */
	}
	keys = u->keys;
	tf = u->mtf;
	for (uint32 j = 0; j < b->n; j++)
		union_sum(u, b->e[j] >> 8, b->e[j] & 0xFF);
	if (b->e)
		pfree(b->e);
	b->e = NULL;
	b->n = 0;
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
	if (sift_seg_sb_dirty(u->seg, gid))
		n = union_live(u, keys, tf, n);
	if (n == 0)
		return;
	if (u->scoring)
		union_dl(u, gid, keys, n, u->dl);
	if (u->w == NULL)
		u->w = sift_postw_begin(NULL, u->ref, u->cx);
	sift_postw_add(u->w, keys, tf, u->dl, n, NULL, NULL);
	u->any = true;
}

static void
union_scan(Union *u, uint32 from, uint32 to)
{
	const char *lo,
			   *hi;
	int			lolen,
				hilen;

	u->from = from;
	u->to = to;
	u->total = 0;
	UNION_STAT(true, passes, sift_prefix_stats.passes + 1);
	UNION_STAT(from != u->seg->hdr->sb_lo || to != u->seg->hdr->sb_hi, windows, sift_prefix_stats.windows + 1);
	UNION_STAT(from == to && u->count == NULL && u->seg->hdr->sb_lo != u->seg->hdr->sb_hi, direct, sift_prefix_stats.direct + 1);
	sift_matcher_bounds(u->m, &lo, &lolen, &hi, &hilen);
	sift_dict_scan(u->seg->dict, lo, lolen, hi, hilen, union_term, u);
	sift_sr_release(&u->seg->sr);
	UNION_STAT(!union_direct(u) && u->total > sift_prefix_stats.peak, peak, u->total);
}

/* The windows [from, to] of consecutive superblocks, each gathering at most the budget (a dense superblock alone is one) */
static uint32
union_window_end(const Union *u, uint32 from)
{
	uint64		sum = 0;
	uint32		to = from;

	for (uint32 sb = from; sb <= u->seg->hdr->sb_hi; sb++)
	{
		sum += u->count[sb - u->seg->hdr->sb_lo];
		if (sb > from && sum * sizeof(uint32) > u->budget)
			break;
		to = sb;
	}
	return to;
}

/* The superblocks of the segment, in windows, once a pass found that all of them do not fit the budget */
static void
union_windows(Union *u)
{
	const SiftSegHeaderData *h = u->seg->hdr;
	uint64	   *count = u->count;

	for (uint32 sb = h->sb_lo; sb <= h->sb_hi;)
	{
		uint32		to;

		if (count[sb - h->sb_lo] == 0)
		{
			sb++;
			continue;
		}
		to = union_window_end(u, sb);
		u->count = NULL;
		union_scan(u, sb, to);
		for (uint32 i = sb; i <= to; i++)
			union_emit(u, i);
		u->count = count;
		sb = to + 1;
	}
}

static const uint8 *
union_build(SiftSegment *seg, const SiftTermMatcher *m, const SiftScoreRef *ref, bool scoring, MemoryContext mcxt,
			SiftTermInfo *info)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift prefix union", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Union		u = {.seg = seg,.m = m,.scoring = scoring,.cx = cx,.ref = ref,.budget = (uint64) Max(sift_prefix_gather_kb, 1) * 1024};
	const SiftSegHeaderData *h = seg->hdr;
	const uint8 *blob = NULL;

	u.tcxt = AllocSetContextCreate(cx, "sift prefix term", ALLOCSET_DEFAULT_SIZES);
	u.sb = palloc0((h->sb_hi - h->sb_lo + 1) * sizeof(SbPostings));
	u.tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	u.tf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	u.acc = palloc0(1 << SLOT_BITS);
	memset(info, 0, sizeof(SiftTermInfo));
	union_scan(&u, h->sb_lo, h->sb_hi);
	if (u.count)
		union_windows(&u);
	else
		for (uint32 sb = h->sb_lo; sb <= h->sb_hi; sb++)
			union_emit(&u, sb);
	if (u.any)
		blob = sift_postw_end_term_mem(u.w, info, mcxt);
	info->ldf = info->df;		/* the union holds live docs only */
	info->slot = -1;
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
	uint64		live_ndocs;		/* the union holds live docs only: another VACUUM makes another blob */
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
	uint64		sum_dl;

	memset(k, 0, sizeof(CacheKey));
	k->rel = seg->index->rd_locator;
	k->gen = seg->entry.gen;
	k->header_blk = seg->entry.header_blk;
	sift_seg_live_totals((SiftSegment *) seg, &k->live_ndocs, &sum_dl);
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

		cache_cx = AllocSetContextCreate(TopMemoryContext, "sift cache prefix", ALLOCSET_DEFAULT_SIZES);
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
	bool		cached = key != NULL && sift_prefix_cache_size > 0;
	CacheEnt   *e = cached ? cache_find(seg, key, keylen, scoring) : NULL;
	const uint8 *blob;
	CacheKey	k;

	if (e)
	{
		*info = e->info;
		return blob_copy(mcxt, e->blob, info);
	}
	blob = union_build(seg, m, ref, scoring, mcxt, info);
	if (cached)
	{
		cache_key(seg, key, keylen, scoring, &k);
		cache_put(&k, info, blob);
	}
	return blob;
}
