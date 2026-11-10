/*
 * col/seg/gorder_seek.c - the reads of a seekable chunk of a global order (col/gorder.h, SIFT_CGORD_SEEK): the sub-blocks a scan or a seek needs, decoded into the buffer's window.
 */

#include "core/cmp.h"
#include "col/seg_int.h"

/* The chunk c of a seekable block, read and parsed into s, which points into the bytes returned (pfree them); point: it is read to find a place in it, not to go on from it, so the pages after it are not read ahead */
static uint8 *
seek_load(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, SiftCGOrdSeek *s, bool point)
{
	Size		kl = ALIGN8((Size) d->klen),
				al = ALIGN8((Size) d->alen);
	uint8	   *raw = palloc(kl + al + SIFT_COL_PAD);

	if (point)
		sift_sr_point(&seg->sr);
	seg_read(seg, g->at + d->off, kl + al, raw);
	if (!sift_cgord_seek_open(s, raw, d->klen, raw + kl, d->alen, d->pad, sift_cgord_chunk_rows(&g->hdr, c), d->first))
		corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	return raw;
}

/* The sub-blocks [b0, b1) of the chunk c, whose keys and addresses are in buf from their place in the chunk on, as the window of the chunk buf holds */
static void
seek_window(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdSeek *s, uint32 b0, uint32 b1, SiftCGOrdBuf *buf)
{
	for (uint32 b = b0; b < b1; b++)
		if (!sift_cgord_seek_decode(s, b, buf->key, buf->addr))
			corrupt(seg, "The global order of leaf %d has a malformed chunk %u.", col, c);
	GORD_DECODED(Min(b1 * SIFT_CGORD_SUB, s->m) - b0 * SIFT_CGORD_SUB);
	buf->at = c * g->hdr.crows;
	buf->rows = s->m;
	buf->compact = false;
	buf->kmin_ok = false;
	buf->afrom = buf->kfrom = b0 * SIFT_CGORD_SUB;
	buf->cm = Min(b1 * SIFT_CGORD_SUB, s->m);
	buf->kn = buf->an = buf->cm - buf->kfrom;
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
	b = sift_cgord_seek_fence(&s, (from - base) / SIFT_CGORD_SUB + 1, (to - 1 - base) / SIFT_CGORD_SUB + 1, key, after) - 1;
	seek_window(seg, g, col, c, &s, b, b + 1, memo);
	pfree(raw);
	end = Min(to, base + (b + 1) * SIFT_CGORD_SUB);
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

/* The addresses of the entries [from, to) of the plain chunk c of a block into buf, which holds its keys already (a seek read them): the keys are not decoded again. False when it does not */
bool
seg_gord_plain_addrs(SiftColSegment *seg, GOrdLeaf *g, int col, uint32 c, const SiftCGOrdDir *d, SiftCGOrdBuf *buf, uint32 from, uint32 to)
{
	uint32		m = sift_cgord_chunk_rows(&g->hdr, c),
				a0 = from;
	Size		klen8 = ALIGN8((Size) d->klen);
	uint8	   *raw;

	if ((d->pad & SIFT_CGORD_SEEK) || buf->at != c * g->hdr.crows || buf->compact || buf->rows != m || buf->kfrom != 0 || buf->kn < m)
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
