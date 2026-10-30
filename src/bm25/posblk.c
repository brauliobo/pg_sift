/*
 * bm25/posblk.c - the position stream of a term in blocks of SIFT_POS_EVERY postings (segment format 4; layout in bm25/posting.h):
 * the block is a byte of the width of the largest first position and whether a posting has several, then a bit stream (least
 * significant bit first): a flag a posting for the several, the first positions at that width, the Rice parameter of the block
 * (4 bits), for each posting with several its occurrences less one in Elias gamma, then the low bits of all the gaps less one
 * and the quotients of them in unary. A posting is read alone: its first position is at a fixed place, the gaps of the ones
 * before it are skipped by counting the ends of unary codes. The writer and the merge hold the records of a posting as LEB128
 * varints (bm25/analyze.h), the reader turns a block back into them (sift_posblk_leb) for what copies records.
 */
#include "core/host.h"

#include "core/varint.h"

#include "bm25/posting.h"
#include "bm25/posting_int.h"

#define BLK_WIDTH_MASK	0x3f
#define BLK_MULTI		0x40
#define RICE_BITS		4
#define RICE_MAX		15

typedef struct BitWriter
{
	uint8	   *p;
	uint64		acc;
	int			n;				/* bits in acc, below 32 between calls */
} BitWriter;

/* nb <= 32 bits of v, low bits first */
static inline void
bw_put(BitWriter *w, uint64 v, int nb)
{
	w->acc |= v << w->n;
	w->n += nb;
	if (w->n >= 32)
	{
		memcpy(w->p, &w->acc, sizeof(uint32));
		w->p += sizeof(uint32);
		w->acc >>= 32;
		w->n -= 32;
	}
}

/* q zero bits and a one */
static inline void
bw_unary(BitWriter *w, uint32 q)
{
	for (; q >= 32; q -= 32)
		bw_put(w, 0, 32);
	bw_put(w, (uint64) 1 << q, q + 1);
}

static inline void
bw_rice(BitWriter *w, uint32 g, int kr)
{
	uint32		q = g >> kr;
	uint64		r = g & (((uint64) 1 << kr) - 1);

	if (q + 1 + kr <= 32)
		bw_put(w, r << (q + 1) | (uint64) 1 << q, q + 1 + kr);
	else
	{
		bw_unary(w, q);
		bw_put(w, r, kr);
	}
}

/*
 * The Rice parameter of n gaps that add up to sum: the best of the three around the one their mean says, by the bits a geometric
 * distribution of that mean needs; *bits: the bits of all their codes, at most
 */
static int
rice_param(uint32 n, uint64 sum, uint64 *bits)
{
	int			lo = Max(pg_leftmost_one_pos64(sum / n + 1) - 1, 0),
				best = lo;

	for (int kr = lo; kr <= Min(lo + 2, RICE_MAX); kr++)
	{
		uint64		b = (uint64) n * kr + (sum >> kr);

		if (kr == lo || b < *bits)
			*bits = b, best = kr;
	}
	*bits += n;
	return best;
}

void
sift_posbuf_grow(SiftPosBuf *out, uint32 n)
{
	out->cap = Max(n, Max(64, out->cap * 2));
	out->v = out->v ? repalloc_huge(out->v, (Size) out->cap * sizeof(uint32)) :
		MemoryContextAllocHuge(out->mcxt, (Size) out->cap * sizeof(uint32));
}

/* LEB128 at *pp, which the writer made */
static inline uint32
leb(const uint8 **pp)
{
	const uint8 *p = *pp;
	uint32		v = 0;
	int			s = 0;
	uint8		b;

	if (*p < 0x80)
	{
		*pp = p + 1;
		return *p;
	}
	do
	{
		b = *p++;
		v |= (uint32) (b & 0x7f) << s;
		s += 7;
	} while (b & 0x80);
	*pp = p;
	return v;
}

/* Appends to out the block of the k postings whose records are at p (len bytes) */
void
sift_posblk_put(const uint8 *p, Size len, uint32 k, StringInfo out)
{
	uint32		first[SIFT_POS_EVERY],
				cnt[SIFT_POS_EVERY],
				sbuf[512],
			   *gap = len <= lengthof(sbuf) ? sbuf : palloc(len * sizeof(uint32)),
				ngap = 0,
				maxfirst = 0,
				multi = 0,
				v;
	uint64		gsum = 0,
				bits,
				gbits = 0;
	int			wf,
				kr = 0;
	BitWriter	w = {0};

	for (uint32 i = 0; i < k; i++)
	{
		v = leb(&p);
		first[i] = v >> 1;
		maxfirst |= first[i];
		cnt[i] = 1;
		if (v & 1)
		{
			cnt[i] = leb(&p) + 2;
			multi |= 1U << i;
			for (uint32 j = 1; j < cnt[i]; j++)
			{
				v = leb(&p);
				gap[ngap++] = v;
				gsum += v;
			}
		}
	}
	wf = maxfirst ? pg_leftmost_one_pos32(maxfirst) + 1 : 0;
	bits = (uint64) k * wf;
	if (multi)
	{
		bits += k + RICE_BITS;
		kr = rice_param(ngap, gsum, &gbits);
		bits += gbits;
		for (uint32 i = 0; i < k; i++)
			if (multi >> i & 1)
				bits += 2 * pg_leftmost_one_pos32(cnt[i] - 1) + 1;
	}
	enlargeStringInfo(out, 1 + bits / 8 + 8);
	w.p = (uint8 *) out->data + out->len;
	*w.p++ = wf | (multi ? BLK_MULTI : 0);
	if (multi)
		bw_put(&w, multi, k);
	for (uint32 i = 0; i < k; i++)
		bw_put(&w, first[i], wf);
	if (multi)
	{
		bw_put(&w, kr, RICE_BITS);
		for (uint32 i = 0; i < k; i++)
		{
			uint32		nb;

			if (!(multi >> i & 1))
				continue;
			nb = pg_leftmost_one_pos32(cnt[i] - 1);
			bw_unary(&w, nb);
			bw_put(&w, (cnt[i] - 1) & ((1U << nb) - 1), nb);
		}
		for (uint32 j = 0; j < ngap; j++)
			bw_put(&w, gap[j] & (((uint32) 1 << kr) - 1), kr);
		for (uint32 j = 0; j < ngap; j++)
			bw_unary(&w, gap[j] >> kr);
	}
	for (; w.n > 0; w.n -= 8, w.acc >>= 8)
		*w.p++ = (uint8) w.acc;
	out->len = (char *) w.p - out->data;
	if (gap != sbuf)
		pfree(gap);
}

/* Bits of a block: nb <= 56 at bit bp (the 8 bytes from the byte of bp are readable) */
static inline uint64
br_get(const uint8 *b, uint64 bp, int nb)
{
	uint64		x;

	memcpy(&x, b + (bp >> 3), sizeof(x));
	return (x >> (bp & 7)) & (((uint64) 1 << nb) - 1);
}

/* The number of zero bits before the next one, which is consumed too; false when the block ends first */
static inline bool
br_unary(const uint8 *b, uint64 *bp, uint64 nbits, uint32 *q)
{
	uint64		x;

	for (uint64 n = 0;;)
	{
		if (*bp >= nbits)
			return false;
		memcpy(&x, b + (*bp >> 3), sizeof(x));
		x >>= *bp & 7;
		if (x)
		{
			uint32		z = pg_rightmost_one_pos64(x);

			*bp += z + 1;
			n += z;
			*q = n;
			return *bp <= nbits && n <= PG_UINT32_MAX;
		}
		n += 56;
		*bp += 56;
	}
}

/* Opens the block of the k postings (len bytes, 8 more readable behind it); false when it is malformed */
bool
sift_posblk_open(SiftPosBlk *bk, const uint8 *b, uint32 len, uint32 k)
{
	uint64		nbits = (uint64) len * 8,
				bp = 8,
				gaps = 0;
	uint32		multi = 0;

	if (len < 1 || (bk->wf = b[0] & BLK_WIDTH_MASK) > 32)
		return false;
	bk->b = b;
	bk->nbits = nbits;
	bk->k = k;
	if (b[0] & BLK_MULTI)
	{
		multi = br_get(b, bp, k);
		bp += k;
	}
	bk->multi = multi;
	bk->first_bp = bp;
	bp += (uint64) k * bk->wf;
	bk->end_bp = bp;
	if (bp > nbits || ((b[0] & BLK_MULTI) != 0) != (multi != 0))
		return false;
	if (multi)
	{
		if (bp + RICE_BITS > nbits)
			return false;
		bk->kr = br_get(b, bp, RICE_BITS);
		bp += RICE_BITS;
		for (uint32 i = 0; i < k; i++)
		{
			uint32		nb,
						x;

			bk->gofs[i] = gaps;
			bk->cnt[i] = 1;
			if (!(multi >> i & 1))
				continue;
			if (!br_unary(b, &bp, nbits, &nb) || nb > 31)
				return false;
			x = (1U << nb) | br_get(b, bp, nb);
			bp += nb;
			if (bp > nbits || x > nbits)
				return false;
			bk->cnt[i] = x + 1;
			gaps += x;
		}
		bk->gofs[k] = gaps;
		bk->rb = bp;
		bk->ub = bk->upos = bp + gaps * bk->kr;
		bk->ug = 0;
		if (bk->ub + gaps > nbits)
			return false;
	}
	else
	{
		for (uint32 i = 0; i < k; i++)
			bk->cnt[i] = 1;
		memset(bk->gofs, 0, sizeof(bk->gofs));
	}
	return true;
}

/* Moves the scan of the unary codes to the start of the code of gap g */
static bool
unary_seek(SiftPosBlk *bk, uint64 g)
{
	uint64		skip;

	if (g < bk->ug)
		bk->upos = bk->ub, bk->ug = 0;
	skip = g - bk->ug;
	bk->ug = g;
	while (skip > 0)
	{
		uint64		x;
		uint32		sh,
					c;

		if (bk->upos >= bk->nbits)
			return false;
		memcpy(&x, bk->b + (bk->upos >> 3), sizeof(x));
		sh = bk->upos & 7;
		x >>= sh;
		c = pg_popcount64(x);
		if (c < skip)
		{
			skip -= c;
			bk->upos += 64 - sh;
			continue;
		}
		for (; skip > 1; skip--)
			x &= x - 1;
		bk->upos += pg_rightmost_one_pos64(x) + 1;
		skip = 0;
	}
	return bk->upos <= bk->nbits;
}

/* The next gap less one of the posting being read, whose low bits are at *rbp; false when the block ends first */
static inline bool
next_gap(SiftPosBlk *bk, uint64 *rbp, uint32 *g)
{
	uint64		x;
	uint32		q;

	memcpy(&x, bk->b + (bk->upos >> 3), sizeof(x));
	x >>= bk->upos & 7;
	if (x)
	{
		q = pg_rightmost_one_pos64(x);
		bk->upos += q + 1;
	}
	else if (!br_unary(bk->b, &bk->upos, bk->nbits, &q))
		return false;
	*g = (q << bk->kr) | (uint32) br_get(bk->b, *rbp, bk->kr);
	*rbp += bk->kr;
	return bk->upos <= bk->nbits;
}

/* The occurrences of posting i of the block into v (SiftPosBlk.cnt[i] of them); false when the block is malformed */
bool
sift_posblk_get(SiftPosBlk *bk, uint32 i, uint32 *v)
{
	uint32		n = bk->cnt[i],
				pos = br_get(bk->b, bk->first_bp + (uint64) i * bk->wf, bk->wf);
	uint64		rbp;

	*v = pos;
	if (n == 1)
		return true;
	if (!unary_seek(bk, bk->gofs[i]))
		return false;
	rbp = bk->rb + (uint64) bk->gofs[i] * bk->kr;
	for (uint32 j = 1; j < n; j++)
	{
		uint32		g;

		if (!next_gap(bk, &rbp, &g))
			return false;
		pos += g + 1;
		v[j] = pos;
	}
	bk->ug += n - 1;
	return true;
}

/*
 * Appends to out the records of the k postings of the block (len bytes, 8 more readable behind it) as the writer takes them, lend[i]
 * being where the one of posting i ends in out; false when the block is not exactly what sift_posblk_put makes
 */
bool
sift_posblk_leb(const uint8 *b, uint32 len, uint32 k, StringInfo out, Size *lend)
{
	SiftPosBlk	bk;
	uint8	   *p;
	uint64		rbp;

	if (!sift_posblk_open(&bk, b, len, k))
		return false;
	enlargeStringInfo(out, (Size) 5 * (2 * k + bk.gofs[k]) + 8);
	p = (uint8 *) out->data + out->len;
	rbp = bk.rb;
	for (uint32 i = 0; i < k; i++)
	{
		uint32		n = bk.cnt[i];
		uint64		v = br_get(b, bk.first_bp + (uint64) i * bk.wf, bk.wf) << 1 | (n > 1);

		if (v < 0x80)
			*p++ = (uint8) v;
		else
			p = sift_put_varint(p, v);
		if (n > 1)
		{
			p = sift_put_varint(p, n - 2);
			for (uint32 j = 1; j < n; j++)
			{
				uint32		g;

				if (!next_gap(&bk, &rbp, &g))
					return false;
				if (g < 0x80)
					*p++ = (uint8) g;
				else
					p = sift_put_varint(p, g);
			}
		}
		lend[i] = (char *) p - out->data;
	}
	out->len = (char *) p - out->data;
	return (((bk.multi ? bk.upos : bk.end_bp) + 7) / 8) == len;
}
