/*
 * col/gorder_seek.c - the seekable chunk of a global order (col/gorder.h, SIFT_CGORD_SEEK): the writer's keys and addresses of the sub-blocks of 32 entries. Portable (core/host.h only).
 */

#include "core/rowmem.h"
#include "col/gorder.h"

/* bits of v: 0 for 0 */
static inline uint32
bitlen(uint64 v)
{
	return v == 0 ? 0 : 64 - __builtin_clzll(v);
}

/* v (w <= 64 bits) at bit `bit` of the zeroed little endian bit string at buf, which has 8 bytes more */
static void
bits_put(uint8 *buf, uint64 bit, uint64 v, uint32 w)
{
	uint64		cur;

	if (w > 56)
	{
		bits_put(buf, bit, v & 0xFFFFFFFF, 32);
		bits_put(buf, bit + 32, v >> 32, w - 32);
		return;
	}
	memcpy(&cur, buf + (bit >> 3), sizeof(cur));
	cur |= v << (bit & 7);
	memcpy(buf + (bit >> 3), &cur, sizeof(cur));
}

/* n zero bytes appended to out (with 8 more behind them, for bits_put): where they start */
static uint8 *
put_zeros(StringInfo out, Size n)
{
	uint8	   *p;

	enlargeStringInfo(out, n + 8);
	p = (uint8 *) out->data + out->len;
	memset(p, 0, n + 8);
	out->len += n;
	return p;
}

static uint64
gcd64(uint64 a, uint64 b)
{
	while (b != 0)
	{
		uint64		t = a % b;

		a = b;
		b = t;
	}
	return a;
}

/* the delta hi - lo as it is coded: zigzag where the deltas of its place go down, else divided by the scale */
static inline uint64
delta_code(int64 hi, int64 lo, bool zz, uint64 scale)
{
	uint64		d = (uint64) hi - (uint64) lo;

	return zz ? (d << 1) ^ (uint64) ((int64) d >> 63) : d / scale;
}

/* The key blob of a seekable chunk (SIFT_CGORD_SEEK in col/gorder.h) */
static void
seek_keys(StringInfo out, const int64 *kv, uint32 m)
{
	uint32		nb = (m + SIFT_CGORD_SUB - 1) / SIFT_CGORD_SUB,
				fw = 0;
	uint64		scale = 1,
				g = 0;
	bool		neg = false,
				fzz = false;
	uint8		kw[SIFT_CGORD_SUBS];
	uint8	   *p;

	for (uint32 i = 1; i < m; i++)
		neg |= kv[i] < kv[i - 1];
	for (uint32 i = 1; !neg && i < m && g != 1; i++)
		g = gcd64(g, (uint64) kv[i] - (uint64) kv[i - 1]);
	scale = g > 1 ? g : 1;
	for (uint32 b = 1; b < nb; b++)
		fzz |= kv[b * SIFT_CGORD_SUB] < kv[(b - 1) * SIFT_CGORD_SUB];
	for (uint32 b = 1; b < nb; b++)
		fw = Max(fw, bitlen(delta_code(kv[b * SIFT_CGORD_SUB], kv[(b - 1) * SIFT_CGORD_SUB], fzz, scale)));
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		from = b * SIFT_CGORD_SUB,
					to = Min(m, from + SIFT_CGORD_SUB),
					w = 0;
		bool		zz = false;

		for (uint32 i = from + 1; i < to; i++)
			zz |= kv[i] < kv[i - 1];
		for (uint32 i = from + 1; i < to; i++)
			w = Max(w, bitlen(delta_code(kv[i], kv[i - 1], zz, scale)));
		kw[b] = (uint8) (w | (zz ? 0x80 : 0));
	}
	appendStringInfoChar(out, (char) fw);
	appendStringInfoChar(out, (char) ((scale > 1 ? 1 : 0) | (fzz ? 2 : 0)));
	if (scale > 1)
		appendBinaryStringInfo(out, (const char *) &scale, sizeof(scale));
	appendBinaryStringInfo(out, (const char *) kw, nb);
	p = put_zeros(out, ((uint64) (nb - 1) * fw + 7) / 8);
	for (uint32 b = 1; b < nb; b++)
		bits_put(p, (uint64) (b - 1) * fw, delta_code(kv[b * SIFT_CGORD_SUB], kv[(b - 1) * SIFT_CGORD_SUB], fzz, scale), fw);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		from = b * SIFT_CGORD_SUB,
					to = Min(m, from + SIFT_CGORD_SUB),
					w = kw[b] & 0x7F;

		p = put_zeros(out, ((uint64) (to - from - 1) * w + 7) / 8);
		for (uint32 i = from + 1; i < to; i++)
			bits_put(p, (uint64) (i - from - 1) * w, delta_code(kv[i], kv[i - 1], kw[b] >> 7, scale), w);
	}
}

/* The address blob of a seekable chunk from the codes dv of its entries (addr_codes); false where a code takes more than 56 bits */
static bool
seek_addrs(StringInfo out, const int64 *kv, const int64 *dv, uint32 m)
{
	uint32		nb = (m + SIFT_CGORD_SUB - 1) / SIFT_CGORD_SUB,
				wa;
	uint64		amin = UINT64_MAX,
				amax = 0;
	uint8		meta[2 * SIFT_CGORD_SUBS];
	uint8	   *p;

	for (uint32 i = 0; i < m; i++)
	{
		amin = Min(amin, (uint64) dv[i]);
		amax = Max(amax, (uint64) dv[i]);
	}
	wa = bitlen(amax - amin);
	if (wa > 56)
		return false;
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		from = b * SIFT_CGORD_SUB,
					to = Min(m, from + SIFT_CGORD_SUB),
					gw = 0,
					ng = 0;
		bool		gaps = true;
		uint64		bits;

		for (uint32 i = from + 1; i < to; i++)
			if (kv[i] == kv[i - 1])
			{
				gaps &= dv[i] > dv[i - 1];
				gw = Max(gw, bitlen((uint64) dv[i] - (uint64) dv[i - 1] - 1));
				ng++;
			}
		bits = (uint64) (to - from) * wa;
		if (gaps && ng > 0 && gw <= 56 && bits > (uint64) (to - from - ng) * wa + (uint64) ng * gw)
			bits = (uint64) (to - from - ng) * wa + (uint64) ng * gw;
		else
			gw = 0xFF;
		meta[2 * b] = (uint8) gw;
		meta[2 * b + 1] = (uint8) ((bits + 7) / 8);
	}
	appendStringInfoChar(out, (char) wa);
	appendBinaryStringInfo(out, (const char *) &amin, sizeof(amin));
	appendBinaryStringInfo(out, (const char *) meta, 2 * nb);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		from = b * SIFT_CGORD_SUB,
					to = Min(m, from + SIFT_CGORD_SUB);
		uint64		bit = 0;

		p = put_zeros(out, meta[2 * b + 1]);
		for (uint32 i = from; i < to; i++)
			if (meta[2 * b] != 0xFF && i > from && kv[i] == kv[i - 1])
			{
				bits_put(p, bit, (uint64) dv[i] - (uint64) dv[i - 1] - 1, meta[2 * b]);
				bit += meta[2 * b];
			}
			else
			{
				bits_put(p, bit, (uint64) dv[i] - amin, wa);
				bit += wa;
			}
	}
	return true;
}

/* The chunk of m entries as a seekable one, written to body; false, nothing written, when its addresses do not fit */
bool
sift_cgord_seek_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m)
{
	int64	   *dv = palloc(m * sizeof(int64));
	uint32		start = body->len,
				ob = sift_cgord_addr_codes(av, m, dv);
	bool		ok;

	seek_keys(body, kv, m);
	d->klen = body->len - start;
	sift_cgord_pad8(body);
	start = body->len;
	ok = seek_addrs(body, kv, dv, m);
	d->alen = body->len - start;
	d->pad = SIFT_CGORD_SEEK | ob << 8;
	sift_cgord_pad8(body);
	pfree(dv);
	return ok;
}
