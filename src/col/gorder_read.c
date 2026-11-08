/*
 * col/gorder_read.c - the global order of a leaf as a segment stores it: the header, the directory, the groups, the strings and the chunks, decoded (col/gorder.h, built by col/gorder.c).
 * Portable (core/host.h only).
 */

#include "col/codec.h"
#include "col/gquant.h"
#include "col/gorder.h"

#define GORD_ALIGN8(x)	(((uint64) (x) + 7) & ~(uint64) 7)

bool
sift_cgord_str_head(const uint8 *sec, const SiftCGOrdHdr *h, uint32 *nstr, uint32 *nblk)
{
	uint32		w[4];

	if (h->strbytes < sizeof(w))
		return false;
	memcpy(w, sec, sizeof(w));
	if (w[1] != SIFT_CGORD_STR_BLK || w[2] != (w[0] + w[1] - 1) / w[1] || w[3] != 0 || (uint64) sizeof(w) + ((uint64) w[2] + 1) * sizeof(uint32) > h->strbytes)
		return false;
	*nstr = w[0];
	*nblk = w[2];
	return true;
}

bool
sift_cgord_perm_head(const uint8 *sec, const SiftCGOrdHdr *h, uint32 *nstr, uint32 *bits)
{
	uint32		w[4];

	if (h->strbytes < sizeof(w))
		return false;
	memcpy(w, sec, sizeof(w));
	if (w[1] != sift_cdict_perm_bits(w[0]) || w[2] != 0 || w[3] != 0 || (uint64) h->strbytes != (((uint64) sizeof(w) + ((uint64) w[0] * w[1] + 7) / 8) + 7) / 8 * 8)
		return false;
	*nstr = w[0];
	*bits = w[1];
	return true;
}

bool
sift_cgord_str_get(const uint8 *b, uint32 len, SiftCStrPos *st, uint32 idx, char *out, uint32 *slen)
{
	uint32		pos,
				olen;

	/* the string before idx is out's: a walk in rank order decodes each string once (a block of 16 would otherwise be decoded from its start for every one) */
	if (idx + 1 < st->next)
		*st = (SiftCStrPos) {0, 0, 0};
	pos = st->pos;
	olen = st->olen;
	for (uint32 i = st->next; i <= idx; i++)
	{
		uint32		shared = 0,
					own = 0;
		int			sh = 0;

		for (;;)
		{
			if (pos >= len)
				return false;
			shared |= (uint32) (b[pos] & 0x7F) << sh;
			sh += 7;
			if (!(b[pos++] & 0x80))
				break;
			if (sh > 28)
				return false;
		}
		sh = 0;
		for (;;)
		{
			if (pos >= len)
				return false;
			own |= (uint32) (b[pos] & 0x7F) << sh;
			sh += 7;
			if (!(b[pos++] & 0x80))
				break;
			if (sh > 28)
				return false;
		}
		if (shared > olen || (uint64) shared + own >= SIFT_CGORD_STR_MAX || (uint64) pos + own > len)
			return false;
		memcpy(out + shared, b + pos, own);
		pos += own;
		olen = shared + own;
		*st = (SiftCStrPos) {i + 1, pos, olen};
	}
	*slen = olen;
	return true;
}

bool
sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h)
{
	uint32		w[8];
	bool		grouped,
				text;

	if (len < SIFT_CGORD_HDR)
		return false;
	memcpy(w, blk, sizeof(w));
	grouped = w[1] == SIFT_CGORD_VERSION_GROUP || w[1] == SIFT_CGORD_VERSION_GROUPQ;
	text = w[1] == SIFT_CGORD_VERSION_TEXT || w[1] == SIFT_CGORD_VERSION_TEXTPERM;
	if (w[1] == SIFT_CGORD_VERSION_COMPACT)
	{
		if (w[0] != SIFT_CGORD_MAGIC || w[3] > w[2] || w[5] != SIFT_CGORD_ROWS_FINE || w[6] < 1 || w[6] > 16 || w[7] != 0 ||
			w[4] != (uint32) (((uint64) w[2] + w[5] - 1) / w[5]) || SIFT_CGORD_HDR + (uint64) w[4] * SIFT_CGORD_DIRENT > len)
			return false;
		*h = (SiftCGOrdHdr) {w[2], w[3], w[4], w[5], 0, -1, 0, 0, 0, 0, w[6]};
		return true;
	}
	if (w[0] != SIFT_CGORD_MAGIC || (w[1] != SIFT_CGORD_VERSION && !grouped && !text) || w[3] > w[2] || (w[5] != SIFT_CGORD_ROWS && w[5] != SIFT_CGORD_ROWS_FINE) ||
		(grouped ? (w[6] == 0 || w[7] == 0 || w[7] > SIFT_COL_MAX_COLS) : text ? (w[7] == 0 || w[7] % 8 != 0) : (w[7] == 0 ? w[6] != 0 : (w[6] > w[3] || w[7] <= w[3] || w[3] - w[6] > SIFT_CGORD_ENDS || w[6] > SIFT_CGORD_ENDS || w[2] - w[3] > SIFT_CGORD_ENDS))) ||
		w[4] != (uint32) (((uint64) w[2] + w[5] - 1) / w[5]) ||
		SIFT_CGORD_HDR + (uint64) w[4] * SIFT_CGORD_DIRENT + (uint64) (text ? w[7] : grouped ? (uint64) w[6] * SIFT_CGORD_GRPENT : 0) > len)
		return false;
	*h = (SiftCGOrdHdr) {w[2], w[3], w[4], w[5], grouped ? w[6] : 0, grouped ? (int) w[7] - 1 : -1, text ? w[6] : 0, text ? w[7] : 0, !grouped && !text ? w[6] : 0, !grouped && !text ? w[7] : 0, w[1] == SIFT_CGORD_VERSION_GROUPQ ? SIFT_GQ_Q : 0, w[1] == SIFT_CGORD_VERSION_TEXTPERM};
	return true;
}

bool
sift_cgord_groups_check(const SiftCGOrdHdr *h, SiftCGOrdGrpRun *st, const SiftCGOrdGrp *e, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		const SiftCGOrdGrp *g = &e[i];

		if ((st->started && g->group <= st->prev) || g->vstart != st->vnext || g->nstart != st->nnext || (g->vcount == 0 && g->ncount == 0) ||
			(uint64) g->vstart + g->vcount > h->values || (uint64) g->nstart + g->ncount > h->entries)
			return false;
		st->prev = g->group;
		st->started = true;
		st->vnext += g->vcount;
		st->nnext += g->ncount;
	}
	return true;
}

bool
sift_cgord_groups_done(const SiftCGOrdHdr *h, const SiftCGOrdGrpRun *st)
{
	return st->vnext == h->values && st->nnext == h->entries;
}

bool
sift_cgord_dirent(const uint8 *ent, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d)
{
	if (c >= h->chunks)
		return false;
	memcpy(d, ent, sizeof(*d));
	return (d->pad == 0 || ((d->pad & ~(SIFT_CGORD_GAPS | SIFT_CGORD_COMPACT | SIFT_CGORD_TIES | 0xF00u | (h->gcol >= 0 ? 0xFF0000u : 0))) == 0 && (!SIFT_CGORD_ANCHORS(d->pad) || (d->pad & SIFT_CGORD_COMPACT)) && !((d->pad & SIFT_CGORD_COMPACT) && ((d->pad & SIFT_CGORD_GAPS) || h->quant == 0)) && (!(d->pad & SIFT_CGORD_TIES) || (d->pad & SIFT_CGORD_COMPACT)) && (d->pad >> 8 & 0xF) >= 1 && (d->pad >> 8 & 0xF) <= 9)) && d->off % 8 == 0 && (uint64) d->off + (((uint64) d->klen + 7) & ~(uint64) 7) + (((uint64) d->alen + 7) & ~(uint64) 7) + SIFT_CGORD_ANCHOR_BYTES(SIFT_CGORD_ANCHORS(d->pad)) <= len &&
		d->off >= SIFT_CGORD_HDR + (uint64) h->chunks * SIFT_CGORD_DIRENT;
}

bool
sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, uint32 w, int64 *key, uint64 *addr)
{
	SiftCc	   *k = sift_cc_open_rows(mcxt, kb, klen, false, m);
	uint32		ob = pad ? pad >> 8 & 0xF : 9;

	w = Min(w, m);
	sift_cc_decode_head(k, w, key);
	sift_cc_close(k);
	if (pad & SIFT_CGORD_GAPS)
	{
		uint32		hdr[2],
					flen,
					nf,
					f = 0,
					fi = 0,
					gi = 0;
		int64	   *fv,
				   *gv;
		SiftCc	   *fc,
				   *gc;
		uint64		prev = 0;

		if (alen < sizeof(hdr))
			return false;
		memcpy(hdr, ab, sizeof(hdr));
		flen = hdr[0];
		nf = hdr[1];
		if (nf == 0 || nf >= m || (uint64) sizeof(hdr) + GORD_ALIGN8(flen) > alen)
			return false;
		fc = sift_cc_open_rows(mcxt, ab + sizeof(hdr), flen, false, nf);
		gc = sift_cc_open_rows(mcxt, ab + sizeof(hdr) + GORD_ALIGN8(flen), alen - sizeof(hdr) - GORD_ALIGN8(flen), false, m - nf);
		for (uint32 i = 0; i < w; i++)
			f += i == 0 || key[i] != key[i - 1];
		fv = palloc((Size) (f + 1) * sizeof(int64));
		gv = palloc((Size) (w - f + 1) * sizeof(int64));
		sift_cc_decode_head(fc, f, fv);
		sift_cc_decode_head(gc, w - f, gv);
		for (uint32 i = 0; i < w; i++)
		{
			prev = i == 0 || key[i] != key[i - 1] ? (uint64) fv[fi++] : prev + (uint64) gv[gi++] + 1;
			addr[i] = (prev >> ob) << 16 | (prev & (((uint64) 1 << ob) - 1));
		}
		pfree(fv);
		pfree(gv);
		sift_cc_close(fc);
		sift_cc_close(gc);
		return true;
	}
	{
		SiftCc	   *a = sift_cc_open_rows(mcxt, ab, alen, false, m);

		sift_cc_decode_head(a, w, (int64 *) addr);
		for (uint32 i = 0; i < w; i++)
			addr[i] = (addr[i] >> ob) << 16 | (addr[i] & (((uint64) 1 << ob) - 1));
		sift_cc_close(a);
	}
	return true;
}

bool
sift_cgord_decode_compact(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first, uint32 w, int64 *kmin, uint32 *sh, uint64 *addr)
{
	uint32		ob = pad >> 8 & 0xF;
	SiftCc	   *a;

	if (kmin != NULL && !sift_gq_decode(kb, klen, m, first, kmin, sh))
		return false;
	w = Min(w, m);
	a = sift_cc_open_rows(mcxt, ab, alen, false, m);
	sift_cc_decode_head(a, w, (int64 *) addr);
	for (uint32 i = 0; i < w; i++)
		addr[i] = (addr[i] >> ob) << 16 | (addr[i] & (((uint64) 1 << ob) - 1));
	sift_cc_close(a);
	return true;
}

/*
 * The buckets of a compact chunk of a grouped block count each group from the bucket after the one before it (sift_cgord_build_grouped): the first key of a group, kept exact after the
 * addresses (n of them, with their positions), puts it back where it was, and is the exact key of that entry (key, its bit in exact). kmin has the lower bound of every entry as if the
 * groups followed one another; false when the positions are not ascending inside the chunk.
 */
bool
sift_cgord_group_bounds(int64 *kmin, int64 *key, uint64 *exact, uint32 m, int64 first, const uint8 *anch, uint32 n)
{
	const uint8 *pk = anch + GORD_ALIGN8((uint64) n * sizeof(uint16));
	uint32		next = 0;
	int64		base = first,
				qa = kmin[0];

	for (uint32 i = 1; i < m; i++)
	{
		uint16		p = m;

		if (next < n)
			memcpy(&p, anch + (Size) next * sizeof(uint16), sizeof(uint16));
		if (p < i)
			return false;
		if (p == i)
		{
			memcpy(&base, pk + (Size) next * sizeof(int64), sizeof(int64));
			qa = kmin[i];
			key[i] = base;
			exact[i / 64] |= UINT64CONST(1) << (i % 64);
			next++;
		}
		kmin[i] = (int64) ((uint64) base + ((uint64) kmin[i] - (uint64) qa));
	}
	return next == n;
}
