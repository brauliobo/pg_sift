/*
 * sift_posting.c - term blob writer and reader (layout in sift_posting.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "sift_kernels.h"
#include "sift_posting.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define BLOB_NG(blob)	(*(const uint32 *) ((blob) + 8))
#define BLOB_KEYS(blob)	((const uint32 *) ((blob) + 12))

int			sift_grouped_min_df = SIFT_GROUPED_MIN_DF;

struct SiftPostingWriter
{
	SiftStreamWriter *sw;
	MemoryContext mcxt;
	SiftBm25	ref;
	SiftKey    *keys;
	uint8	   *tf;
	uint8	   *bound;
	uint32		n;
	uint32		cap;
	uint8	   *out;
	Size		outcap;
	uint8	   *pos;			/* position records of the term's postings, back to back */
	Size		poslen;
	Size		poscap;
	uint32	   *ckpt;			/* offset in pos of every SIFT_POS_EVERY-th posting */
	bool		haspos;
	uint8		bound1[SIFT_MAX_DL + 1];	/* bound for tf 1 by dl; 0 = not computed */
};

struct SiftPostingCursor
{
	SiftTermInfo info;
	sift_cur	cur;
	uint8	   *curbuf;
	const uint8 *blob;
	const SiftGroupScore *gs;	/* GROUPED */
	uint32		ng;
	const uint8 *blkrows;
	const uint8 *l1;
	const uint8 *nib;
	const uint8 *esc;
	Size		bloblen;		/* the blob without its position stream */
	SiftStreamReader *sr;		/* the position stream is read on demand */
	uint64		posoff;			/* its offset in the segment stream */
	uint32		poslen;
	uint32		nck;
	uint32	   *ckpt;			/* checkpoints, read with the first position */
	uint8	   *win;			/* window of the records */
	uint64		win_lo;			/* offset in the records */
	uint32		win_len;
	SiftTfPos	next;			/* LIST: where the last decoded group ended */
	SiftGroupBounds *bounds;
	int			nbounds;
	MemoryContext mcxt;
};

uint8
sift_bound_u8(const SiftScoreRef *ref, uint32 tf, uint32 dl)
{
	SiftBm25	bm = {.k1 = ref->k1,.b = ref->b,.avgdl = ref->avgdl};

	return sift_kern_bound_u8(&bm, tf, dl);
}

/* ---------- writer ---------- */

SiftPostingWriter *
sift_postw_begin(SiftStreamWriter *sw, const SiftScoreRef *ref, MemoryContext mcxt)
{
	SiftPostingWriter *w = MemoryContextAllocZero(mcxt, sizeof(SiftPostingWriter));

	w->sw = sw;
	w->mcxt = mcxt;
	w->ref.k1 = ref->k1;
	w->ref.b = ref->b;
	w->ref.avgdl = ref->avgdl;
	return w;
}

static void *
grow(MemoryContext mcxt, void *p, Size sz)
{
	return p ? repalloc_huge(p, sz) : MemoryContextAllocHuge(mcxt, sz);
}

static inline uint8
bound_of(SiftPostingWriter *w, uint8 tf, uint16 dl)
{
	if (tf > 1)
		return sift_kern_bound_u8(&w->ref, tf, dl);
	if (w->bound1[dl] == 0)
		w->bound1[dl] = sift_kern_bound_u8(&w->ref, 1, dl);
	return w->bound1[dl];
}

void
sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n,
			   const uint8 *pos, const uint32 *poslen)
{
	if (n <= 0)
		return;
	if (w->n + n > w->cap)
	{
		w->cap = Max(w->n + n, Max(1024, w->cap * 2));
		w->keys = grow(w->mcxt, w->keys, (Size) w->cap * sizeof(SiftKey));
		w->tf = grow(w->mcxt, w->tf, w->cap);
		w->bound = grow(w->mcxt, w->bound, w->cap);
		w->ckpt = grow(w->mcxt, w->ckpt, (Size) ((w->cap + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32));
	}
	if (w->n == 0)
		w->haspos = pos != NULL;
	else if (w->haspos != (pos != NULL))
		elog(ERROR, "sift: postings of a term must all have positions or none");
	for (int i = 0; i < n; i++)
	{
		if ((w->n > 0 || i > 0) && keys[i] <= (i > 0 ? keys[i - 1] : w->keys[w->n - 1]))
			elog(ERROR, "sift: postings of a term must arrive in ascending key order");
		if (tf[i] == 0)
			elog(ERROR, "sift: posting with tf 0");
		w->bound[w->n + i] = bound_of(w, tf[i], dl[i]);
	}
	memcpy(w->keys + w->n, keys, (Size) n * sizeof(SiftKey));
	memcpy(w->tf + w->n, tf, (Size) n);
	if (pos)
	{
		Size		len = 0;

		for (int i = 0; i < n; i++)
		{
			if ((w->n + i) % SIFT_POS_EVERY == 0)
				w->ckpt[(w->n + i) / SIFT_POS_EVERY] = w->poslen + len;
			len += poslen[i];
		}
		if (w->poslen + len > w->poscap)
		{
			w->poscap = Max(w->poslen + len, Max(4096, w->poscap * 2));
			w->pos = grow(w->mcxt, w->pos, w->poscap);
		}
		memcpy(w->pos + w->poslen, pos, len);
		w->poslen += len;
	}
	w->n += n;
}

static uint8 *
out_buf(SiftPostingWriter *w, Size len)
{
	if (len > w->outcap)
	{
		w->outcap = Max(len, Max(4096, w->outcap * 2));
		if (w->out)
			pfree(w->out);
		w->out = MemoryContextAllocHuge(w->mcxt, w->outcap);
	}
	memset(w->out, 0, len);
	return w->out;
}

/* Group scores and block bound rows of a GROUPED term, in codec group order */
static void
fill_groups(SiftPostingWriter *w, SiftGroupScore *gs, uint8 *rows)
{
	uint32		nib = 0,
				esc = 0;

	for (uint32 i = 0, g = 0; i < w->n; g++)
	{
		uint32		gid = SIFT_GID(w->keys[i]),
					j = i;
		uint8		blk[SIFT_BLKS_PER_SB] = {0};
		bool		tf1 = true;

		gs[g].nib_base = nib;
		gs[g].esc_base = esc;
		for (; j < w->n && SIFT_GID(w->keys[j]) == gid; j++)
		{
			int			b = (SIFT_BLK(w->keys[j]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT;

			blk[b] = Max(blk[b], w->bound[j]);
			if (w->tf[j] > 1)
			{
				tf1 = false;
				nib++;
				esc += w->tf[j] >= 17;
			}
		}
		for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
			gs[g].bound = Max(gs[g].bound, blk[b]);
		gs[g].flags = tf1 ? SIFT_GS_ALL_TF1 : 0;
		if (j - i >= SIFT_BLKBOUND_MIN)
		{
			gs[g].flags |= SIFT_GS_BLKBOUND;
			memcpy(rows, blk, SIFT_BLKS_PER_SB);
			rows += SIFT_BLKS_PER_SB;
		}
		i = j;
	}
}

void
sift_postw_end_term(SiftPostingWriter *w, SiftTermInfo *info)
{
	uint32		n = w->n,
				nesc = 0,
				ng = 0,
				nrows = 0;
	sift_enc_opts opts = {0};
	Size		clen,
				side,
				total,
				posoff = 0;
	uint8	   *out;

	if (n == 0)
		elog(ERROR, "sift: term without postings");
	memset(info, 0, sizeof(SiftTermInfo));
	info->df = n;
	for (uint32 i = 0; i < n; i++)
	{
		info->max_bound = Max(info->max_bound, w->bound[i]);
		info->nnib += w->tf[i] > 1;
		nesc += w->tf[i] >= 17;
	}
	if (info->nnib == 0)
		info->flags |= SIFT_TERM_ALL_TF1;
	if (n <= SIFT_INLINE_MAX)
	{
		info->flags |= SIFT_TERM_INLINE;
		memcpy(info->inline_keys, w->keys, n * sizeof(SiftKey));
		memcpy(info->inline_tf, w->tf, n);
		if (w->haspos && w->poslen <= SIFT_INLINE_POS_MAX)
		{
			info->flags |= SIFT_TERM_POS;
			info->pos_len = w->poslen;
			info->inline_pos = w->pos;
		}
		w->n = 0;
		w->poslen = 0;
		return;
	}
	opts.kind = n >= (uint32) sift_grouped_min_df ? SIFT_GROUPED : SIFT_LIST;
	if (opts.kind == SIFT_LIST)
		info->flags |= SIFT_TERM_SPARSE;
	else
	{
		for (uint32 i = 0, j; i < n; i = j, ng++)
		{
			for (j = i; j < n && SIFT_GID(w->keys[j]) == SIFT_GID(w->keys[i]); j++)
				;
			nrows += j - i >= SIFT_BLKBOUND_MIN;
		}
	}
	clen = sift_encode(w->keys, NULL, NULL, n, &opts, NULL);
	side = (Size) ng * sizeof(SiftGroupScore) + (Size) nrows * SIFT_BLKS_PER_SB +
		(info->nnib ? (n + 7) / 8 + (info->nnib + 1) / 2 + nesc : 0);
	total = side ? ALIGN4(clen) + side : clen;
	if (w->haspos)
	{
		posoff = total;
		total = posoff + (Size) ((n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32) + w->poslen;
		info->flags |= SIFT_TERM_POS;
		info->pos_len = total - posoff;
	}
	if (total > PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: postings of one term exceed 4 GB")));
	out = out_buf(w, total);
	if (sift_encode(w->keys, NULL, NULL, n, &opts, out) != clen)
		elog(ERROR, "sift: codec size mismatch");
	if (side)
	{
		uint8	   *p = out + ALIGN4(clen);

		if (ng)
		{
			fill_groups(w, (SiftGroupScore *) p, p + ng * sizeof(SiftGroupScore));
			p += ng * sizeof(SiftGroupScore) + nrows * SIFT_BLKS_PER_SB;
		}
		if (info->nnib)
		{
			SiftTfPos	pos = {0};
			uint8	   *nib = p + (n + 7) / 8;

			sift_kern_tf_encode(p, nib, nib + (info->nnib + 1) / 2, &pos, w->tf, n);
		}
	}
	if (w->haspos)
	{
		Size		nck = (Size) ((n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32);

		memcpy(out + posoff, w->ckpt, nck);
		memcpy(out + posoff + nck, w->pos, w->poslen);
	}
	sift_sw_write(w->sw, "\0\0\0", ALIGN4(sift_sw_tell(w->sw)) - sift_sw_tell(w->sw));
	info->post_off = sift_sw_tell(w->sw);
	info->post_len = total;
	info->codec_len = clen;
	sift_sw_write(w->sw, out, total);
	w->n = 0;
	w->poslen = 0;
}

/* ---------- reader ---------- */

static void
corrupt_blob(const SiftTermInfo *info)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: term blob at %llu does not match its dictionary entry",
					(unsigned long long) info->post_off)));
}

static SiftPostingCursor *
post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt, bool verify)
{
	SiftPostingCursor *c = MemoryContextAllocZero(mcxt, sizeof(SiftPostingCursor));
	uint8	   *blob;
	const void *src;
	const char *detail;

	c->info = *info;
	c->mcxt = mcxt;
	c->curbuf = MemoryContextAllocAligned(mcxt, SIFT_CUR_BUF, SIFT_SIMD_ALIGN, 0);
	if (info->flags & SIFT_TERM_INLINE)
	{
		if (info->df == 0 || info->df > SIFT_INLINE_MAX)
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
		if (info->pos_len > info->post_len || info->post_len - info->pos_len < info->codec_len ||
			(uint64) c->nck * sizeof(uint32) > info->pos_len)
			corrupt_blob(info);
	}
	c->bloblen = info->post_len - c->poslen;
	c->sr = sr;
	c->posoff = info->post_off + c->bloblen;
	blob = MemoryContextAllocAligned(mcxt, c->bloblen + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	src = sift_sr_get(sr, info->post_off, c->bloblen, blob);
	if (src != blob)
		memcpy(blob, src, c->bloblen);
	memset(blob + c->bloblen, 0, SIFT_KERN_PAD);
	c->blob = blob;
	if (info->codec_len > c->bloblen || info->codec_len < 4 ||
		sift_blob_kind(blob) != ((info->flags & SIFT_TERM_SPARSE) ? SIFT_LIST : SIFT_GROUPED) ||
		sift_blob_n(blob) != info->df)
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
			if ((uint64) c->ng * sizeof(SiftGroupScore) > c->bloblen - ALIGN4(info->codec_len))
				corrupt_blob(info);
			c->gs = (const SiftGroupScore *) p;
			c->blkrows = p + c->ng * sizeof(SiftGroupScore);
			p = c->blkrows;
			for (uint32 i = 0; i < c->ng; i++)
				p += (c->gs[i].flags & SIFT_GS_BLKBOUND) ? SIFT_BLKS_PER_SB : 0;
		}
		c->l1 = p;
		c->nib = p + (info->df + 7) / 8;
		c->esc = c->nib + (info->nnib + 1) / 2;
		if ((info->nnib && c->esc > blob + c->bloblen) || p > blob + c->bloblen)
			corrupt_blob(info);
	}
	else if (!(info->flags & SIFT_TERM_SPARSE) || !(info->flags & SIFT_TERM_ALL_TF1))
		corrupt_blob(info);
	sift_post_rewind(c);
	return c;
}

SiftPostingCursor *
sift_post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt)
{
	return post_open(sr, info, mcxt, false);
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
	c->win = NULL;
	c->win_len = 0;
	c->curbuf = MemoryContextAllocAligned(mcxt, SIFT_CUR_BUF, SIFT_SIMD_ALIGN, 0);
	sift_post_rewind(c);
	return c;
}

sift_cur *
sift_post_cursor(SiftPostingCursor *c)
{
	return &c->cur;
}

void
sift_post_rewind(SiftPostingCursor *c)
{
	if (c->blob)
		sift_cur_blob(&c->cur, c->blob, c->curbuf);
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

		sift_post_rewind(c);
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

/* Stream position of rank r, counted from the start (LIST random access) */
static SiftTfPos
list_tf_pos(const SiftPostingCursor *c, uint32 r)
{
	SiftTfPos	pos = {.rank = r};

	pos.nib = sift_popcount(c->l1, r / 8);
	if (r % 8)
		pos.nib += sift_pc64(c->l1[r / 8] & ((1u << (r % 8)) - 1));
	pos.esc = escapes(c->nib, pos.nib);
	return pos;
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
	if (c->gs)
		pos = (SiftTfPos) {g->base, c->gs[c->cur.gr.i].nib_base, c->gs[c->cur.gr.i].esc_base};
	else
		pos = c->next.rank == g->base ? c->next : list_tf_pos(c, g->base);
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

/* ---------- positions ---------- */

static void
corrupt_pos(const SiftPostingCursor *c)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: position stream of the term blob at %llu is malformed",
					(unsigned long long) c->info.post_off)));
}

/* LEB128 at *pp; false when it runs past end */
static inline bool
get_v32(const uint8 **pp, const uint8 *end, uint32 *x)
{
	const uint8 *p = *pp;
	uint32		v = 0;

	for (int s = 0; s < 35; s += 7)
	{
		if (p >= end)
			return false;
		v |= (uint32) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
		{
			*pp = p;
			*x = v;
			return true;
		}
	}
	return false;
}

/* End of the record at p and what it holds; NULL when it is cut off by end */
static const uint8 *
pos_next(const uint8 *p, const uint8 *end, uint32 *n, uint32 *last)
{
	uint32		v,
				cnt = 1,
				pos;

	if (!get_v32(&p, end, &v))
		return NULL;
	pos = v >> 1;
	if (v & 1)
	{
		if (!get_v32(&p, end, &v) || (uint64) v + 1 > (uint64) (end - p))
			return NULL;
		cnt = v + 2;
	}
	for (uint32 i = 1; i < cnt; i++)
	{
		if (!get_v32(&p, end, &v))
			return NULL;
		pos += v + 1;
	}
	*n = cnt;
	*last = pos;
	return p;
}

const uint8 *
sift_pos_skip(const uint8 *p, const uint8 *end, uint32 *n, uint32 *last)
{
	const uint8 *q = pos_next(p, end, n, last);

	if (q == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: truncated position record")));
	return q;
}

bool
sift_post_has_positions(const SiftPostingCursor *c)
{
	return c->poslen > 0;
}

/* Reads len bytes of the position stream at off into dst, and lets go of the page */
static void
pos_read(const SiftPostingCursor *c, uint64 off, size_t len, void *dst)
{
	const void *src = sift_sr_get(c->sr, c->posoff + off, len, dst);

	if (src != dst)
		memcpy(dst, src, len);
	sift_sr_release(c->sr);
}

#define POS_WINDOW	4096

/* The window of records covering [off, off + want), refilled from off when it does not; bytes available from off */
static const uint8 *
pos_window(SiftPostingCursor *c, uint64 off, uint32 want, uint32 *avail)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32);

	if (off > recs)
		corrupt_pos(c);
	if (c->win == NULL)
		c->win = MemoryContextAlloc(c->mcxt, POS_WINDOW);
	if (c->win_len == 0 || off < c->win_lo || (off + want > c->win_lo + c->win_len && c->win_lo + c->win_len < recs))
	{
		uint64		have = Min(recs - off, POS_WINDOW);

		if (want > POS_WINDOW)
		{
			/* a record longer than the window: the caller reads it whole */
			*avail = 0;
			return NULL;
		}
		pos_read(c, (uint64) c->nck * sizeof(uint32) + off, have, c->win);
		c->win_lo = off;
		c->win_len = have;
	}
	*avail = c->win_lo + c->win_len - off;
	return c->win + (off - c->win_lo);
}

static void
pos_ckpt(SiftPostingCursor *c)
{
	if (c->ckpt)
		return;
	c->ckpt = MemoryContextAlloc(c->mcxt, (Size) c->nck * sizeof(uint32));
	pos_read(c, 0, (Size) c->nck * sizeof(uint32), c->ckpt);
}

/* Appends the n occurrences of the record at p (fully inside end) to out */
static void
pos_decode(const uint8 *p, const uint8 *end, uint32 n, SiftPosBuf *out)
{
	uint32		v = 0,
				pos;

	if (n > out->cap)
	{
		out->cap = Max(n, Max(64, out->cap * 2));
		out->v = out->v ? repalloc_huge(out->v, (Size) out->cap * sizeof(uint32)) :
			MemoryContextAllocHuge(out->mcxt, (Size) out->cap * sizeof(uint32));
	}
	get_v32(&p, end, &v);
	pos = v >> 1;
	if (v & 1)
		get_v32(&p, end, &v);
	out->v[0] = pos;
	for (uint32 i = 1; i < n; i++)
	{
		get_v32(&p, end, &v);
		out->v[i] = pos += v + 1;
	}
	out->n = n;
}

void
sift_post_positions(SiftPostingCursor *c, uint32 rank, SiftPosBuf *out)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32),
				off;
	uint32		want = 512;

	Assert(c->poslen > 0 && rank < c->info.df);
	if (c->info.flags & SIFT_TERM_INLINE)
	{
		const uint8 *p = c->info.inline_pos,
				   *end = p + c->poslen;
		uint32		n,
					last;

		for (uint32 k = 0; k < rank; k++)
			p = sift_pos_skip(p, end, &n, &last);
		end = sift_pos_skip(p, end, &n, &last);
		pos_decode(p, end, n, out);
		return;
	}
	pos_ckpt(c);
	off = c->ckpt[rank >> SIFT_POS_SHIFT];
	for (;; want *= 2)
	{
		uint32		avail,
					n = 0,
					last;
		const uint8 *p = pos_window(c, off, Min(want, recs - off), &avail),
				   *end,
				   *q;
		uint8	   *big = NULL;

		if (p == NULL)
		{
			/* longer than the window: read what is wanted at once */
			avail = Min(want, recs - off);
			big = palloc(avail);
			pos_read(c, (uint64) c->nck * sizeof(uint32) + off, avail, big);
			p = big;
		}
		end = p + avail;
		q = p;
		for (uint32 k = rank & (SIFT_POS_EVERY - 1); k > 0 && q; k--)
			q = pos_next(q, end, &n, &last);
		if (q && (end = pos_next(q, end, &n, &last)) != NULL)
		{
			pos_decode(q, end, n, out);
			if (big)
				pfree(big);
			return;
		}
		if (big)
			pfree(big);
		if (avail >= recs - off)
			corrupt_pos(c);
	}
}

void
sift_post_pos_stream(SiftPostingCursor *c, const uint32 **ckpt, const uint8 **rec, uint32 *len)
{
	uint8	   *buf;

	if (c->info.flags & SIFT_TERM_INLINE)
	{
		*ckpt = NULL;
		*rec = c->info.inline_pos;
		*len = c->poslen;
		return;
	}
	buf = MemoryContextAllocHuge(c->mcxt, c->poslen);
	pos_read(c, 0, c->poslen, buf);
	*ckpt = (const uint32 *) buf;
	*rec = buf + (Size) c->nck * sizeof(uint32);
	*len = c->poslen - c->nck * sizeof(uint32);
}
