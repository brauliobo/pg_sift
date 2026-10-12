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
sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n)
{
	if (w->n + n > w->cap)
	{
		w->cap = Max(w->n + n, Max(1024, w->cap * 2));
		w->keys = grow(w->mcxt, w->keys, (Size) w->cap * sizeof(SiftKey));
		w->tf = grow(w->mcxt, w->tf, w->cap);
		w->bound = grow(w->mcxt, w->bound, w->cap);
	}
	for (int i = 0; i < n; i++)
	{
		if ((w->n > 0 || i > 0) && keys[i] <= (i > 0 ? keys[i - 1] : w->keys[w->n - 1]))
			elog(ERROR, "sift: postings of a term must arrive in ascending key order");
		if (tf[i] == 0)
			elog(ERROR, "sift: posting with tf 0");
		w->bound[w->n + i] = bound_of(w, tf[i], dl[i]);
	}
	memcpy(w->keys + w->n, keys, n * sizeof(SiftKey));
	memcpy(w->tf + w->n, tf, n);
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
				total;
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
		w->n = 0;
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
	sift_sw_write(w->sw, "\0\0\0", ALIGN4(sift_sw_tell(w->sw)) - sift_sw_tell(w->sw));
	info->post_off = sift_sw_tell(w->sw);
	info->post_len = total;
	info->codec_len = clen;
	sift_sw_write(w->sw, out, total);
	w->n = 0;
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
		sift_post_rewind(c);
		return c;
	}
	blob = MemoryContextAllocAligned(mcxt, (Size) info->post_len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN,
									 MCXT_ALLOC_HUGE);
	src = sift_sr_get(sr, info->post_off, info->post_len, blob);
	if (src != blob)
		memcpy(blob, src, info->post_len);
	memset(blob + info->post_len, 0, SIFT_KERN_PAD);
	c->blob = blob;
	if (info->codec_len > info->post_len || info->codec_len < 4 ||
		sift_blob_kind(blob) != ((info->flags & SIFT_TERM_SPARSE) ? SIFT_LIST : SIFT_GROUPED) ||
		sift_blob_n(blob) != info->df)
		corrupt_blob(info);
	/* the codec follows the blob's own lengths, so verify checks them before any cursor does */
	if (verify && (detail = sift_blob_check(blob, info->codec_len)) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: term blob at %llu is malformed", (unsigned long long) info->post_off),
				 errdetail_internal("%s.", detail)));
	if (info->post_len > info->codec_len)
	{
		const uint8 *p = blob + ALIGN4(info->codec_len);

		if (!(info->flags & SIFT_TERM_SPARSE))
		{
			c->ng = BLOB_NG(blob);
			if ((uint64) c->ng * sizeof(SiftGroupScore) > info->post_len - ALIGN4(info->codec_len))
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
		if ((info->nnib && c->esc > blob + info->post_len) || p > blob + info->post_len)
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
		int			cap = 16;

		c->bounds = MemoryContextAlloc(c->mcxt, cap * sizeof(SiftGroupBounds));
		sift_post_rewind(c);
		for (const sift_grp *g = sift_next(&c->cur); g; g = sift_next(&c->cur))
		{
			if (c->nbounds == cap)
			{
				cap *= 2;
				c->bounds = repalloc_huge(c->bounds, cap * sizeof(SiftGroupBounds));
			}
			c->bounds[c->nbounds].gid = g->gid;
			c->bounds[c->nbounds].bound = c->info.max_bound;
			memset(c->bounds[c->nbounds++].blk_bound, c->info.max_bound, SIFT_BLKS_PER_SB);
		}
		sift_post_rewind(c);
	}
	*n = c->nbounds;
	return c->bounds;
}

/* Stream position of rank r, counted from the start (LIST random access) */
static SiftTfPos
list_tf_pos(const SiftPostingCursor *c, uint32 r)
{
	SiftTfPos	pos = {.rank = r};

	pos.nib = sift_popcount(c->l1, r / 8);
	if (r % 8)
		pos.nib += __builtin_popcount(c->l1[r / 8] & ((1u << (r % 8)) - 1));
	for (uint32 i = 0; i < pos.nib; i++)
		pos.esc += ((c->nib[i >> 1] >> ((i & 1) * 4)) & 15) == 15;
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
	const uint8 *end = c->blob + info->post_len;
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
		(nib != info->nnib || c->esc + esc != end || (info->df % 8 && c->l1[info->df / 8] >> (info->df % 8)) ||
		 (info->nnib % 2 && nibble(c->nib, info->nnib) != 0)))
		corrupt_blob(info);
}
