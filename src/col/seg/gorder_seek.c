/*
 * col/seg/gorder_seek.c - the reads of a seekable chunk of a global order (col/gorder.h, SIFT_CGORD_SEEK): the sub-blocks a scan or a seek needs, decoded into the buffer's window.
 */

#include "core/cmp.h"
#include "col/seg_int.h"

/* The chunk c of a seekable or positional block, read and parsed into s, which points into the bytes returned (pfree them); point: it is read to find a place in it, not to go on from it, so the pages after it are not read ahead */
static uint8 *
seek_load(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, SiftCGOrdSeek *s, bool point)
{
	Size		kl = ALIGN8((Size) d->klen),
				al = ALIGN8((Size) d->alen);
	uint8	   *raw = palloc(kl + al + SIFT_COL_PAD);
	uint32		m = sift_cgord_chunk_rows(&g->hdr, c);

	if (point)
		sift_sr_point(&seg->sr);
	seg_read(seg, g->at + d->off, kl + al, raw);
	if (!((d->pad & SIFT_CGORD_POS) ? sift_cgord_pos_open(s, raw, d->klen, raw + kl, d->alen, m) : sift_cgord_seek_open(s, raw, d->klen, raw + kl, d->alen, d->pad, m, d->first)))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	return raw;
}

/* The plain block the positions of the grouped block of slot col are in: the slot of the same leaf ncols less */
static GOrdLeaf *
plain_leaf(SiftColSegment *seg, int col)
{
	GOrdLeaf   *pg = col >= seg->schema->ncols ? seg_gord_leaf(seg, col - seg->schema->ncols) : NULL;

	if (pg == NULL || pg->hdr.gcol >= 0 || pg->hdr.vtotal != 0 || pg->hdr.strbytes != 0)
		corrupt(seg, "The global order of leaf %d has positions in a plain order that is not there.", col);
	return pg;
}

/* The chunk c of the plain block pg into its cache: a seekable one parsed, any other decoded whole */
static void
plain_load(SiftColSegment *seg, GOrdLeaf *pg, int pcol, uint32 c, GOrdPlainCache *pc)
{
	SiftCGOrdDir d = seg_gord_dirent(seg, pg, pcol, c);
	uint32		m = sift_cgord_chunk_rows(&pg->hdr, c);
	Size		kl = ALIGN8((Size) d.klen),
				al = ALIGN8((Size) d.alen);
	uint8	   *raw;

	if (d.pad & SIFT_CGORD_COMPACT)
		corrupt(seg, "The global order of leaf %d has a compact chunk %u under positions.", pcol, c);
	raw = MemoryContextAlloc(seg->mcxt, kl + al + SIFT_COL_PAD);
	seg_read(seg, pg->at + d.off, kl + al, raw);
	if (pc->raw != NULL)
		pfree(pc->raw);
	pc->raw = raw;
	pc->c = c;
	pc->from = pc->n = 0;
	pc->seek = (d.pad & SIFT_CGORD_SEEK) != 0;
	if (pc->seek ? !sift_cgord_seek_open(&pc->s, raw, d.klen, raw + kl, d.alen, d.pad, m, d.first) :
		!sift_cgord_decode(seg->mcxt, raw, d.klen, raw + kl, d.alen, d.pad, m, d.first, m, pc->key, pc->addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", pcol, c);
	if (!pc->seek)
	{
		pc->n = m;
		GORD_DECODED(m);
	}
}

/* The key and the heap address of the entry at position p of the plain block pg (leaf pcol); the chunk, and of a seekable one the sub-block, read last are kept */
static void
plain_get(SiftColSegment *seg, GOrdLeaf *pg, int pcol, uint64 p, int64 *key, uint64 *addr)
{
	GOrdPlainCache *pc = pg->pcache;
	uint32		c,
				pe;

	if (p >= pg->hdr.entries)
		corrupt(seg, "The global order of leaf %d has the position %llu of %u entries.", pcol, (unsigned long long) p, pg->hdr.entries);
	if (pc == NULL)
	{
		pc = pg->pcache = MemoryContextAlloc(seg->mcxt, sizeof(GOrdPlainCache));
		pc->c = UINT32_MAX;
		pc->raw = NULL;
	}
	c = (uint32) (p / pg->hdr.crows);
	pe = (uint32) (p - (uint64) c * pg->hdr.crows);
	if (pc->c != c)
		plain_load(seg, pg, pcol, c, pc);
	if (pc->seek)
	{
		if (!sift_cgord_seek_get(&pc->s, pe, key, addr))
			corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", pcol, c);
		GORD_DECODED(1);
		return;
	}
	*key = pc->key[pe];
	*addr = pc->addr[pe];
}

/* The entries [e0, e1) of a positional chunk as their keys and addresses into key[e0 ..] and addr[e0 ..]: each is read from the plain block alone, the sub-blocks of positions decoded once */
static void
pos_resolve(SiftColSegment *seg, int col, const SiftCGOrdSeek *s, uint32 e0, uint32 e1, int64 *key, uint64 *addr)
{
	GOrdLeaf   *pg = plain_leaf(seg, col);
	int64		pos[SIFT_CGORD_CAP];

	for (uint32 b = e0 / SIFT_CGORD_SUB; b * SIFT_CGORD_SUB < e1; b++)
		sift_cgord_pos_decode(s, b, pos);
	for (uint32 i = e0; i < e1; i++)
		plain_get(seg, pg, col - seg->schema->ncols, (uint64) pos[i], &key[i], &addr[i]);
}

/* The first sub-block in [lo, hi) of the chunk s whose fence is not before key (after: is after it), hi when none; the key of a fence of a positional chunk is its position's in the plain block */
static uint32
seek_fence(SiftColSegment *seg, int col, const SiftCGOrdSeek *s, uint32 lo, uint32 hi, int64 key, bool after)
{
	GOrdLeaf   *pg;
	int64		k;
	uint64		a;

	if (s->ab != NULL)
		return sift_cgord_seek_fence(s, lo, hi, key, after);
	pg = plain_leaf(seg, col);
	SIFT_BISECT(uint32, mid, lo, hi, (plain_get(seg, pg, col - seg->schema->ncols, (uint64) s->fence[mid], &k, &a), after ? k <= key : k < key));
	return lo;
}

/* buf holds the entries [e0, e1) of the chunk c, keys and addresses, as its window */
static void
window_set(const GOrdLeaf *g, uint32 c, uint32 m, uint32 e0, uint32 e1, SiftCGOrdBuf *buf)
{
	buf->at = c * g->hdr.crows;
	buf->rows = m;
	buf->compact = false;
	buf->kmin_ok = false;
	buf->afrom = buf->kfrom = e0;
	buf->cm = e1;
	buf->kn = buf->an = e1 - e0;
}

/* The sub-blocks [b0, b1) of the chunk c, whose keys and addresses are in buf from their place in the chunk on, as the window of the chunk buf holds */
static void
seek_window(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdSeek *s, uint32 b0, uint32 b1, SiftCGOrdBuf *buf)
{
	uint32		e1 = Min(b1 * SIFT_CGORD_SUB, s->m);

	for (uint32 b = b0; b < b1; b++)
		if (!sift_cgord_seek_decode(s, b, buf->key, buf->addr))
			corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	GORD_DECODED(e1 - b0 * SIFT_CGORD_SUB);
	window_set(g, c, s->m, b0 * SIFT_CGORD_SUB, e1, buf);
}

/*
 * The entries of a positional chunk a scan wants: each costs a read of the plain order, in a chunk of its own where the positions of a group are far apart, so the first window of a chunk is
 * SIFT_CGORD_POS_FIRST entries and the next ones double, up to a sub-block, as the scan shows it goes on.
 */
static void
pos_fill(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdSeek *s, uint32 pe, uint32 w, SiftCGOrdBuf *buf)
{
	bool		more = buf->at == c * g->hdr.crows && !buf->compact;
	uint32		n = Min(w, more ? Min(2 * buf->an, (uint32) SIFT_CGORD_SUB) : SIFT_CGORD_POS_FIRST),
				e0 = pe,
				e1 = Min(s->m, pe + Max(n, 1));

	if (w == UINT32_MAX)
	{
		e0 = 0;
		e1 = s->m;
	}
	else if (buf->desc)
	{
		e0 = pe + 1 > n ? pe + 1 - n : 0;
		e1 = pe + 1;
	}
	pos_resolve(seg, col, s, e0, e1, buf->key, buf->addr);
	window_set(g, c, s->m, e0, e1, buf);
}

/*
 * The window of the seekable chunk c a scan that reads from the entry p on (buf->desc: down to it) with w entries to go wants: whole sub-blocks, the chunk whole for w = UINT32_MAX; kept
 * if buf already has it.
 */
void
seg_gord_seek_fill(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, uint32 p, uint32 w, SiftCGOrdBuf *buf)
{
	SiftCGOrdSeek s;
	uint8	   *raw;
	uint32		pe = p - c * g->hdr.crows,
				b0,
				b1;

	if (buf->at == c * g->hdr.crows && !buf->compact && pe - buf->afrom < buf->an && (w != UINT32_MAX || buf->an == buf->rows))
		return;
	raw = seek_load(seg, g, col, c, d, &s, false);
	if (s.ab == NULL)
	{
		pos_fill(seg, g, col, c, &s, pe, w, buf);
		pfree(raw);
		return;
	}
	if (w == UINT32_MAX)
	{
		b0 = 0;
		b1 = s.nb;
	}
	else if (buf->desc)
	{
		b0 = (pe + 1 > w ? pe + 1 - w : 0) / SIFT_CGORD_SUB;
		b1 = pe / SIFT_CGORD_SUB + 1;
	}
	else
	{
		b0 = pe / SIFT_CGORD_SUB;
		b1 = Min(s.nb, (pe + Max(w, 1) - 1) / SIFT_CGORD_SUB + 1);
	}
	seek_window(seg, g, col, c, &s, b0, b1, buf);
	pfree(raw);
}


/*
 * The first entry of [from, to), all in the seekable chunk c, whose key is not before key (after: is after it); to when there is none. The last sub-block that starts inside the range
 * with a fence before key holds it or ends before it: that one is decoded, and the next fence, which is not before key, answers where it has none.
 */
uint32
seg_gord_seek_seek(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, uint32 from, uint32 to, int64 key, bool after, SiftCGOrdBuf *memo)
{
	SiftCGOrdSeek s;
	uint8	   *raw;
	uint32		base = c * g->hdr.crows,
				b,
				end;

	raw = seek_load(seg, g, col, c, d, &s, true);
	b = seek_fence(seg, col, &s, (from - base) / SIFT_CGORD_SUB + 1, (to - 1 - base) / SIFT_CGORD_SUB + 1, key, after) - 1;
	end = Min(to, base + (b + 1) * SIFT_CGORD_SUB);
	if (s.ab == NULL)
	{
		uint32		lo = Max(from, base + b * SIFT_CGORD_SUB) - base,
					hi = end - base;
		int64		pos[SIFT_CGORD_CAP],
					k;
		uint64		a;
		GOrdLeaf   *pg = plain_leaf(seg, col);

		sift_cgord_pos_decode(&s, b, pos);
		SIFT_BISECT(uint32, mid, lo, hi, (plain_get(seg, pg, col - seg->schema->ncols, (uint64) pos[mid], &k, &a), after ? k <= key : k < key));
		pfree(raw);
		return base + lo;
	}
	seek_window(seg, g, col, c, &s, b, b + 1, memo);
	pfree(raw);
	for (uint32 i = Max(from, base + b * SIFT_CGORD_SUB); i < end; i++)
		if (after ? memo->key[i - base] > key : memo->key[i - base] >= key)
			return i;
	return end;
}

/*
 * Of the first mv entries (the values) of the seekable chunk c, how many have a key before v (desc: not after it): the sub-block whose fence is the last before it is decoded into memo and
 * searched.
 */
uint32
seg_gord_seek_before(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, uint32 mv, int64 v, bool desc, SiftCGOrdBuf *memo)
{
	SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);
	SiftCGOrdSeek s;
	uint8	   *raw = seek_load(seg, g, col, c, &d, &s, true);
	uint32		b = sift_cgord_seek_fence(&s, 1, (mv + SIFT_CGORD_SUB - 1) / SIFT_CGORD_SUB, v, desc) - 1,
				end = Min(mv, (b + 1) * SIFT_CGORD_SUB),
				i;

	seek_window(seg, g, col, c, &s, b, b + 1, memo);
	pfree(raw);
	for (i = b * SIFT_CGORD_SUB; i < end; i++)
		if (desc ? memo->key[i] > v : memo->key[i] >= v)
			break;
	return i;
}

/* The first w entries (the m of the chunk when fewer) of the positional chunk c, as keys and addresses: key and addr have SIFT_CGORD_CAP slots; the count */
uint32
seg_gord_pos_head(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, uint32 w, int64 *key, uint64 *addr)
{
	SiftCGOrdSeek s;
	uint8	   *raw = seek_load(seg, g, col, c, d, &s, false);

	w = Min(w, s.m);
	pos_resolve(seg, col, &s, 0, w, key, addr);
	pfree(raw);
	return w;
}

/* The addresses of the entries [from, to) of the plain chunk c of a block into buf, which holds its keys already (a seek read them): the keys are not decoded again. False when it does not */
bool
seg_gord_plain_addrs(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, SiftCGOrdBuf *buf, uint32 from, uint32 to)
{
	uint32		m = sift_cgord_chunk_rows(&g->hdr, c),
				a0 = from;
	Size		klen8 = ALIGN8((Size) d->klen);
	uint8	   *raw;

	if ((d->pad & (SIFT_CGORD_SEEK | SIFT_CGORD_POS)) || buf->at != c * g->hdr.crows || buf->compact || buf->rows != m || buf->kfrom != 0 || buf->kn < m)
		return false;
	raw = palloc(ALIGN8((Size) d->alen) + SIFT_COL_PAD);
	seg_read(seg, g->at + d->off + klen8, ALIGN8((Size) d->alen), raw);
	if (!sift_cgord_decode_addrs(seg->mcxt, raw, d->alen, d->pad, m, buf->key, &a0, to, buf->addr))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	pfree(raw);
	buf->afrom = from;
	buf->an = to - from;
	return true;
}
