/*
 * bm25/pos.c - position records of a term blob: the checkpoints, the windowed reads and the record decoder
 * (layout in bm25/posting.h).
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "bm25/posting.h"
#include "bm25/posting_int.h"

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

struct SiftPosReader
{
	SiftPostingCursor *c;
	uint8	   *buf;			/* the window; the records of an inline term themselves */
	Size		cap;
	Size		pos;			/* of the next record in the window */
	Size		len;			/* bytes in the window */
	uint64		next;			/* offset in the records of the byte after the window */
	uint64		recs;			/* bytes of records */
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
	r->cap = POS_READER_WINDOW;
	r->buf = MemoryContextAlloc(mcxt, r->cap);
	return r;
}

const uint8 *
sift_post_pos_next(SiftPosReader *r, uint32 *len)
{
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
