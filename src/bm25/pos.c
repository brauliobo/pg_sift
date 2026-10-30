/*
 * bm25/pos.c - position records of a term blob: the checkpoints, the windowed reads and the record decoder
 * (layout in bm25/posting.h).
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "bm25/posting.h"
#include "bm25/posting_int.h"

static void
corrupt_pos(const SiftPostingCursor *c)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: position stream of the term blob at %llu is malformed",
					(unsigned long long) c->info.post_off)));
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
#define POS_SLACK	8

/*
 * The bytes [off, off + want) of the records in w, read from off when they are not there (the window grows to hold a record
 * longer than itself); avail: the bytes from off in the window
 */
static const uint8 *
pos_window(const SiftPostingCursor *c, PosWin *w, uint64 off, uint32 want, uint32 *avail)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32);

	if (off > recs)
		corrupt_pos(c);
	if (w->buf == NULL || want > w->cap)
	{
		if (w->buf)
			pfree(w->buf);
		w->cap = Max(want, w->cap ? w->cap : POS_WINDOW);
		w->buf = MemoryContextAlloc(c->mcxt, (Size) w->cap + POS_SLACK);
		memset(w->buf + w->cap, 0, POS_SLACK);
		w->len = 0;
	}
	if (w->len == 0 || off < w->lo || (off + want > w->lo + w->len && w->lo + w->len < recs))
	{
		uint64		have = Min(recs - off, w->cap);

		pos_read(c, (uint64) c->nck * sizeof(uint32) + off, have, w->buf);
		w->lo = off;
		w->len = have;
	}
	*avail = w->lo + w->len - off;
	return w->buf + (off - w->lo);
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

	sift_posbuf_reserve(out, n);
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

/* The block blk of the term (its postings: *k), in w: the checkpoints are the offsets of the blocks */
static const uint8 *
pos_block(SiftPostingCursor *c, PosWin *w, uint32 blk, uint32 *k, uint32 *len)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32),
				off,
				end;
	uint32		avail;
	const uint8 *p;

	pos_ckpt(c);
	off = c->ckpt[blk];
	end = blk + 1 < c->nck ? c->ckpt[blk + 1] : recs;
	if (end <= off || end > recs || end - off > PG_UINT32_MAX / 2)
		corrupt_pos(c);
	*len = end - off;
	*k = Min(SIFT_POS_EVERY, c->info.df - blk * SIFT_POS_EVERY);
	p = pos_window(c, w, off, *len, &avail);
	if (avail < *len)
		corrupt_pos(c);
	return p;
}

static void
pos_posting_block(SiftPostingCursor *c, uint32 rank, SiftPosBuf *out)
{
	uint32		blk = rank >> SIFT_POS_SHIFT,
				i = rank & (SIFT_POS_EVERY - 1);

	if (c->cblk != blk + 1)
	{
		uint32		k,
					len;
		const uint8 *p = pos_block(c, &c->win, blk, &k, &len);

		if (!sift_posblk_open(&c->bk, p, len, k))
			corrupt_pos(c);
		c->cblk = blk + 1;
	}
	sift_posbuf_reserve(out, c->bk.cnt[i]);
	if (!sift_posblk_get(&c->bk, i, out->v))
		corrupt_pos(c);
	out->n = c->bk.cnt[i];
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
	if (c->info.flags & SIFT_TERM_POS2)
	{
		pos_posting_block(c, rank, out);
		return;
	}
	pos_ckpt(c);
	off = c->ckpt[rank >> SIFT_POS_SHIFT];
	for (;; want *= 2)
	{
		uint32		avail,
					n = 0,
					last;
		const uint8 *p = pos_window(c, &c->win, off, Min(want, recs - off), &avail),
				   *end,
				   *q = p;

		end = p + avail;
		for (uint32 k = rank & (SIFT_POS_EVERY - 1); k > 0 && q; k--)
			q = pos_next(q, end, &n, &last);
		if (q && (end = pos_next(q, end, &n, &last)) != NULL)
		{
			pos_decode(q, end, n, out);
			return;
		}
		if (avail >= recs - off)
			corrupt_pos(c);
	}
}

/* All the blocks of the term as the records of the postings in rank order, and the checkpoints of those */
static void
pos_stream_blocks(SiftPostingCursor *c, const uint8 *blocks, uint32 **ckpt, StringInfo recs)
{
	uint64		total = c->poslen - (uint64) c->nck * sizeof(uint32),
				start = 0;
	Size		lend[SIFT_POS_EVERY];
	const uint32 *at = (const uint32 *) blocks;

	*ckpt = MemoryContextAlloc(c->mcxt, (Size) c->nck * sizeof(uint32));
	for (uint32 b = 0; b < c->nck; b++)
	{
		uint64		end = b + 1 < c->nck ? at[b + 1] : total;

		if (at[b] != start || end <= start || end > total)
			corrupt_pos(c);
		(*ckpt)[b] = recs->len;
		if (!sift_posblk_leb(blocks + (Size) c->nck * sizeof(uint32) + start, end - start,
							 Min(SIFT_POS_EVERY, c->info.df - b * SIFT_POS_EVERY), recs, lend))
			corrupt_pos(c);
		start = end;
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
	buf = MemoryContextAllocHuge(c->mcxt, (Size) c->poslen + POS_SLACK);
	pos_read(c, 0, c->poslen, buf);
	if (c->info.flags & SIFT_TERM_POS2)
	{
		StringInfoData recs;
		MemoryContext old = MemoryContextSwitchTo(c->mcxt);
		uint32	   *ck;

		initStringInfo(&recs);
		pos_stream_blocks(c, buf, &ck, &recs);
		MemoryContextSwitchTo(old);
		pfree(buf);
		*ckpt = ck;
		*rec = (const uint8 *) recs.data;
		*len = recs.len;
		return;
	}
	*ckpt = (const uint32 *) buf;
	*rec = buf + (Size) c->nck * sizeof(uint32);
	*len = c->poslen - c->nck * sizeof(uint32);
}

struct SiftPosReader
{
	SiftPostingCursor *c;
	uint8	   *buf;			/* the window; the records of an inline term themselves */
	Size		cap;
	Size		pos;			/* of the next record in the window */
	Size		len;			/* bytes in the window */
	uint64		next;			/* offset in the records of the byte after the window */
	uint64		recs;			/* bytes of records */
	/* position blocks: the one decoded, as records */
	PosWin		win;
	StringInfoData leb;
	uint32		blk;			/* of the next block */
	uint32		npost;			/* postings of the decoded block, and how many were given */
	uint32		given;
	Size		lend[SIFT_POS_EVERY];	/* end of each one's record in leb */
};

#define POS_READER_WINDOW	65536

SiftPosReader *
sift_post_pos_reader(SiftPostingCursor *c, MemoryContext mcxt)
{
	SiftPosReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftPosReader));

	r->c = c;
	if (c->info.flags & SIFT_TERM_INLINE)
	{
		r->buf = (uint8 *) c->info.inline_pos;
		r->len = r->next = r->recs = c->poslen;
		return r;
	}
	r->recs = c->poslen - (uint64) c->nck * sizeof(uint32);
	if (c->info.flags & SIFT_TERM_POS2)
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		r->win.cap = Min(POS_READER_WINDOW, r->recs);
		initStringInfo(&r->leb);
		MemoryContextSwitchTo(old);
		return r;
	}
	r->cap = POS_READER_WINDOW;
	r->buf = MemoryContextAlloc(mcxt, r->cap);
	return r;
}

/* The next record of a term with position blocks: the postings of the next block turn into records when the last one was given */
static const uint8 *
pos_next_block(SiftPosReader *r, uint32 *len)
{
	if (r->given == r->npost)
	{
		uint32		k,
					blen;
		const uint8 *p;

		if (r->blk >= r->c->nck)
			corrupt_pos(r->c);
		p = pos_block(r->c, &r->win, r->blk, &k, &blen);
		resetStringInfo(&r->leb);
		if (!sift_posblk_leb(p, blen, k, &r->leb, r->lend))
			corrupt_pos(r->c);
		r->npost = k;
		r->blk++;
		r->given = 0;
	}
	*len = r->lend[r->given] - (r->given ? r->lend[r->given - 1] : 0);
	return (const uint8 *) r->leb.data + (r->given ? r->lend[r->given - 1] : 0);
}

const uint8 *
sift_post_pos_next(SiftPosReader *r, uint32 *len)
{
	if (r->c->info.flags & SIFT_TERM_POS2)
	{
		const uint8 *rec = pos_next_block(r, len);

		r->given++;
		return rec;
	}
	for (;;)
	{
		const uint8 *rec = r->buf + r->pos;
		uint32		n,
					last;
		const uint8 *end = pos_next(rec, r->buf + r->len, &n, &last);
		Size		keep = r->len - r->pos,
					have;

		if (end)
		{
			*len = end - rec;
			r->pos += *len;
			return rec;
		}
		if (r->next >= r->recs)
			corrupt_pos(r->c);
		/* the record is cut off by the window: its start moves to the front and the window is refilled (a record longer than the window grows it) */
		if (keep == r->cap)
			r->buf = repalloc(r->buf, r->cap *= 2);
		memmove(r->buf, r->buf + r->pos, keep);
		have = Min(r->recs - r->next, r->cap - keep);
		pos_read(r->c, (uint64) r->c->nck * sizeof(uint32) + r->next, have, r->buf + keep);
		r->next += have;
		r->pos = 0;
		r->len = keep + have;
	}
}
