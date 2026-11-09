/*
 * col/gorder_read.c - the global order of a leaf as a segment stores it: the header, the directory, the groups, the strings and the chunks, decoded (col/gorder.h, built by col/gorder.c).
 * Portable (core/host.h only).
 */

#include "col/codec.h"
#include "col/gquant.h"
#include "col/gorder.h"
#include "core/cmp.h"

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
	return (d->pad == 0 || ((d->pad & ~(SIFT_CGORD_GAPS | SIFT_CGORD_COMPACT | SIFT_CGORD_TIES | SIFT_CGORD_SEEK | 0xF00u | (h->gcol >= 0 ? 0xFF0000u : 0))) == 0 && (!SIFT_CGORD_ANCHORS(d->pad) || (d->pad & SIFT_CGORD_COMPACT)) && !((d->pad & SIFT_CGORD_COMPACT) && ((d->pad & SIFT_CGORD_GAPS) || h->quant == 0)) && (!(d->pad & SIFT_CGORD_TIES) || (d->pad & SIFT_CGORD_COMPACT)) && (!(d->pad & SIFT_CGORD_SEEK) || (!(d->pad & (SIFT_CGORD_GAPS | SIFT_CGORD_COMPACT | SIFT_CGORD_TIES)) && h->crows == SIFT_CGORD_ROWS_FINE)) && (d->pad >> 8 & 0xF) >= 1 && (d->pad >> 8 & 0xF) <= 9)) && d->off % 8 == 0 && (uint64) d->off + (((uint64) d->klen + 7) & ~(uint64) 7) + (((uint64) d->alen + 7) & ~(uint64) 7) + SIFT_CGORD_ANCHOR_BYTES(SIFT_CGORD_ANCHORS(d->pad)) <= len &&
		d->off >= SIFT_CGORD_HDR + (uint64) h->chunks * SIFT_CGORD_DIRENT;
}

bool
sift_cgord_has_seek(const uint8 *blk, const SiftCGOrdHdr *h)
{
	for (uint32 c = 0; c < h->chunks; c++)
	{
		uint32		pad;

		memcpy(&pad, blk + SIFT_CGORD_HDR + (uint64) c * SIFT_CGORD_DIRENT + offsetof(SiftCGOrdDir, pad), sizeof(pad));
		if (pad & SIFT_CGORD_SEEK)
			return true;
	}
	return false;
}

/* bits [bit, bit + w) of the little endian bit string at p (w <= 64; 8 readable bytes after the string) */
static inline uint64
bits_get(const uint8 *p, uint64 bit, uint32 w)
{
	uint64		v;

	if (w == 0)
		return 0;
	if (w > 56)
		return bits_get(p, bit, 32) | bits_get(p, bit + 32, w - 32) << 32;
	memcpy(&v, p + (bit >> 3), sizeof(v));
	return (v >> (bit & 7)) & ((UINT64CONST(1) << w) - 1);
}

static inline int64
unzigzag(uint64 v)
{
	return (int64) (v >> 1) ^ -(int64) (v & 1);
}

bool
sift_cgord_seek_open(SiftCGOrdSeek *s, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first)
{
	uint32		nb = (m + SIFT_CGORD_SUB - 1) / SIFT_CGORD_SUB,
				fw,
				p = 2,
				at;
	bool		fzz;

	if (m == 0 || nb > SIFT_CGORD_SUBS || klen < 2 || alen < 9 + 2 * nb)
		return false;
	fw = kb[0];
	fzz = kb[1] >> 1 & 1;
	s->scale = 1;
	if (kb[1] & 1)
	{
		if (klen < p + sizeof(uint64))
			return false;
		memcpy(&s->scale, kb + p, sizeof(uint64));
		p += sizeof(uint64);
		if (s->scale < 2)
			return false;
	}
	s->kb = kb;
	s->ab = ab;
	s->m = m;
	s->nb = nb;
	s->ob = pad >> 8 & 0xF;
	s->kw = kb + p;
	p += nb;
	if (fw > 64 || (uint64) p + ((uint64) (nb - 1) * fw + 7) / 8 > klen)
		return false;
	s->fence[0] = first;
	for (uint32 b = 1; b < nb; b++)
	{
		uint64		v = bits_get(kb + p, (uint64) (b - 1) * fw, fw);

		s->fence[b] = (int64) ((uint64) s->fence[b - 1] + (fzz ? (uint64) unzigzag(v) : v * s->scale));
	}
	at = p + (uint32) (((uint64) (nb - 1) * fw + 7) / 8);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		n = Min(SIFT_CGORD_SUB, m - b * SIFT_CGORD_SUB),
					w = s->kw[b] & 0x7F;

		if (w > 64)
			return false;
		s->koff[b] = at;
		at += (uint32) (((uint64) (n - 1) * w + 7) / 8);
	}
	s->koff[nb] = at;
	if (at > klen)
		return false;
	s->wa = ab[0];
	memcpy(&s->amin, ab + 1, sizeof(uint64));
	s->am = ab + 9;
	if (s->wa > 56)
		return false;
	at = 9 + 2 * nb;
	for (uint32 b = 0; b < nb; b++)
	{
		if (s->am[2 * b] != 0xFF && s->am[2 * b] > 56)
			return false;
		s->aoff[b] = at;
		at += s->am[2 * b + 1];
	}
	s->aoff[nb] = at;
	return at <= alen;
}

uint32
sift_cgord_seek_fence(const SiftCGOrdSeek *s, uint32 lo, uint32 hi, int64 key, bool after)
{
	SIFT_BISECT(uint32, mid, lo, hi, after ? s->fence[mid] <= key : s->fence[mid] < key);
	return lo;
}

bool
sift_cgord_seek_decode(const SiftCGOrdSeek *s, uint32 b, int64 *key, uint64 *addr)
{
	uint32		base = b * SIFT_CGORD_SUB,
				n = Min(SIFT_CGORD_SUB, s->m - base),
				w = s->kw[b] & 0x7F,
				gw = s->am[2 * b];
	uint64		k = (uint64) s->fence[b],
				bit = 0,
				prev = 0,
				mask = ((uint64) 1 << s->ob) - 1;
	bool		zz = s->kw[b] >> 7;
	const uint8 *p = s->kb + s->koff[b];

	key[base] = s->fence[b];
	for (uint32 i = 1; i < n; i++, bit += w)
	{
		uint64		v = bits_get(p, bit, w);

		k += zz ? (uint64) unzigzag(v) : v * s->scale;
		key[base + i] = (int64) k;
	}
	p = s->ab + s->aoff[b];
	bit = 0;
	for (uint32 i = 0; i < n; i++)
	{
		if (gw != 0xFF && i > 0 && key[base + i] == key[base + i - 1])
		{
			prev += bits_get(p, bit, gw) + 1;
			bit += gw;
		}
		else
		{
			prev = s->amin + bits_get(p, bit, s->wa);
			bit += s->wa;
		}
		addr[base + i] = (prev >> s->ob) << 16 | (prev & mask);
	}
	return bit <= (uint64) s->am[2 * b + 1] * 8;
}

void
sift_cgord_decode_keys(MemoryContext mcxt, const uint8 *kb, uint32 klen, uint32 m, uint32 w, int64 *key)
{
	SiftCc	   *k = sift_cc_open_rows(mcxt, kb, klen, false, m);

	sift_cc_decode_head(k, Min(w, m), key);
	sift_cc_close(k);
}

/* the values idx[0 .. n) = s .. s + n - 1 of a blob, into dst */
static void
decode_run(const SiftCc *c, uint32 s, uint32 n, int64 *dst)
{
	uint32		idx[SIFT_CGORD_CAP];

	for (uint32 i = 0; i < n; i++)
		idx[i] = s + i;
	sift_cc_decode_sel(c, idx, n, dst);
}

bool
sift_cgord_decode_addrs(MemoryContext mcxt, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, const int64 *key, uint32 *a0, uint32 a1, uint64 *addr)
{
	uint32		ob = pad ? pad >> 8 & 0xF : 9;
	uint32		s = *a0;
	uint64		mask = ((uint64) 1 << ob) - 1;

	if (a1 > m || s >= a1)
		return s >= a1;
	if (pad & SIFT_CGORD_GAPS)
	{
		uint32		hdr[2],
					flen,
					nf,
					before = 0,
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
		/* the blob codes the first address of each run of equal keys and the gaps after it: the window starts at a run's first entry, and the runs before it are counted */
		while (s > 0 && key[s] == key[s - 1])
			s--;
		for (uint32 i = 1; i < s; i++)
			before += key[i] != key[i - 1];
		before += s > 0;
		for (uint32 i = s; i < a1; i++)
			f += i == 0 || key[i] != key[i - 1];
		if (before + f > nf || (s - before) + (a1 - s - f) > m - nf)
			return false;
		fc = sift_cc_open_rows(mcxt, ab + sizeof(hdr), flen, false, nf);
		gc = sift_cc_open_rows(mcxt, ab + sizeof(hdr) + GORD_ALIGN8(flen), alen - sizeof(hdr) - GORD_ALIGN8(flen), false, m - nf);
		fv = palloc((Size) (f + 1 + SIFT_COL_PAD / 8) * sizeof(int64));
		gv = palloc((Size) (a1 - s - f + 1 + SIFT_COL_PAD / 8) * sizeof(int64));
		decode_run(fc, before, f, fv);
		if (a1 - s > f)
			decode_run(gc, s - before, a1 - s - f, gv);
		for (uint32 i = s; i < a1; i++)
		{
			prev = i == 0 || key[i] != key[i - 1] ? (uint64) fv[fi++] : prev + (uint64) gv[gi++] + 1;
			addr[i] = (prev >> ob) << 16 | (prev & mask);
		}
		pfree(fv);
		pfree(gv);
		sift_cc_close(fc);
		sift_cc_close(gc);
		*a0 = s;
		return true;
	}
	{
		SiftCc	   *a = sift_cc_open_rows(mcxt, ab, alen, false, m);

		decode_run(a, s, a1 - s, (int64 *) addr + s);
		for (uint32 i = s; i < a1; i++)
			addr[i] = (addr[i] >> ob) << 16 | (addr[i] & mask);
		sift_cc_close(a);
	}
	return true;
}

bool
sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first, uint32 w, int64 *key, uint64 *addr)
{
	uint32		a0 = 0;

	w = Min(w, m);
	if (pad & SIFT_CGORD_SEEK)
	{
		SiftCGOrdSeek s;

		if (!sift_cgord_seek_open(&s, kb, klen, ab, alen, pad, m, first))
			return false;
		for (uint32 b = 0; b * SIFT_CGORD_SUB < w; b++)
			if (!sift_cgord_seek_decode(&s, b, key, addr))
				return false;
		return true;
	}
	sift_cgord_decode_keys(mcxt, kb, klen, m, w, key);
	return sift_cgord_decode_addrs(mcxt, ab, alen, pad, m, key, &a0, w, addr);
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
	uint32		i = 1;
	int64		base = first,
				qa = kmin[0];

	/* the entries between two anchors move by one offset: a plain loop over the run, not a test per entry */
	for (uint32 next = 0; next <= n; next++)
	{
		uint16		p = m;
		uint64		shift = (uint64) base - (uint64) qa;

		if (next < n)
			memcpy(&p, anch + (Size) next * sizeof(uint16), sizeof(uint16));
		if (p < i || p > m || (next < n && p == m))
			return false;
		for (uint32 j = i; j < p; j++)
			kmin[j] = (int64) ((uint64) kmin[j] + shift);
		if (next == n)
			break;
		memcpy(&base, pk + (Size) next * sizeof(int64), sizeof(int64));
		qa = kmin[p];
		key[p] = base;
		exact[p / 64] |= UINT64CONST(1) << (p % 64);
		kmin[p] = base;
		i = p + 1;
	}
	return true;
}
