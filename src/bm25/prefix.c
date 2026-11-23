/*
 * bm25/prefix.c - the documents of a leaf with more terms than sift.max_expansions.
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
#include "core/host.h"

#include "bm25/kernels.h"
#include "bm25/prefix.h"
#include "core/cachebudget.h"
#include "core/damage.h"
#include "core/fmv.h"
#include "core/memtrack.h"

/* A posting's slot in its superblock: page * 512 + offset - 1 */
#define SLOT_BITS		17
#define SLOT_OFFS		512
#define SLOT(key)		((uint32) ((((key) >> 16) & (SIFT_SB_PAGES - 1)) * SLOT_OFFS + ((key) & 0xFFFF) - 1))

/* The superblocks of a segment are kept in chunks that are made when a posting first lands in one: a sparse segment takes memory for the superblocks it has postings in, not for its span */
#define SB_CHUNK		256

/* A superblock with at most this many postings is merged by sorting them; a denser one in the table of slots */
#define UNION_SPARSE_MAX	2048

/* A term the pass found: later windows read its postings without the scan of the dictionary and the matcher */
typedef struct CapTerm
{
	SiftTermInfo info;
	uint32		sb_lo;			/* the first and last superblock it has postings in */
	uint32		sb_hi;
} CapTerm;

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
 * (a dense one) keeps the best tf of its postings as they come, so a pass holds the budget at most.
 */
typedef struct Union
{
	SiftSegment *seg;
	SiftTermMatcher *m;
	bool		scoring;
	uint8		min_bound;		/* the terms with a smaller bound are left out */
	uint8		left_out;		/* the largest bound of those */
	MemoryContext cx;			/* the gathered postings */
	MemoryContext tcxt;			/* the cursor of one term */
	SbPostings **sb;			/* the chunks of [sb_lo .. sb_hi] of the segment, NULL where no posting landed */
	uint64		total;			/* postings of the pass */
	bool		any;			/* some superblock was built */
	SiftPostingWriter *w;		/* begun by the first one */
	bool		field;			/* its terms are a field's (the matcher's terms start with the field byte) */
	const SiftScoreRef *ref;
	uint64		budget;			/* postings a pass may gather */
	uint32		from;			/* the superblocks of the pass */
	uint32		to;
	uint64	   *count;			/* counting: postings of every superblock of the segment, else NULL */
	uint8	   *acc;			/* tf of every slot of the superblock being merged (a dense one), made by the first */
	uint64		dirty[(1 << SLOT_BITS) / 64 / 64];	/* the blocks of 64 slots of acc that hold a tf */
	CapTerm    *terms;			/* the terms of the pass over the segment, while it fits half the budget (terms_capped: it did not) */
	uint32		nterms;
	uint32		termcap;
	bool		capture;
	bool		terms_capped;
	bool		replay;			/* the passes of windows read terms, not the dictionary */
	sift_tid   *tids;
	uint8	   *tf;
	SiftKey    *keys;			/* the merged superblock: allocated by the first */
	uint8	   *mtf;
	uint16	   *dl;
	uint32		keycap;			/* entries of keys, mtf and dl */
	uint32		tidcap;			/* entries of tids and tf */
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
	SIFT_DAMAGE_INDEX(seg->index, errmsg("index \"%s\" is corrupted: segment %llu: %s", RelationGetRelationName(seg->index),
					(unsigned long long) seg->entry.gen, what));
}

static inline void
union_check(const Union *u, uint32 gid, SiftKey key)
{
	const SiftSegHeaderData *h = u->seg->hdr;

	if (gid < h->sb_lo || gid > h->sb_hi || (key & 0xFFFF) == 0 || (key & 0xFFFF) > SLOT_OFFS)
		corrupt_union(u->seg, "a posting lies outside the segment's superblocks or has no offset");
}

static void *
union_regrow(Union *u, void *p, Size n)
{
	SIFT_REGROW_IN(u->cx, p, n);
	return p;
}

/* MEM[MEM-BM25-01]: prefix union_build scratch (128KB acc + 1.49MB tid/tf/key arrays; 128MB gather cap) */
/* The postings of a group of n: the arrays of one group's decode start at 256 and grow to what a group needs (their content is not kept) */
static void
union_group_scratch(Union *u, uint32 n)
{
	if (n <= u->tidcap)
		return;
	if (u->tids)
	{
		pfree(u->tids);
		pfree(u->tf);
	}
	u->tidcap = Min(Max(n, 256), SIFT_GRP_MAXTIDS);
	u->tids = MemoryContextAlloc(u->cx, (Size) u->tidcap * sizeof(sift_tid));
	u->tf = MemoryContextAlloc(u->cx, u->tidcap + SIFT_KERN_PAD);
}

/* The arrays of a merged superblock of n postings; dl is zero, the one dl of a union that does not score */
static void
union_merge_scratch(Union *u, uint32 n)
{
	if (n <= u->keycap)
		return;
	if (u->keys)
	{
		pfree(u->keys);
		pfree(u->mtf);
		pfree(u->dl);
	}
	u->keycap = Min(Max(n, 2048), SIFT_GRP_MAXTIDS);
	u->keys = MemoryContextAlloc(u->cx, (Size) u->keycap * sizeof(SiftKey));
	u->mtf = MemoryContextAlloc(u->cx, u->keycap);
	u->dl = MemoryContextAllocZero(u->cx, (Size) u->keycap * sizeof(uint16));
}

static inline SbPostings *
union_sb(const Union *u, uint32 gid)
{
	uint32		i = gid - u->seg->hdr->sb_lo;

	return u->sb[i / SB_CHUNK] ? &u->sb[i / SB_CHUNK][i % SB_CHUNK] : NULL;
}

static SbPostings *
union_sb_make(Union *u, uint32 gid)
{
	uint32		i = gid - u->seg->hdr->sb_lo;

	if (u->sb[i / SB_CHUNK] == NULL)
		u->sb[i / SB_CHUNK] = MemoryContextAllocZero(u->cx, SB_CHUNK * sizeof(SbPostings));
	return &u->sb[i / SB_CHUNK][i % SB_CHUNK];
}

static inline bool
union_direct(const Union *u)
{
	return u->from == u->to;
}

static inline void
union_max(Union *u, uint32 slot, uint8 tf)
{
	if (u->acc == NULL)
		u->acc = MemoryContextAllocZero(u->cx, 1 << SLOT_BITS);
	u->acc[slot] = u->scoring ? Max(u->acc[slot], tf) : 1;
	u->dirty[slot >> 12] |= UINT64CONST(1) << ((slot >> 6) & 63);
}

/* Gathers a posting of the window (its tf is merged on the spot in a window of one superblock) */
static void
union_add(Union *u, uint32 gid, SiftKey key, uint8 tf)
{
	SbPostings *b;

	union_check(u, gid, key);
	if (gid < u->from || gid > u->to)
		return;
	u->total++;
	if (union_direct(u))
		return union_max(u, SLOT(key), tf);
	b = union_sb_make(u, gid);
	if (b->n == b->cap)
	{
		b->cap = Max(64, 2 * b->cap);
		b->e = union_regrow(u, b->e, b->cap * sizeof(uint32));
	}
	b->e[b->n++] = SLOT(key) << 8 | tf;
}

/* Over budget: forgets what was gathered and counts the postings of every superblock from here on */
static void
union_count_from_here(Union *u)
{
	uint32		nsb = u->seg->hdr->sb_hi - u->seg->hdr->sb_lo + 1;

	u->count = MemoryContextAllocZero(u->cx, nsb * sizeof(uint64));
	for (uint32 c = 0; c < (nsb + SB_CHUNK - 1) / SB_CHUNK; c++)
	{
		if (u->sb[c] == NULL)
			continue;
		for (uint32 i = c * SB_CHUNK; i < Min(nsb, (c + 1) * SB_CHUNK); i++)
		{
			u->count[i] = u->sb[c][i % SB_CHUNK].n;
			if (u->sb[c][i % SB_CHUNK].e)
				pfree(u->sb[c][i % SB_CHUNK].e);
		}
		pfree(u->sb[c]);
		u->sb[c] = NULL;
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
union_skip(void *arg, const char *term, int len, const char **next, int *nextlen)
{
	return sift_matcher_skip(((Union *) arg)->m, term, len, next, nextlen);
}

static inline void
cap_note(CapTerm *ct, uint32 gid)
{
	ct->sb_lo = Min(ct->sb_lo, gid);
	ct->sb_hi = Max(ct->sb_hi, gid);
}

/* The postings of a term in the superblocks [from, to] of the pass; ct (a term of the capture) learns the superblocks it lies in */
static void
union_postings(Union *u, const SiftTermInfo *info, CapTerm *ct)
{
	SiftPostingCursor *c;
	sift_cur   *cur;

	if (info->flags & SIFT_TERM_INLINE)
	{
		for (uint32 i = 0; i < info->df; i++)
		{
			SiftKey		key = info->inline_keys[i];

			if (ct)
				cap_note(ct, SIFT_GID(key));
			if (u->count)
				union_count(u, SIFT_GID(key), 1);
			else
				union_add(u, SIFT_GID(key), key, info->inline_tf[i]);
		}
		union_over_budget(u, 0);
		return;
	}
	c = sift_post_open(&u->seg->sr, info, u->tcxt);
	sift_sr_unlock(&u->seg->sr);
	cur = sift_post_cursor(c);
	for (const sift_grp *g = sift_seek(cur, u->from); g && g->gid <= u->to; g = sift_next(cur))
	{
		uint64		n = sift_grp_count(g);
		uint32		k;

		CHECK_FOR_INTERRUPTS();
		if (ct)
			cap_note(ct, g->gid);
		union_over_budget(u, n);
		if (u->count)
		{
			union_count(u, g->gid, n);
			continue;
		}
		union_group_scratch(u, n);
		k = sift_grp_tids(g, NULL, u->tids);
		if (u->scoring)
			sift_post_group_tf(c, u->tf);
		for (uint32 j = 0; j < k; j++)
			union_add(u, g->gid, u->tids[j], u->scoring ? u->tf[j] : 1);
	}
	MemoryContextReset(u->tcxt);
}

/* Keeps the term for the later windows, unless the terms of the segment would take more than half the budget */
static CapTerm *
union_capture(Union *u, const SiftTermInfo *info)
{
	CapTerm    *ct;

	if (u->terms_capped)
		return NULL;
	if ((uint64) (u->nterms + 1) * sizeof(CapTerm) > u->budget / 2)
	{
		u->terms_capped = true;
		if (u->terms)
			pfree(u->terms);
		u->terms = NULL;
		return NULL;
	}
	if (u->nterms == u->termcap)
	{
		u->termcap = Max(256, 2 * u->termcap);
		u->terms = union_regrow(u, u->terms, (Size) u->termcap * sizeof(CapTerm));
	}
	ct = &u->terms[u->nterms++];
	ct->info = *info;
	ct->sb_lo = UINT32_MAX;
	ct->sb_hi = 0;
	return ct;
}

static bool
union_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Union	   *u = arg;

	if (!sift_matcher_match(u->m, term, len))
		return true;
	if (info->max_bound < u->min_bound)
	{
		u->left_out = Max(u->left_out, info->max_bound);
		return true;
	}
	union_postings(u, info, u->capture ? union_capture(u, info) : NULL);
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

/* Moves the nonzero slots of acc (the tf of every slot of a superblock) to keys and tf in slot order and zeroes them; returns their number: only the blocks that hold a tf are read */
static uint32
drain_acc(Union *u, SiftKey *keys, uint8 *tf, SiftKey base)
{
	uint32		n = 0;

	for (int w = 0; w < (int) lengthof(u->dirty); w++)
	{
		for (uint64 m = u->dirty[w]; m != 0; m &= m - 1)
		{
			uint32		blk = (w * 64 + pg_rightmost_one_pos64(m)) * 64;

			for (uint32 slot = blk; slot < blk + 64; slot++)
			{
				if (u->acc[slot] == 0)
					continue;
				keys[n] = base | (SiftKey) (slot / SLOT_OFFS) << 16 | (slot % SLOT_OFFS + 1);
				tf[n++] = u->acc[slot];
				u->acc[slot] = 0;
			}
		}
		u->dirty[w] = 0;
	}
	return n;
}

static int
slot_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const uint32 *) a, *(const uint32 *) b);
}

/* The postings of a sparse superblock sorted by slot and tf, the best tf of a slot last: one pass keeps it */
static uint32
sparse_merge(Union *u, SbPostings *b, SiftKey *keys, uint8 *tf, SiftKey base)
{
	uint32		n = 0;

	qsort(b->e, b->n, sizeof(uint32), slot_cmp);
	for (uint32 j = 0; j < b->n;)
	{
		uint32		slot = b->e[j] >> 8;
		uint8		best = b->e[j] & 0xFF;

		while (++j < b->n && b->e[j] >> 8 == slot)
			best = b->e[j] & 0xFF;
		keys[n] = base | (SiftKey) (slot / SLOT_OFFS) << 16 | (slot % SLOT_OFFS + 1);
		tf[n++] = u->scoring ? best : 1;
	}
	return n;
}

/*
 * Merges the superblock's postings into the writer, in key order: a document that has several words
 * keeps their best tf. The slots are merged in a dense array, which the scan also clears.
 */
static void
union_emit(Union *u, uint32 gid)
{
	SbPostings *b = union_sb(u, gid);
	int			n = 0;

	SiftKey    *keys;
	uint8	   *tf;

	if (b == NULL ? !union_direct(u) : b->n == 0 && !union_direct(u))
		return;
	union_merge_scratch(u, b != NULL && b->n <= UNION_SPARSE_MAX ? b->n : SIFT_GRP_MAXTIDS);
	keys = u->keys;
	tf = u->mtf;
	if (b != NULL && b->n <= UNION_SPARSE_MAX)
		n = sparse_merge(u, b, keys, tf, (SiftKey) gid << (SIFT_SB_SHIFT + 16));
	else
	{
		for (uint32 j = 0; b != NULL && j < b->n; j++)
			union_max(u, b->e[j] >> 8, b->e[j] & 0xFF);
		n = u->acc ? drain_acc(u, keys, tf, (SiftKey) gid << (SIFT_SB_SHIFT + 16)) : 0;
	}
	if (b != NULL)
	{
		if (b->e)
			pfree(b->e);
		b->e = NULL;
		b->n = 0;
	}
	if (sift_seg_sb_dirty(u->seg, gid))
		n = union_live(u, keys, tf, n);
	if (n == 0)
		return;
	if (u->scoring)
		union_dl(u, gid, keys, n, u->dl);
	if (u->w == NULL)
	{
		u->w = sift_postw_begin(NULL, u->ref, u->cx);
		sift_postw_field(u->w, u->field);
	}
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
	if (u->replay)
	{
		for (uint32 i = 0; i < u->nterms; i++)
			if (u->terms[i].sb_hi >= from && u->terms[i].sb_lo <= to)
				union_postings(u, &u->terms[i].info, NULL);
	}
	else
	{
		sift_matcher_bounds(u->m, &lo, &lolen, &hi, &hilen);
		sift_dict_scan_skipping(u->seg->dict, lo, lolen, hi, hilen, sift_matcher_skips(u->m) ? union_skip : NULL, union_term, u);
	}
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
union_build(SiftSegment *seg, SiftTermMatcher *m, const SiftScoreRef *ref, bool scoring, int min_bound, MemoryContext mcxt,
			SiftTermInfo *info, uint8 *left_out)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift prefix union");
	MemoryContext old = MemoryContextSwitchTo(cx);
	Union		u = {.seg = seg,.m = m,.scoring = scoring,.min_bound = min_bound,.cx = cx,.ref = ref,
					 .budget = (uint64) Max(Min(sift_prefix_gather_kb, work_mem / 2), 1) * 1024};
	const SiftSegHeaderData *h = seg->hdr;
	const uint8 *blob = NULL;

	u.tcxt = sift_mem_allocset_create(cx, "sift prefix term");
	u.sb = palloc0(((h->sb_hi - h->sb_lo + SB_CHUNK) / SB_CHUNK) * sizeof(SbPostings *));
	memset(info, 0, sizeof(SiftTermInfo));
	{
		const char *lo,
				   *hi;
		int			lolen,
					hilen;

		sift_matcher_bounds(m, &lo, &lolen, &hi, &hilen);
		u.field = lolen > 0 && SIFT_FIELD_TERM(lo[0], SIFT_MAX_FIELDS);
	}
	u.capture = true;
	union_scan(&u, h->sb_lo, h->sb_hi);
	u.capture = false;
	u.replay = !u.terms_capped;
	if (u.count)
		union_windows(&u);
	else if (union_direct(&u))
		union_emit(&u, h->sb_lo);
	else
		for (uint32 c = 0; c * SB_CHUNK <= h->sb_hi - h->sb_lo; c++)
			for (uint32 i = c * SB_CHUNK; u.sb[c] && i < Min(h->sb_hi - h->sb_lo + 1, (c + 1) * SB_CHUNK); i++)
				union_emit(&u, h->sb_lo + i);
	if (u.any)
		blob = sift_postw_end_term_mem(u.w, info, mcxt);
	info->ldf = info->df;		/* the union holds live docs only */
	info->slot = -1;
	if (left_out)
		*left_out = u.left_out;
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return blob;
}

/* A blob is named by the segment (index file and generation) and the term it stands for */
typedef struct CacheKey
{
	RelFileLocator rel;
	uint64		gen;
	BlockNumber header_blk;
	uint64		live_ndocs;		/* the union holds live docs only: another VACUUM makes another blob */
	uint16		keylen;
	bool		scoring;
	uint8		min_bound;		/* the union leaves the terms with a smaller bound out: 0 is the union of all of them */
	char		key[SIFT_MAX_TERM_BYTES + 2];
} CacheKey;

typedef struct CacheEnt
{
	CacheKey	key;			/* first: the hash key */
	SiftTermInfo info;
	uint8	   *blob;			/* with its padding; NULL for an inline term */
	uint8		left_out;		/* the largest bound among the terms the union left out */
	Size		size;
	uint64		used;			/* sift_cache_tick() of its last use */
	int			pins;			/* threads copying the blob: a pinned entry is out of the list, so no eviction reaches it */
	struct CacheEnt *newer;		/* the entries by use: the list ends are the newest and the oldest */
	struct CacheEnt *older;
} CacheEnt;

static HTAB *cache;
static MemoryContext cache_cx;
static CacheEnt *cache_newest;
static CacheEnt *cache_lru;

static void
cache_key(const SiftSegment *seg, const char *key, int keylen, bool scoring, int min_bound, CacheKey *k)
{
	uint64		sum_dl;

	memset(k, 0, sizeof(CacheKey));
	k->rel = seg->index->rd_locator;
	k->gen = seg->entry.gen;
	k->header_blk = seg->entry.header_blk;
	sift_seg_live_totals((SiftSegment *) seg, &k->live_ndocs, &sum_dl);
	k->keylen = keylen;
	k->scoring = scoring;
	k->min_bound = (uint8) min_bound;
	memcpy(k->key, key, keylen);
}

static uint8 *
blob_copy(MemoryContext mcxt, const uint8 *blob, const SiftTermInfo *info, int flags)
{
	uint8	   *c;

	if (blob == NULL)
		return NULL;
	c = MemoryContextAllocAligned(mcxt, (Size) info->post_len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, flags);
	memcpy(c, blob, (Size) info->post_len + SIFT_KERN_PAD);
	return c;
}

static void
cache_unlink(CacheEnt *e)
{
	if (e->newer)
		e->newer->older = e->older;
	else
		cache_newest = e->older;
	if (e->older)
		e->older->newer = e->newer;
	else
		cache_lru = e->newer;
}

static void
cache_link_newest(CacheEnt *e)
{
	e->newer = NULL;
	e->older = cache_newest;
	if (cache_newest)
		cache_newest->newer = e;
	else
		cache_lru = e;
	cache_newest = e;
	e->used = sift_cache_tick();
}

static uint64
cache_oldest(void)
{
	return cache_lru ? cache_lru->used : UINT64_MAX;
}

static bool
cache_evict_one(void)
{
	CacheEnt   *lru = cache_lru;

	if (lru == NULL)
		return false;
	cache_unlink(lru);
	if (lru->blob)
		pfree(lru->blob);
	hash_search(cache, &lru->key, HASH_REMOVE, NULL);
	return true;
}

static void
cache_evict_all(void)
{
	while (cache_evict_one())
		;
}

static const SiftCacheKind prefix_kind = {"bm25_prefix", cache_oldest, cache_evict_one, cache_evict_all};

/* MEM[MEM-BM25-02]: prefix cache_put (MCXT_ALLOC_HUGE, missing sift_cache_commit; 258B inline CacheKey) */
static void
cache_put(const CacheKey *k, const SiftTermInfo *info, const uint8 *blob, uint8 left_out)
{
	Size		size = sizeof(CacheEnt) + (blob ? (Size) info->post_len + SIFT_KERN_PAD : 0);
	CacheEnt   *e;
	uint8	   *copy;

	if (cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(CacheKey),.entrysize = sizeof(CacheEnt)};

		cache_cx = sift_cache_create_context("sift cache prefix");
		ctl.hcxt = cache_cx;
		cache = hash_create("sift prefix cache", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		sift_cache_register(&prefix_kind);
	}
	if (size > (Size) sift_backend_cache_kb * 1024 / 2 || !sift_cache_room(size) || hash_search(cache, k, HASH_FIND, NULL) != NULL)
		return;
	copy = blob_copy(cache_cx, blob, info, 0);
	e = hash_search(cache, k, HASH_ENTER, NULL);
	e->info = *info;
	e->blob = copy;
	e->left_out = left_out;
	e->size = size;
	e->pins = 0;
	cache_link_newest(e);
	sift_cache_commit();
}

/*
 * A cached blob answers a scoring request, and the other way round only with its own kind; the union of all the terms answers a request that leaves terms out
 * (it holds them, which the caller's score allows), and a union that leaves terms out answers the request for the same bound only. The caller holds the cache lock and pins the entry before it lets go.
 */
static CacheEnt *
cache_find(const SiftSegment *seg, const char *key, int keylen, bool scoring, int min_bound)
{
	const int	bounds[2] = {0, min_bound};
	CacheEnt   *e = NULL;
	CacheKey	k;

	if (cache == NULL)
		return NULL;
	for (int b = 0; b < (min_bound > 0 ? 2 : 1) && e == NULL; b++)
		for (int plain = 0; plain <= !scoring && e == NULL; plain++)
		{
			cache_key(seg, key, keylen, !plain, bounds[b], &k);
			e = hash_search(cache, &k, HASH_FIND, NULL);
		}
	return e;
}

/*
 * The cache is one for all the threads of a host that has them (sift_host_cache_lock: no lock in PostgreSQL, where a backend is alone): its table, lists and
 * copies are touched under the lock, the build of a union never is, so queries for different prefixes build at once.
 */
const uint8 *
sift_prefix_union(SiftSegment *seg, SiftTermMatcher *m, const char *key, int keylen, const SiftScoreRef *ref,
				  bool scoring, int min_bound, MemoryContext mcxt, SiftTermInfo *info, uint8 *left_out)
{
	bool		cached = key != NULL && sift_backend_cache_kb > 0;
	const uint8 *blob;
	uint8		left = 0;

	if (cached)
	{
		CacheEnt   *e;

		sift_host_cache_lock();
		e = cache_find(seg, key, keylen, scoring, min_bound);
		if (e)
		{
			*info = e->info;
			if (left_out)
				*left_out = e->left_out;
			if (e->pins++ == 0)
				cache_unlink(e);
			sift_host_cache_unlock();
			blob = blob_copy(mcxt, e->blob, info, MCXT_ALLOC_HUGE);
			sift_host_cache_lock();
			if (--e->pins == 0)
				cache_link_newest(e);
			sift_host_cache_unlock();
			return blob;
		}
		sift_host_cache_unlock();
	}
	blob = union_build(seg, m, ref, scoring, min_bound, mcxt, info, &left);
	if (cached)
	{
		CacheKey	k;

		cache_key(seg, key, keylen, scoring, min_bound, &k);
		sift_host_cache_lock();
		cache_put(&k, info, blob, left);
		sift_host_cache_unlock();
	}
	if (left_out)
		*left_out = left;
	return blob;
}
