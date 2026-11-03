/*
 * col/codec/filter.c - filters and sums over an encoded chunk, in the domain of the codes
 * a constant, a bit-packed run, a dictionary, a scale, the decoded values
 * portable C
 */
#include "core/host.h"
#include "core/popcount.h"
#include "core/cmp.h"
#include "col/codec/decode_int.h"

static void
set_zeros(uint64 *bits, uint32 n)
{
	memset(bits, 0, sizeof(uint64) * ((n + 63) / 64));
}

/* bits [from, to) set */
static void
set_range(uint64 *bits, uint32 from, uint32 to)
{
	while (from < to)
	{
		uint32		w = from / 64,
					lo = from % 64,
					cnt = to - from < 64 - lo ? to - from : 64 - lo;
		uint64		m = cnt == 64 ? ~UINT64CONST(0) : ((UINT64CONST(1) << cnt) - 1) << lo;

		bits[w] |= m;
		from += cnt;
	}
}

/* set bits in [from, to) */
static uint64
count_range(const uint64 *bits, uint32 from, uint32 to)
{
	uint64		c = 0;

	while (from < to)
	{
		uint32		w = from / 64,
					lo = from % 64,
					cnt = to - from < 64 - lo ? to - from : 64 - lo;
		uint64		m = cnt == 64 ? ~UINT64CONST(0) : ((UINT64CONST(1) << cnt) - 1) << lo;

		c += sift_pc64(bits[w] & m);
		from += cnt;
	}
	return c;
}

/* ---- filters on the encoded form ---- */

/*
 * [lo, hi] as offsets from base within width bits (maxoff = largest offset, or UINT64_MAX): false when nothing can match, else the
 * offsets in *lo_off and *span (hi - lo)
 */
static bool
off_range(int64 base, uint64 maxoff, int64 lo, int64 hi, uint64 *lo_off, uint64 *span)
{
	uint64		l,
				h;

	if (hi < base)
		return false;
	l = lo <= base ? 0 : (uint64) lo - (uint64) base;
	h = (uint64) hi - (uint64) base;
	if (l > maxoff)
		return false;
	if (h > maxoff)
		h = maxoff;
	*lo_off = l;
	*span = h - l;
	return true;
}

static void nd_filter(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits);

/* digits d with key(decode(d)) in [lo, hi] form one interval of [dmin, dmax] (decode is monotone); *dlo > *dhi when empty */
static void
alp_digit_range(const CcNode *nd, int64 lo, int64 hi, int64 *dlo, int64 *dhi)
{
	int64		a = nd->dmin,
				b = nd->dmax + 1;

	SIFT_BISECT(int64, m, a, b, cc_alp_key(nd, m) < lo);
	*dlo = a;
	a = nd->dmin;
	b = nd->dmax + 1;
	SIFT_BISECT(int64, m, a, b, cc_alp_key(nd, m) <= hi);
	*dhi = a - 1;
}

static void nd_filter(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits);

static void
filter_const(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	if (nd->base >= lo && nd->base <= hi)
		sift_cc_set_ones(bits, nd->n);
	else
		set_zeros(bits, nd->n);
}

static void
filter_forbit(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	uint64		l,
				span;

	if (!off_range(nd->base, nd->W >= 32 ? 0xFFFFFFFFu : (UINT64CONST(1) << nd->W) - 1, lo, hi, &l, &span))
	{
		set_zeros(bits, nd->n);
		return;
	}
	sift_cck->unpack_range32((const uint32 *) nd->words, nd->W, rows_for(nd->n), (uint32) l, (uint32) span, bits);
	clear_tail(bits, nd->n);
}

/* false for a width without a range kernel: the caller decodes the node */
static bool
filter_forbyte(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	uint64		l,
				span;

	if (nd->B != 1 && nd->B != 2 && nd->B != 4 && nd->B != 8)
		return false;
	if (!off_range(nd->base, nd->B == 8 ? ~UINT64CONST(0) : (UINT64CONST(1) << (8 * nd->B)) - 1, lo, hi, &l, &span))
		set_zeros(bits, nd->n);
	else
		sift_colk->range[nd->B == 1 ? 0 : nd->B == 2 ? 1 : nd->B == 4 ? 2 : 3] (nd->words, nd->n, l, span, bits);
	return true;
}

static void
filter_rle(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits)
{
	uint64	   *rb = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(nd->nruns));
	uint32		start = 0;
	bool		open = false;

	nd_filter(nd->a, lo, hi, sub, off32, rb);
	set_zeros(bits, nd->n);
	for (uint32 r = 0; r < nd->nruns; r++)
	{
		bool		hit = (rb[r / 64] >> (r % 64)) & 1;

		if (hit && !open)
		{
			start = nd->rstart[r];
			open = true;
		}
		else if (!hit && open)
		{
			set_range(bits, start, nd->rstart[r]);
			open = false;
		}
	}
	if (open)
		set_range(bits, start, nd->n);
	pfree(rb);
}

static void
filter_dict(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	uint32		a = 0,
				b = nd->d,
				cl,
				ch;

	SIFT_BISECT(uint32, m, a, b, nd->dict[m] < lo);
	cl = a;
	b = nd->d;
	SIFT_BISECT(uint32, m, a, b, nd->dict[m] <= hi);
	ch = a;
	if (cl >= ch)
		set_zeros(bits, nd->n);
	else
		nd_filter(nd->b, cl, (int64) ch - 1, 0, true, bits);
}

static void
filter_alp(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	int64		dlo,
				dhi;

	alp_digit_range(nd, lo, hi, &dlo, &dhi);
	if (dlo > dhi)
		set_zeros(bits, nd->n);
	else
		nd_filter(nd->a, dlo, dhi, nd->dmin, true, bits);
	for (uint32 k = 0; k < nd->nexc; k++)
	{
		uint32		p = nd->pos[k];
		int64		key = get64(nd->ekeys + 8 * (size_t) k);

		bits[p / 64] &= ~(UINT64CONST(1) << (p % 64));
		if (key >= lo && key <= hi)
			bits[p / 64] |= UINT64CONST(1) << (p % 64);
	}
}

static void
filter_scale(const CcNode *nd, int64 lo, int64 hi, bool off32, uint64 *bits)
{
	/* base + g * w in [lo, hi]  <=>  w in [ceil((lo - base) / g), floor((hi - base) / g)] */
	uint64		g = (uint64) nd->mind,
				d = lo <= nd->base ? 0 : (uint64) lo - (uint64) nd->base,
				wlo = d / g + (d % g != 0),
				whi;

	if (hi < nd->base)
		whi = 0, wlo = 1;
	else
		whi = ((uint64) hi - (uint64) nd->base) / g;
	if (wlo > whi || wlo > (uint64) INT64_MAX)
		set_zeros(bits, nd->n);
	else
		nd_filter(nd->a, (int64) wlo, (int64) whi, 0, off32, bits);
}

/* DELTA, and FORBYTE of an odd width: decode, then the range kernel on the plain array */
static void
filter_decoded(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits)
{
	uint32		n = nd->n;
	uint64		l,
				span;

	if (off32)
	{
		uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

		if (!off_range(sub, 0xFFFFFFFFu, lo, hi, &l, &span))
			set_zeros(bits, n);
		else
		{
			nd_u32(nd, sub, tmp);
			sift_colk->range[2](tmp, n, l, span, bits);
		}
		pfree(tmp);
	}
	else
	{
		int64	   *tmp = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

		nd_i64(nd, tmp);
		sift_colk->range[3](tmp, n, (uint64) lo, (uint64) hi - (uint64) lo, bits);
		pfree(tmp);
	}
}

static void
nd_filter(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits)
{
	if (lo > hi)
	{
		set_zeros(bits, nd->n);
		return;
	}
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			filter_const(nd, lo, hi, bits);
			return;
		case SIFT_CCN_FORBIT:
			filter_forbit(nd, lo, hi, bits);
			return;
		case SIFT_CCN_FORBYTE:
			if (filter_forbyte(nd, lo, hi, bits))
				return;
			break;
		case SIFT_CCN_RLE:
			filter_rle(nd, lo, hi, sub, off32, bits);
			return;
		case SIFT_CCN_DICT:
			filter_dict(nd, lo, hi, bits);
			return;
		case SIFT_CCN_ALP:
			filter_alp(nd, lo, hi, bits);
			return;
		case SIFT_CCN_SCALE:
			filter_scale(nd, lo, hi, off32, bits);
			return;
		case SIFT_CCN_WRAP:
			nd_filter(nd->a, lo, hi, sub, off32, bits);
			return;
	}
	filter_decoded(nd, lo, hi, sub, off32, bits);
}

void
sift_cc_filter(const SiftCc *c, int64 lo, int64 hi, uint64 *bits)
{
	if (c->n == 0)
		return;
	if (lo > hi || hi < c->min || lo > c->max)
		set_zeros(bits, c->n);
	else if (lo <= c->min && hi >= c->max)
		sift_cc_set_ones(bits, c->n);
	else
		nd_filter(c->root, lo, hi, c->min, (c->flags & SIFT_CCF_OFF32) != 0, bits);
}

uint64
sift_cc_sum(const SiftCc *c, const uint64 *bits, int64 *sum)
{
	const CcNode *nd = unwrap(c->root);
	uint64		cnt = 0;
	uint64		s = 0;
	uint32		n = c->n;

	if (n == 0)
	{
		*sum = 0;
		return 0;
	}
	cnt = bits ? count_range(bits, 0, n) : n;
	if (nd->tag == SIFT_CCN_CONST)
		s = cnt * (uint64) nd->base;
	else if (nd->tag == SIFT_CCN_RLE)
	{
		int64	   *rv = palloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));

		nd_i64(nd->a, rv);
		for (uint32 r = 0; r < nd->nruns; r++)
			s += (uint64) rv[r] * (bits ? count_range(bits, nd->rstart[r], nd->rstart[r + 1]) : nd->rstart[r + 1] - nd->rstart[r]);
		pfree(rv);
	}
	else
	{
		int64	   *all = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

		nd_i64(nd, all);
		for (uint32 i = 0; i < n; i++)
			if (!bits || ((bits[i / 64] >> (i % 64)) & 1))
				s += (uint64) all[i];
		pfree(all);
	}
	*sum = (int64) s;
	return cnt;
}
