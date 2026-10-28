/*
 * col/codec.c - the chunk codec of the column engine: the writer (statistics, exact-size choice of the lightweight cascade,
 * general-purpose layer under the cost rule), the reader (validation, decoding, filters on the encoded form, random access), the policy
 * parser and the string and bit-vector blobs. Format and rationale:
 * The writer is in codec_write.c, the policy parser in codec_policy.c, the parser of a chunk in codec_parse.c, the blobs in
 * codec_blob.c; what they share is codec_int.h. This file holds decoding, random access and the filters on the encoded form.
 *
 * Every node is 8-byte aligned and a multiple of 8 bytes: a 16-byte header (tag, one parameter byte, a u32 count, 8 bytes of fields) and
 * its data; blobs start 8-byte aligned (the engine's chunks do), so all loads below are aligned. Little-endian hosts only, as every
 * on-disk format of pg_sift.
 */
#include "core/host.h"

#include "col/codec_int.h"
#include "core/popcount.h"
#include "core/cmp.h"

void
sift_cc_set_ones(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		bits[w] = ~UINT64CONST(0);
	clear_tail(bits, n);
}

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

/* ---- decoding ---- */

/* The B-byte little-endian values of a FORBYTE node plus a base, wrapped to the domain: a loop per width, so the compiler vectorizes it */
#define CC_BYTES_LOOP(T, expr) \
	for (uint32 i = 0; i < n; i++) \
	{ \
		T			x; \
		memcpy(&x, s + (size_t) i * sizeof(T), sizeof(T)); \
		dst[i] = (expr); \
	}

/* A width of 3, 5, 6 or 7 bytes: one 8-byte load and a mask per value (a memcpy of B bytes is a library call per row); the last values, whose load would run past the node, are copied */
#define CC_ODD_LOOP(w, expr) \
	{ \
		const uint32 B_ = (w), \
					fast = n > 8 ? n - (8 + B_ - 1) / B_ : 0; \
		const uint64 mask = (UINT64CONST(1) << (8 * B_)) - 1; \
		uint32		i = 0; \
		for (; i < fast; i++) \
		{ \
			uint64		x; \
			memcpy(&x, s + (size_t) i * B_, 8); \
			x &= mask; \
			dst[i] = (expr); \
		} \
		for (; i < n; i++) \
		{ \
			uint64		x = 0; \
			memcpy(&x, s + (size_t) i * B_, B_); \
			dst[i] = (expr); \
		} \
	}

static void
forbyte_u32(const CcNode *nd, uint32 off, uint32 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, off + x) break;
		case 2:
			CC_BYTES_LOOP(uint16, off + x) break;
		case 4:
			CC_BYTES_LOOP(uint32, off + x) break;
		case 3:
			CC_ODD_LOOP(3, off + (uint32) x) break;
		default:
			for (uint32 i = 0; i < n; i++)
			{
				uint64		x = 0;

				memcpy(&x, s + (size_t) i * nd->B, nd->B);
				dst[i] = off + (uint32) x;
			}
	}
}

static void
forbyte_i64(const CcNode *nd, int64 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;
	uint64		base = (uint64) nd->base;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, (int64) (base + x)) break;
		case 2:
			CC_BYTES_LOOP(uint16, (int64) (base + x)) break;
		case 4:
			CC_BYTES_LOOP(uint32, (int64) (base + x)) break;
		case 8:
			CC_BYTES_LOOP(uint64, (int64) (base + x)) break;
		case 3:
			CC_ODD_LOOP(3, (int64) (base + x)) break;
		case 5:
			CC_ODD_LOOP(5, (int64) (base + x)) break;
		case 6:
			CC_ODD_LOOP(6, (int64) (base + x)) break;
		case 7:
			CC_ODD_LOOP(7, (int64) (base + x)) break;
		default:
			Assert(false);
	}
}

/* the u32 domain: dst[i] = value i - sub, for nodes whose values all lie in [sub, sub + 2^32); dst has CC_CAP32(n) slots */
void
nd_u32(const CcNode *nd, int64 sub, uint32 *dst)
{
	uint32		n = nd->n;
	uint32		off = (uint32) ((uint64) nd->base - (uint64) sub);

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = off;
			break;
		case SIFT_CCN_FORBIT:
			sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), off, dst);
			break;
		case SIFT_CCN_FORBYTE:
			forbyte_u32(nd, off, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix32(tmp, n, dst, off, (uint32) nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				uint32	   *rv = palloc(sizeof(uint32) * CC_CAP32(nd->nruns)),
						   *rl = palloc(sizeof(uint32) * CC_CAP32(nd->nruns));

				nd_u32(nd->a, sub, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					rl[r] = nd->rstart[r + 1] - nd->rstart[r];
				sift_cck->rle_expand32(rv, rl, nd->nruns, dst);
				pfree(rv);
				pfree(rl);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *dv = palloc(sizeof(uint32) * CC_CAP32(nd->d)),
						   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				for (uint32 i = 0; i < nd->d; i++)
					dv[i] = (uint32) ((uint64) nd->dict[i] - (uint64) sub);
				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->gather_u32(dv, codes, n, dst);
				pfree(dv);
				pfree(codes);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_u32(nd->a, 0, dst);
			sift_cck->scale32(dst, n, (uint32) ((uint64) nd->base - (uint64) sub), (uint32) nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_u32(nd->a, sub, dst);
			break;
		default:
			{
				int64	   *tmp = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

				nd_i64(nd, tmp);
				for (uint32 i = 0; i < n; i++)
					dst[i] = (uint32) ((uint64) tmp[i] - (uint64) sub);
				pfree(tmp);
				break;
			}
	}
}

void
nd_i64(const CcNode *nd, int64 *dst)
{
	uint32		n = nd->n;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = nd->base;
			break;
		case SIFT_CCN_FORBIT:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, n, nd->base, dst);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_FORBYTE:
			forbyte_i64(nd, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix64(tmp, n, dst, nd->base, nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				int64	   *rv = palloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));

				nd_i64(nd->a, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					for (uint32 j = nd->rstart[r]; j < nd->rstart[r + 1]; j++)
						dst[j] = rv[r];
				pfree(rv);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->for_i64_sel[SIFT_COLW_64](nd->dict, codes, n, 0, dst);
				pfree(codes);
				break;
			}
		case SIFT_CCN_ALP:
			{
				uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->a, nd->dmin, dg);
				if (nd->kind == SIFT_CC_F8)
					sift_cck->alp_keys_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
				else
					sift_cck->alp_keys_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
				for (uint32 k = 0; k < nd->nexc; k++)
					memcpy(dst + nd->pos[k], nd->ekeys + 8 * (size_t) k, 8);
				pfree(dg);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_i64(nd->a, dst);
			sift_cck->scale64(dst, n, nd->base, nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_i64(nd->a, dst);
			break;
	}
}

void
sift_cc_decode(const SiftCc *c, int64 *dst)
{
	if (c->n)
		nd_i64(c->root, dst);
}

/* The first w keys of a node: the nodes that store a row after another (a constant, frame of reference, delta) decode w rows and no more, the others all of them */
static void
nd_i64_head(const CcNode *nd, int64 *dst, uint32 w)
{
	CcNode		h;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
		case SIFT_CCN_FORBIT:
		case SIFT_CCN_FORBYTE:
		case SIFT_CCN_DELTA:
			h = *nd;
			h.n = w;
			nd_i64(&h, dst);
			break;
		case SIFT_CCN_SCALE:
			nd_i64_head(nd->a, dst, w);
			sift_cck->scale64(dst, w, nd->base, nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_i64_head(nd->a, dst, w);
			break;
		default:
			{
				int64	   *all = palloc(sizeof(int64) * ((size_t) nd->n + SIFT_COL_PAD / 8));

				nd_i64(nd, all);
				memcpy(dst, all, sizeof(int64) * w);
				pfree(all);
			}
	}
}

void
sift_cc_decode_head(const SiftCc *c, uint32 w, int64 *dst)
{
	if (w >= c->n)
		sift_cc_decode(c, dst);
	else if (w > 0)
		nd_i64_head(c->root, dst, w);
}

/* The doubles of the keys: an ALP node is divided straight into doubles (its exceptions are keys), any other node decodes the keys and turns them in place */
static void
nd_f8(const CcNode *nd, double *dst)
{
	uint32		n = nd->n;

	if (nd->tag == SIFT_CCN_WRAP)
		nd_f8(nd->a, dst);
	else if (nd->tag == SIFT_CCN_ALP)
	{
		uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

		nd_u32(nd->a, nd->dmin, dg);
		if (nd->kind == SIFT_CC_F8)
			sift_cck->alp_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
		else
			sift_cck->alp_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
		for (uint32 k = 0; k < nd->nexc; k++)
			dst[nd->pos[k]] = sift_col_f8_from_key((int64) get64(nd->ekeys + 8 * (size_t) k));
		pfree(dg);
	}
	else
	{
		nd_i64(nd, (int64 *) dst);
		sift_colk->f8_from_keys((int64 *) dst, n);
	}
}

void
sift_cc_decode_f8(const SiftCc *c, double *dst)
{
	if (c->n)
		nd_f8(c->root, dst);
}

void
sift_cc_decode_u32(const SiftCc *c, uint32 *dst)
{
	if (c->n)
		nd_u32(c->root, 0, dst);
}

bool
sift_cc_decode_off32(const SiftCc *c, uint32 *dst)
{
	if (!(c->flags & SIFT_CCF_OFF32))
		return false;
	if (c->n)
		nd_u32(c->root, c->min, dst);
	return true;
}

/* ---- random access ---- */

/* A bit-packed lane: the 32-row group of a block starts at `a`, the width is W, sh is the bit offset in the word of the group, and mask keeps W bits */
typedef struct ForbitGroup
{
	const uint32 *a;
	uint32		l;				/* lane of the row in the group */
	uint32		sh;
	uint32		mask;
	bool		spill;			/* the value goes on in the next words of the group */
} ForbitGroup;

static inline ForbitGroup
forbit_group(const uint8 *words, uint32 W, uint32 i)
{
	uint32		j = i % 1024,
				bp = (j / 32) * W;

	return (ForbitGroup) {
		.a = (const uint32 *) words + (size_t) (i / 1024) * 32 * W + (size_t) (bp >> 5) * 32,
		.l = j % 32, .sh = bp & 31, .mask = W >= 32 ? 0xFFFFFFFFu : (1u << W) - 1, .spill = (bp & 31) + W > 32
	};
}

/* spill is g->spill, passed so that a loop over a group tests it once (the callers inline it as a constant) */
static pg_attribute_always_inline uint32
forbit_lane(const ForbitGroup *g, uint32 l, bool spill)
{
	uint32		x = sift_cc_ld32(g->a + l) >> g->sh;

	if (spill)
		x |= sift_cc_ld32(g->a + 32 + l) << (32 - g->sh);
	return x & g->mask;
}

static pg_attribute_always_inline void
forbit_fill(const ForbitGroup *g, uint32 n, int64 base, bool spill, int64 *dst)
{
	for (uint32 k = 0; k < n; k++)
		dst[k] = (int64) ((uint64) base + forbit_lane(g, g->l + k, spill));
}

static uint32
forbit_at(const uint8 *words, uint32 W, uint32 i)
{
	ForbitGroup g = forbit_group(words, W, i);

	return forbit_lane(&g, g.l, g.spill);
}

static int64
alp_key(const CcNode *nd, int64 d)
{
	if (nd->kind == SIFT_CC_F8)
		return sift_col_f8_key((double) d / sift_cc_p10[nd->e]);
	return sift_col_f8_key((double) ((float) (int32) d / sift_cc_p10f[nd->e]));
}

static int64
nd_get(const CcNode *nd, uint32 i)
{
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			return nd->base;
		case SIFT_CCN_FORBIT:
			return (int64) ((uint64) nd->base + forbit_at(nd->words, nd->W, i));
		case SIFT_CCN_FORBYTE:
			{
				uint64		x = 0;

				memcpy(&x, nd->words + (size_t) i * nd->B, nd->B);
				return (int64) ((uint64) nd->base + x);
			}
		case SIFT_CCN_DELTA:
			{
				uint32		blk = i / 1024,
							upto = i % 1024;
				uint64		run = (uint64) (blk ? get64(nd->chk + 8 * (size_t) (blk - 1)) : nd->base);

				if (upto)
				{
					uint32		tmp[1024 + SIFT_COL_PAD / 4];

					sift_cck->unpack32((const uint32 *) nd->words + (size_t) blk * 32 * nd->W, nd->W, upto / 32 + 1, 0, tmp);
					for (uint32 j = 1; j <= upto; j++)
						run += (uint64) tmp[j] + (uint64) nd->mind;
				}
				return (int64) run;
			}
		case SIFT_CCN_RLE:
			{
				uint32		lo = 0,
							hi = nd->nruns;

				while (hi - lo > 1)
				{
					uint32		m = (lo + hi) / 2;

					if (nd->rstart[m] <= i)
						lo = m;
					else
						hi = m;
				}
				return nd_get(nd->a, lo);
			}
		case SIFT_CCN_DICT:
			{
				int64		code = nd_get(nd->b, i);

				if (code < 0 || code >= (int64) nd->d)
					CC_CORRUPT("dictionary code out of range");
				return nd->dict[code];
			}
		case SIFT_CCN_ALP:
			{
				uint32		lo = 0,
							hi = nd->nexc;

				SIFT_BISECT(uint32, m, lo, hi, nd->pos[m] < i);
				if (lo < nd->nexc && nd->pos[lo] == i)
					return get64(nd->ekeys + 8 * (size_t) lo);
				return alp_key(nd, nd_get(nd->a, i));
			}
		case SIFT_CCN_SCALE:
			return (int64) ((uint64) nd->base + (uint64) nd->mind * (uint64) nd_get(nd->a, i));
		case SIFT_CCN_WRAP:
			return nd_get(nd->a, i);
	}
	return 0;
}

/* The values of rows [s, s + len) of a node: a run costs a few instructions a value, where nd_get descends the tree for each row */
static void
nd_range(const CcNode *nd, uint32 s, uint32 len, int64 *dst)
{
	switch (nd->tag)
	{
		case SIFT_CCN_FORBIT:
			for (uint32 i = 0; i < len;)
			{
				ForbitGroup g = forbit_group(nd->words, nd->W, s + i);
				uint32		take = Min(32 - g.l, len - i);

				if (g.spill)
					forbit_fill(&g, take, nd->base, true, dst + i);
				else
					forbit_fill(&g, take, nd->base, false, dst + i);
				i += take;
			}
			return;
		case SIFT_CCN_DICT:
			{
				int64		codes[64];

				for (uint32 off = 0; off < len; off += 64)
				{
					uint32		w = Min(64, len - off);

					nd_range(nd->b, s + off, w, codes);
					for (uint32 i = 0; i < w; i++)
					{
						if (codes[i] < 0 || codes[i] >= (int64) nd->d)
							CC_CORRUPT("dictionary code out of range");
						dst[off + i] = nd->dict[codes[i]];
					}
				}
				return;
			}
		case SIFT_CCN_SCALE:
			nd_range(nd->a, s, len, dst);
			for (uint32 i = 0; i < len; i++)
				dst[i] = (int64) ((uint64) nd->base + (uint64) nd->mind * (uint64) dst[i]);
			return;
		case SIFT_CCN_WRAP:
			nd_range(nd->a, s, len, dst);
			return;
		default:
			for (uint32 i = 0; i < len; i++)
				dst[i] = nd_get(nd, s + i);
	}
}

int64
sift_cc_get(const SiftCc *c, uint32 row)
{
	SIFT_INVARIANT(row < c->n, "row %u is past the end of a column chunk of %u rows", row, c->n);
	return nd_get(c->root, row);
}

/* A delta-coded value is the sum of the deltas before it in its block of 1024: a lookup costs a block's prefix, not a few operations */
static bool
nd_has_delta(const CcNode *nd)
{
	return nd != NULL && (nd->tag == SIFT_CCN_DELTA || nd_has_delta(nd->a) || nd_has_delta(nd->b));
}

void
sift_cc_decode_sel(const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst)
{
	if (m == 0)
		return;
	if ((uint64) m * (nd_has_delta(c->root) ? 256 : 8) > c->n)
	{
		int64	   *all = palloc(sizeof(int64) * ((size_t) c->n + SIFT_COL_PAD / 8));

		nd_i64(c->root, all);
		for (uint32 k = 0; k < m; k++)
		{
			SIFT_INVARIANT(idx[k] < c->n, "row %u is past the end of a column chunk of %u rows", idx[k], c->n);
			dst[k] = all[idx[k]];
		}
		pfree(all);
		return;
	}
	for (uint32 k = 0; k < m;)
	{
		uint32		e = k + 1;

		while (e < m && idx[e] == idx[e - 1] + 1)
			e++;
		SIFT_INVARIANT(idx[e - 1] < c->n, "row %u is past the end of a column chunk of %u rows", idx[e - 1], c->n);
		if (e - k > 1)
			nd_range(c->root, idx[k], e - k, dst + k);
		else
			dst[k] = nd_get(c->root, idx[k]);
		k = e;
	}
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

	SIFT_BISECT(int64, m, a, b, alp_key(nd, m) < lo);
	*dlo = a;
	a = nd->dmin;
	b = nd->dmax + 1;
	SIFT_BISECT(int64, m, a, b, alp_key(nd, m) <= hi);
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
