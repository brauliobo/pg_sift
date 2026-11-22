/*
 * col/codec/filter.c - filters and sums over an encoded chunk, in the domain of the codes
 * a constant, a bit-packed run, a dictionary, a scale, the decoded values
 * portable C
 */
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

/* The rows of the runs whose bit is set in rb: neighbouring runs are one span */
static void
emit_runs(const CcNode *nd, const uint64 *rb, uint64 *bits)
{
	uint32		start = 0;
	bool		open = false;

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
}

static void
filter_rle(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits)
{
	uint64	   *rb = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(nd->nruns));

	nd_filter(nd->a, lo, hi, sub, off32, rb);
	emit_runs(nd, rb, bits);
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

/*
 * A DELTA node whose steps never fall (or never rise): a block of 1024 rows lies between the value it starts from and the one the next block starts from, both of them kept in the checkpoints, so
 * a block that the range misses or holds whole is not unpacked; at most the blocks the range ends in are. The last block has no checkpoint after it: it is bounded by the steps the width allows,
 * and decoded when that bound wraps. false for a node whose checkpoints do not run in one direction: the caller decodes it.
 */
static bool
filter_delta(const CcNode *nd, int64 lo, int64 hi, uint64 *bits)
{
	uint32		nblks = (nd->n + 1023) / 1024;
	bool		asc = nd->mind >= 0,
				desc = !asc && nd->W < 32 && nd->mind + (int64) ((UINT64CONST(1) << nd->W) - 1) <= 0;
	int64	   *first = palloc(sizeof(int64) * nblks);

	if (!asc && !desc)
	{
		pfree(first);
		return false;
	}
	first[0] = nd->base;
	for (uint32 b = 1; b < nblks; b++)
	{
		first[b] = get64(nd->chk + 8 * (size_t) (b - 1));
		if (asc ? first[b] < first[b - 1] : first[b] > first[b - 1])
		{
			pfree(first);
			return false;
		}
	}
	for (uint32 b = 0; b < nblks; b++)
	{
		uint32		from = b * 1024,
					cnt = Min(1024, nd->n - from);
		bool		last = b + 1 == nblks;
		__int128	reach = (__int128) (cnt - 1) * (asc ? nd->mind + (int64) ((UINT64CONST(1) << nd->W) - 1) : nd->mind);
		__int128	end = (__int128) first[b] + reach;
		bool		bounded = !last || (end >= PG_INT64_MIN && end <= PG_INT64_MAX);
		int64		bmin = asc ? first[b] : (last ? (int64) end : first[b + 1]),
					bmax = asc ? (last ? (int64) end : first[b + 1]) : first[b];

		if (bounded && (hi < bmin || lo > bmax))
			memset(bits + from / 64, 0, sizeof(uint64) * ((cnt + 63) / 64));
		else if (bounded && lo <= bmin && hi >= bmax)
		{
			memset(bits + from / 64, 0xFF, sizeof(uint64) * ((cnt + 63) / 64));
			clear_tail(bits + from / 64, cnt);
		}
		else
		{
			uint32		tmp[1024 + SIFT_COL_PAD / 4];
			int64		run[1024 + SIFT_COL_PAD / 8];

			sift_cck->unpack32((const uint32 *) nd->words + (size_t) b * 32 * nd->W, nd->W, rows_for(cnt), 0, tmp);
			sift_cck->prefix64(tmp, cnt, run, first[b], nd->mind);
			sift_colk->range[3](run, cnt, (uint64) lo, (uint64) hi - (uint64) lo, bits + from / 64);
		}
	}
	pfree(first);
	return true;
}

/* DELTA of no direction, and FORBYTE of an odd width: decode, then the range kernel on the plain array */
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
		case SIFT_CCN_DELTA:
			if (filter_delta(nd, lo, hi, bits))
				return;
			break;
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

/* ---- membership of the keys in a set of the small non-negative integers: the codes of a dictionary that a predicate chose ---- */

static inline bool
is_member(const uint64 *m, uint32 nm, int64 key)
{
	return key >= 0 && (uint64) key < nm && ((m[key / 64] >> (key % 64)) & 1);
}

static void
nd_member(const CcNode *nd, const uint64 *m, uint32 nm, uint64 *bits)
{
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			if (is_member(m, nm, nd->base))
				sift_cc_set_ones(bits, nd->n);
			else
				set_zeros(bits, nd->n);
			return;
		case SIFT_CCN_WRAP:
			nd_member(nd->a, m, nm, bits);
			return;
		case SIFT_CCN_RLE:
			{
				uint64	   *rb = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(nd->nruns));

				nd_member(nd->a, m, nm, rb);
				emit_runs(nd, rb, bits);
				pfree(rb);
				return;
			}
		case SIFT_CCN_DICT:
			{
				uint64	   *cb = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(nd->d));

				for (uint32 i = 0; i < nd->d; i++)
					if (is_member(m, nm, nd->dict[i]))
						cb[i / 64] |= UINT64CONST(1) << (i % 64);
				nd_member(nd->b, cb, nd->d, bits);
				pfree(cb);
				return;
			}
		default:
			{
				int64	   *tmp = palloc(sizeof(int64) * ((size_t) nd->n + SIFT_COL_PAD / 8));

				nd_i64(nd, tmp);
				set_zeros(bits, nd->n);
				for (uint32 i = 0; i < nd->n; i++)
					bits[i / 64] |= (uint64) is_member(m, nm, tmp[i]) << (i % 64);
				pfree(tmp);
			}
	}
}

void
sift_cc_filter_member(const SiftCc *c, const uint64 *member, uint32 nmember, uint64 *bits)
{
	if (c->n == 0)
		return;
	if (c->max < 0 || c->min >= (int64) nmember)
		set_zeros(bits, c->n);
	else
		nd_member(c->root, member, nmember, bits);
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
