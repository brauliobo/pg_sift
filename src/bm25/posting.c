/*
 * bm25/posting.c - term blob reader: the posting cursor over a term blob (layout in bm25/posting.h;
 * the writer is postw.c, the position records pos.c).
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "bm25/posting.h"
#include "bm25/posting_int.h"

/* the L1 bit and nibble bytes of a term, from counts the dictionary stores as uint32: the sums wrap in 32 bits when a count is near 2^32 */
#define L1_BYTES(info)	(((uint64) (info)->df + 7) / 8)
#define NIB_BYTES(info)	(((uint64) (info)->nnib + 1) / 2)
#define BLOB_NG(blob)	(*(const uint32 *) ((blob) + 8))
#define BLOB_KEYS(blob)	((const uint32 *) ((blob) + 12))

uint8
sift_bound_u8(const SiftScoreRef *ref, uint32 tf, uint32 dl)
{
	SiftBm25	bm = {.k1 = ref->k1,.b = ref->b,.avgdl = ref->avgdl};

	return sift_kern_bound_u8(&bm, tf, dl);
}

/* the codec of the portable core (bm25/bitmap.c) raises through the host */
void
sift_codec_corrupt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("sift: corrupt posting data: %s", what)));
}

static void
corrupt_blob(const SiftTermInfo *info)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: term blob at %llu does not match its dictionary entry",
					(unsigned long long) info->post_off)));
}

/* The length of the blob without its position stream */
static Size
blob_len(const SiftTermInfo *info)
{
	Size		poslen = (info->flags & SIFT_TERM_POS) ? info->pos_len : 0;

	if (poslen > info->post_len)
		corrupt_blob(info);
	return info->post_len - poslen;
}

#ifdef SIFT_TESTING
SiftPostStats sift_post_stats;
#define POST_STAT(f, v)		(sift_post_stats.f += (v))
#else
#define POST_STAT(f, v)		((void) 0)
#endif

/*
 * The buffer a cursor decodes its groups into. A sparse term (a LIST blob or inline keys) has a row per
 * page of a group, at most as many as it has postings (post_attach bounds their width); SIFT_CUR_BUF is for a group of
 * a GROUPED term
 */
static uint8 *
cursor_buf(MemoryContext mcxt, const SiftTermInfo *info)
{
	Size		size = SIFT_CUR_BUF;

	if (info->flags & (SIFT_TERM_INLINE | SIFT_TERM_SPARSE))
		size = Min(size, (Size) Min(info->df, SIFT_GROUP_PAGES) * SIFT_MAX_W + SIFT_KERN_PAD);
	POST_STAT(cursors, 1);
	POST_STAT(curbuf_bytes, size);
	return MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, 0);
}

/* The cursor over a term blob with SIFT_KERN_PAD zero bytes behind it (NULL for an inline term); sr reads the positions */
static SiftPostingCursor *
post_attach(const SiftTermInfo *info, MemoryContext mcxt, const uint8 *blob, SiftStreamReader *sr, bool verify)
{
	SiftPostingCursor *c = MemoryContextAllocZero(mcxt, sizeof(SiftPostingCursor));
	const char *detail;

	c->info = *info;
	c->mcxt = mcxt;
	c->curbuf = cursor_buf(mcxt, info);
	if (blob == NULL)
	{
		if (!(info->flags & SIFT_TERM_INLINE) || info->df == 0 || info->df > SIFT_INLINE_MAX)
			corrupt_blob(info);
		for (uint32 i = 0; i < info->df; i++)
			if (SIFT_OFF(info->inline_keys[i]) - 1 >= SIFT_MAX_OFF)
				corrupt_blob(info);
		if (info->flags & SIFT_TERM_POS)
		{
			c->poslen = info->pos_len;
			c->info.inline_pos = memcpy(MemoryContextAlloc(mcxt, info->pos_len), info->inline_pos, info->pos_len);
		}
		sift_post_rewind(c);
		return c;
	}
	if (info->flags & SIFT_TERM_POS)
	{
		c->poslen = info->pos_len;
		c->nck = (info->df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY;
		if ((uint64) c->nck * sizeof(uint32) > info->pos_len)
			corrupt_blob(info);
	}
	c->bloblen = blob_len(info);
	c->sr = sr;
	c->posoff = info->post_off + c->bloblen;
	c->blob = blob;
	if (info->codec_len > c->bloblen || info->codec_len < 4 ||
		sift_blob_kind(blob) != ((info->flags & SIFT_TERM_SPARSE) ? SIFT_LIST : SIFT_GROUPED) ||
		sift_blob_n(blob) != info->df || ((info->flags & SIFT_TERM_SPARSE) && blob[1] > SIFT_LIST_OB_MAX))
		corrupt_blob(info);
	/* the codec follows the blob's own lengths, so verify checks them before any cursor does */
	if (verify && (detail = sift_blob_check(blob, info->codec_len)) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: term blob at %llu is malformed", (unsigned long long) info->post_off),
				 errdetail_internal("%s.", detail)));
	if (c->bloblen > info->codec_len)
	{
		const uint8 *p = blob + ALIGN4(info->codec_len);

		if (!(info->flags & SIFT_TERM_SPARSE))
		{
			c->ng = BLOB_NG(blob);
			if (ALIGN4(info->codec_len) > c->bloblen || (uint64) c->ng * sizeof(SiftGroupScore) > c->bloblen - ALIGN4(info->codec_len))
				corrupt_blob(info);
			c->gs = (const SiftGroupScore *) p;
			c->blkrows = p + c->ng * sizeof(SiftGroupScore);
			p = c->blkrows;
			for (uint32 i = 0; i < c->ng; i++)
				p += (c->gs[i].flags & SIFT_GS_BLKBOUND) ? SIFT_BLKS_PER_SB : 0;
		}
		c->l1 = p;
		c->nib = p + L1_BYTES(info);
		c->esc = c->nib + NIB_BYTES(info);
		/* a term with a tf stream has its L1 bits and nibbles inside the blob (the escape bytes are the rest); one without has none */
		if (p > blob + c->bloblen ||
			(!(info->flags & SIFT_TERM_ALL_TF1) &&
			 (info->nnib == 0 || (uint64) (blob + c->bloblen - p) < L1_BYTES(info) + NIB_BYTES(info))))
			corrupt_blob(info);
	}
	else if (!(info->flags & SIFT_TERM_SPARSE) || !(info->flags & SIFT_TERM_ALL_TF1))
		corrupt_blob(info);
	sift_post_rewind(c);
	return c;
}

static SiftPostingCursor *
post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt, bool verify)
{
	uint8	   *blob = NULL;

	if (!(info->flags & SIFT_TERM_INLINE))
	{
		Size		len = blob_len(info);
		const void *src;

		POST_STAT(blobs, 1);
		POST_STAT(blob_bytes, len + SIFT_KERN_PAD);
		blob = MemoryContextAllocAligned(mcxt, len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
		src = sift_sr_get(sr, info->post_off, len, blob);
		if (src != blob)
			memcpy(blob, src, len);
		memset(blob + len, 0, SIFT_KERN_PAD);
	}
	return post_attach(info, mcxt, blob, sr, verify);
}

SiftPostingCursor *
sift_post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt)
{
	return post_open(sr, info, mcxt, false);
}

SiftPostingCursor *
sift_post_open_blob(const SiftTermInfo *info, const uint8 *blob, MemoryContext mcxt)
{
	return post_attach(info, mcxt, blob, NULL, false);
}

SiftPostingCursor *
sift_post_open_verified(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt)
{
	return post_open(sr, info, mcxt, true);
}

SiftPostingCursor *
sift_post_clone(const SiftPostingCursor *src, MemoryContext mcxt)
{
	SiftPostingCursor *c = MemoryContextAlloc(mcxt, sizeof(SiftPostingCursor));

	*c = *src;
	c->mcxt = mcxt;
	c->bounds = NULL;
	c->ckpt = NULL;
	c->win = (PosWin) {0};
	c->cblk = 0;
	c->curbuf = cursor_buf(mcxt, &src->info);
	sift_post_rewind(c);
	return c;
}

sift_cur *
sift_post_cursor(SiftPostingCursor *c)
{
	return &c->cur;
}

void
sift_post_group_stats(const SiftPostingCursor *c, uint32 *ng, uint32 *nrows)
{
	*ng = c->gs ? c->ng : 0;
	*nrows = 0;
	for (uint32 i = 0; c->gs && i < c->ng; i++)
		*nrows += (c->gs[i].flags & SIFT_GS_BLKBOUND) != 0;
}

void
sift_post_rewind(SiftPostingCursor *c)
{
	if (c->blob)
		sift_cur_blob(&c->cur, c->blob, c->info.codec_len, c->curbuf);
	else
		sift_cur_tids(&c->cur, c->info.inline_keys, c->info.df, c->curbuf);
	memset(&c->next, 0, sizeof(SiftTfPos));
}

const SiftGroupBounds *
sift_post_bounds(SiftPostingCursor *c, int *n)
{
	if (c->bounds == NULL && c->gs)
	{
		const uint8 *row = c->blkrows;

		c->bounds = MemoryContextAllocHuge(c->mcxt, Max(c->ng, 1) * sizeof(SiftGroupBounds));
		for (uint32 i = 0; i < c->ng; i++)
		{
			SiftGroupBounds *b = &c->bounds[i];

			b->gid = BLOB_KEYS(c->blob)[i] >> 8;
			b->bound = c->gs[i].bound;
			if (c->gs[i].flags & SIFT_GS_BLKBOUND)
			{
				memcpy(b->blk_bound, row, SIFT_BLKS_PER_SB);
				row += SIFT_BLKS_PER_SB;
			}
			else
				memset(b->blk_bound, b->bound, SIFT_BLKS_PER_SB);
		}
		c->nbounds = c->ng;
	}
	else if (c->bounds == NULL)
	{
		uint32	   *gid = palloc(Max(c->info.df, 1) * sizeof(uint32));
		uint32		lo = 0,
					hi = 0;

		sift_post_rewind(c);
		if (c->blob && c->info.df > SIFT_SPAN_MIN_DF)
			sift_cur_gid_span(&c->cur, &lo, &hi);
		if (c->info.df > hi - lo && c->info.df > SIFT_SPAN_MIN_DF)
		{
			/* at least a posting per SB on average: every SB between the first and the last, decoding nothing */
			c->nbounds = hi - lo + 1;
			for (uint32 i = 0; i < c->nbounds; i++)
				gid[i] = lo + i;
		}
		else
			c->nbounds = sift_cur_gids(&c->cur, gid);
		c->bounds = MemoryContextAlloc(c->mcxt, Max(c->nbounds, 1) * sizeof(SiftGroupBounds));
		for (uint32 i = 0; i < c->nbounds; i++)
		{
			c->bounds[i].gid = gid[i];
			c->bounds[i].bound = c->info.max_bound;
			memset(c->bounds[i].blk_bound, c->info.max_bound, SIFT_BLKS_PER_SB);
		}
		pfree(gid);
		sift_post_rewind(c);
	}
	*n = c->nbounds;
	return c->bounds;
}

/* Escape nibbles (15) among the first n nibbles, 16 per step */
static uint32
escapes(const uint8 *nib, uint32 n)
{
	uint32		esc = 0,
				i = 0;

	for (; i + 16 <= n; i += 16)
	{
		uint64		w;

		memcpy(&w, nib + i / 2, 8);
		w &= w >> 1;
		w &= w >> 2;
		esc += sift_pc64(w & 0x1111111111111111ULL);
	}
	for (; i < n; i++)
		esc += ((nib[i >> 1] >> ((i & 1) * 4)) & 15) == 15;
	return esc;
}

/* Escape nibbles among the n nibbles from nibble index a */
static uint32
escapes_from(const uint8 *nib, uint32 a, uint32 n)
{
	uint32		esc = 0;

	if ((a & 1) && n)
		esc = ((nib[a >> 1] >> 4) & 15) == 15, a++, n--;
	return esc + escapes(nib + a / 2, n);
}

/* Set bits among the n of l1 from bit r on: the nibbles the tf stream holds for those ranks */
static uint32
l1_count(const uint8 *l1, uint32 r, uint32 n)
{
	uint32		cnt = 0,
				head = (r % 8) ? Min(8 - r % 8, n) : 0;

	if (head)
		cnt = sift_pc64((l1[r / 8] >> (r % 8)) & ((1u << head) - 1)), r += head, n -= head;
	cnt += sift_popcount(l1 + r / 8, n / 8);
	if (n % 8)
		cnt += sift_pc64(l1[(r + n) / 8] & ((1u << (n % 8)) - 1));
	return cnt;
}

/* Stream position of rank r, counted from the start (LIST random access) */
static SiftTfPos
list_tf_pos(const SiftPostingCursor *c, uint32 r)
{
	SiftTfPos	pos = {.rank = r};

	pos.nib = sift_popcount(c->l1, r / 8);
	if (r % 8)
		pos.nib += sift_pc64(c->l1[r / 8] & ((1u << (r % 8)) - 1));
	if (pos.nib > c->info.nnib)
		sift_codec_corrupt("tf stream: the L1 bits count more nibbles than the stream holds");
	pos.esc = escapes(c->nib, pos.nib);
	return pos;
}

/*
 * The kernels read 64 bytes at the nibble and escape positions they reach, so the group's n ranks from pos must find the nibbles
 * their L1 bits count and the escape bytes of their escape nibbles inside the stream (the escapes are counted only when the
 * stream could end before them: the stream holds one escape byte per escape nibble of the term)
 */
static void
tf_check(const SiftPostingCursor *c, const SiftTfPos *pos, uint32 n)
{
	const SiftTermInfo *info = &c->info;
	uint64		elen = c->blob + c->bloblen - c->esc;
	uint32		nn;

	if (pos->nib > info->nnib)
		sift_codec_corrupt("tf stream: a group starts past the nibbles");
	nn = l1_count(c->l1, pos->rank, n);
	if (nn > info->nnib - pos->nib)
		sift_codec_corrupt("tf stream: the nibbles of a group are not inside the stream");
	if (pos->esc > elen || (nn > elen - pos->esc && escapes_from(c->nib, pos->nib, nn) > elen - pos->esc))
		sift_codec_corrupt("tf stream: the escape bytes of a group are not inside the stream");
}

void
sift_post_group_tf(SiftPostingCursor *c, uint8 *tf)
{
	const sift_grp *g = &c->cur.g;
	uint32		n = g->n ? g->n : (uint32) sift_grp_count(g);
	SiftTfPos	pos;

	if (c->blob == NULL)
	{
		memcpy(tf, c->info.inline_tf + g->base, n);
		return;
	}
	if ((c->info.flags & SIFT_TERM_ALL_TF1) || (c->gs && (c->gs[c->cur.gr.i].flags & SIFT_GS_ALL_TF1)))
	{
		memset(tf, 1, n);
		return;
	}
	if ((uint64) g->base + n > c->info.df)
		sift_codec_corrupt("tf stream: a group lies past the last posting");
	if (c->gs)
		pos = (SiftTfPos) {g->base, c->gs[c->cur.gr.i].nib_base, c->gs[c->cur.gr.i].esc_base};
	else
		pos = c->next.rank == g->base ? c->next : list_tf_pos(c, g->base);
	tf_check(c, &pos, n);
	sift_kern->tf_decode(tf, c->l1, c->nib, c->esc, &pos, n);
	c->next = pos;
}

static inline uint32
nibble(const uint8 *nib, uint32 i)
{
	return (nib[i >> 1] >> ((i & 1) * 4)) & 15;
}

void
sift_post_check(SiftPostingCursor *c)
{
	const SiftTermInfo *info = &c->info;
	const uint8 *end = c->blob + c->bloblen;
	const uint32 *base = c->gs ? BLOB_KEYS(c->blob) + 2 * c->ng + 1 : NULL;
	uint32		nib = 0,
				esc = 0;

	if (c->blob == NULL)
	{
		for (uint32 i = 0; i < info->df; i++)
			if (info->inline_tf[i] == 0 || (i > 0 && info->inline_keys[i] <= info->inline_keys[i - 1]) ||
				((info->flags & SIFT_TERM_ALL_TF1) && info->inline_tf[i] != 1))
				corrupt_blob(info);
		return;
	}
	if ((info->flags & SIFT_TERM_ALL_TF1) ? info->nnib != 0 || (c->l1 && c->l1 != end) : c->l1 == NULL || info->nnib == 0)
		corrupt_blob(info);
	if (c->gs && BLOB_NG(c->blob) != c->ng)
		corrupt_blob(info);
	for (uint32 g = 0, r = 0; g < Max(c->ng, 1); g++)
	{
		uint32		hi = c->gs ? base[g + 1] : info->df;
		bool		tf1 = true;

		if (c->gs && (base[g] != r || hi > info->df || c->gs[g].nib_base != nib || c->gs[g].esc_base != esc))
			corrupt_blob(info);
		for (; r < hi && !(info->flags & SIFT_TERM_ALL_TF1); r++)
			if ((c->l1[r >> 3] >> (r & 7)) & 1)
			{
				if (nib >= info->nnib)
					corrupt_blob(info);
				tf1 = false;
				esc += nibble(c->nib, nib++) == 15;
			}
		r = hi;
		if (c->gs && tf1 != ((c->gs[g].flags & SIFT_GS_ALL_TF1) != 0))
			corrupt_blob(info);
	}
	if (!(info->flags & SIFT_TERM_ALL_TF1) &&
		(nib != info->nnib || c->esc + esc != end ||
		 (info->df % 8 && c->l1[info->df / 8] >> (info->df % 8)) ||
		 (info->nnib % 2 && nibble(c->nib, info->nnib) != 0)))
		corrupt_blob(info);
}
