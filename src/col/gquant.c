/*
 * col/gquant.c - the quantized keys of a compact chunk of a global order (col/gquant.h)
 */
#include "core/host.h"
#include "col/gquant.h"

static inline uint32
bitlen(uint64 v)
{
	return v ? 64 - __builtin_clzll(v) : 0;
}

/* w bits (w <= 64) of the stream b at bit pos, LSB first; b has 8 readable bytes past pos / 8 */
static inline uint64
get_bits(const uint8 *b, uint64 pos, uint32 w)
{
	uint64		x;

	if (w == 0)
		return 0;
	if (w > 56)
		return get_bits(b, pos, 32) | (get_bits(b, pos + 32, w - 32) << 32);
	memcpy(&x, b + (pos >> 3), 8);
	return (x >> (pos & 7)) & (((uint64) 1 << w) - 1);
}

static inline void
put_bits(uint8 *b, uint64 pos, uint64 v, uint32 w)
{
	while (w > 0)
	{
		uint32		s = pos & 7,
					n = Min(8 - s, w);

		b[pos >> 3] = (uint8) ((b[pos >> 3] & ~((((uint32) 1 << n) - 1) << s)) | (((uint32) (v & (((uint64) 1 << n) - 1))) << s));
		v >>= n;
		pos += n;
		w -= n;
	}
}

/*
 * The shift makes about q buckets of the mean gap g = (last - first) / (m - 1) (0 when g / q is under 2), and the Rice parameter is that of a geometric difference of mean
 * mean: floor(log2(0.7 * mean)). Integers only.
 */
uint32
sift_gq_encode(const int64 *key, uint32 m, uint32 q, StringInfo out)
{
	uint64		first = (uint64) key[0],
				span = m > 1 ? (uint64) key[m - 1] - first : 0,
				tgt = m > 1 ? span / (m - 1) / q : 0,
				mean,
				qprev = 0,
				bit = SIFT_GQ_HDR * 8;
	uint32		sh = 0,
				rb = 0;
	uint8	   *buf = palloc0(SIFT_GQ_HDR + ((uint64) m * (SIFT_GQ_ESC + 1 + 6 + 64) + 7) / 8 + 8);
	uint32		len;

	if (tgt >= 2)
		sh = 63 - __builtin_clzll(tgt);
	mean = m > 1 ? (span >> sh) / (m - 1) : 0;
	if (mean * 7 / 10 >= 1)
		rb = 63 - __builtin_clzll(mean * 7 / 10);
	buf[0] = (uint8) sh;
	buf[1] = (uint8) rb;
	for (uint32 i = 1; i < m; i++)
	{
		uint64		qi = ((uint64) key[i] - first) >> sh,
					d = qi - qprev,
					un = Min(d >> rb, (uint64) SIFT_GQ_ESC);

		qprev = qi;
		for (uint64 u = 0; u < un; u++)
			put_bits(buf, bit + u, 1, 1);
		bit += un + 1;
		if (un == SIFT_GQ_ESC)
		{
			uint32		w = bitlen(d);

			put_bits(buf, bit, w - 1, 6);
			bit += 6;
			put_bits(buf, bit, d, w);
			bit += w;
		}
		else
		{
			put_bits(buf, bit, d & (((uint64) 1 << rb) - 1), rb);
			bit += rb;
		}
	}
	len = (uint32) ((bit + 7) >> 3);
	appendBinaryStringInfo(out, (const char *) buf, len);
	pfree(buf);
	return len;
}

bool
sift_gq_decode(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh_out)
{
	uint32		sh,
				rb;
	uint64		p = SIFT_GQ_HDR * 8,
				q = 0,
				lim = (uint64) len * 8;

	if (len < SIFT_GQ_HDR)
		return false;
	sh = b[0];
	rb = b[1];
	if (sh > 63 || rb > 63)
		return false;
	kmin[0] = first;
	for (uint32 i = 1; i < m; i++)
	{
		uint64		x;
		uint32		un;

		if (p >= lim)
			return false;
		memcpy(&x, b + (p >> 3), 8);
		x >>= (p & 7);
		un = (uint32) __builtin_ctzll(~x | ((uint64) 1 << 40));
		if (un > SIFT_GQ_ESC)
			return false;
		p += un + 1;
		if (un == SIFT_GQ_ESC)
		{
			uint32		w = (uint32) get_bits(b, p, 6) + 1;

			p += 6;
			q += get_bits(b, p, w);
			p += w;
		}
		else
		{
			q += ((uint64) un << rb) | get_bits(b, p, rb);
			p += rb;
		}
		if (p > lim)
			return false;
		kmin[i] = (int64) ((uint64) first + (q << sh));
	}
	*sh_out = sh;
	return true;
}

int64
sift_gq_kmax(int64 kmin, uint32 sh)
{
	uint64		w = sh >= 63 ? (uint64) PG_INT64_MAX : ((uint64) 1 << sh) - 1,
				u = (uint64) kmin + ((uint64) 1 << 63);

	if (w > UINT64_MAX - u)
		w = UINT64_MAX - u;
	return (int64) (u + w - ((uint64) 1 << 63));
}
