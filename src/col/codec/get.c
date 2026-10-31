/*
 * col/codec/get.c - one value of a chunk by its position, the selected positions, the order
 * the bit-packed group readers and the delta chain
 * portable C
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/codec/decode_int.h"

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

int64
cc_alp_key(const CcNode *nd, int64 d)
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
				return cc_alp_key(nd, nd_get(nd->a, i));
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
		case SIFT_CCN_DELTA:
			/* a block of 1024 rows from its checkpoint: the rows of the range, not the prefix sums from the first row of the chunk */
			for (uint32 i = 0; i < len;)
			{
				uint32		b = (s + i) / 1024,
							lo = (s + i) % 1024,
							upto = Min(Min(1024, nd->n - b * 1024), lo + len - i);
				uint32		tmp[1024 + SIFT_COL_PAD / 4];
				int64		run[1024 + SIFT_COL_PAD / 8];

				sift_cck->unpack32((const uint32 *) nd->words + (size_t) b * 32 * nd->W, nd->W, rows_for(upto), 0, tmp);
				sift_cck->prefix64(tmp, upto, run, b ? (int64) get64(nd->chk + 8 * (size_t) (b - 1)) : nd->base, nd->mind);
				memcpy(dst + i, run + lo, (upto - lo) * sizeof(int64));
				i += upto - lo;
			}
			return;
		case SIFT_CCN_RLE:
			{
				uint32		lo = 0,
							hi = nd->nruns,
							r0,
							r1;
				int64	   *rv;

				while (hi - lo > 1)
				{
					uint32		m = (lo + hi) / 2;

					if (nd->rstart[m] <= s)
						lo = m;
					else
						hi = m;
				}
				r0 = lo;
				for (r1 = r0; r1 + 1 < nd->nruns && nd->rstart[r1 + 1] < s + len; r1++)
					;
				rv = palloc(sizeof(int64) * (r1 - r0 + 1 + SIFT_COL_PAD / 8));
				nd_range(nd->a, r0, r1 - r0 + 1, rv);
				for (uint32 i = 0, r = r0; i < len; i++)
				{
					while (nd->rstart[r + 1] <= s + i)
						r++;
					dst[i] = rv[r - r0];
				}
				pfree(rv);
				return;
			}
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

/* Does nd_range cost by the rows it asks for: the nodes it decodes in blocks or runs, not row by row from the start of the chunk */
static bool
nd_range_cheap(const CcNode *nd)
{
	switch (nd->tag)
	{
		case SIFT_CCN_FORBIT:
		case SIFT_CCN_DELTA:
			return true;
		case SIFT_CCN_WRAP:
		case SIFT_CCN_SCALE:
		case SIFT_CCN_RLE:
			return nd_range_cheap(nd->a);
		case SIFT_CCN_DICT:
			return nd_range_cheap(nd->b);
		default:
			return false;
	}
}

#define GATHER_RUN	16

/* The values of the rows idx[0..m) of a node with no run of GATHER_RUN rows: a level at a time, so that the switch of nd_get and its recursion are paid once for the rows, not for each */
static void
nd_gather(const CcNode *nd, const uint32 *idx, uint32 m, int64 *dst)
{
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 k = 0; k < m; k++)
				dst[k] = nd->base;
			return;
		case SIFT_CCN_FORBIT:
			{
				const uint32 *w32 = (const uint32 *) nd->words;
				uint32		W = nd->W,
							mask = W >= 32 ? 0xFFFFFFFFu : (1u << W) - 1;

				for (uint32 k = 0; k < m; k++)
				{
					uint32		i = idx[k],
								bp = ((i & 1023) >> 5) * W,
								sh = bp & 31,
								x;
					const uint32 *a = w32 + (size_t) (i >> 10) * 32 * W + (size_t) (bp >> 5) * 32 + (i & 31);

					x = sift_cc_ld32(a) >> sh;
					if (sh + W > 32)
						x |= sift_cc_ld32(a + 32) << (32 - sh);
					dst[k] = (int64) ((uint64) nd->base + (x & mask));
				}
			}
			return;
		case SIFT_CCN_WRAP:
			nd_gather(nd->a, idx, m, dst);
			return;
		case SIFT_CCN_SCALE:
			nd_gather(nd->a, idx, m, dst);
			for (uint32 k = 0; k < m; k++)
				dst[k] = (int64) ((uint64) nd->base + (uint64) nd->mind * (uint64) dst[k]);
			return;
		case SIFT_CCN_DICT:
			nd_gather(nd->b, idx, m, dst);
			for (uint32 k = 0; k < m; k++)
			{
				if (dst[k] < 0 || dst[k] >= (int64) nd->d)
					CC_CORRUPT("dictionary code out of range");
				dst[k] = nd->dict[dst[k]];
			}
			return;
		default:
			for (uint32 k = 0; k < m; k++)
				dst[k] = nd_get(nd, idx[k]);
	}
}

void
sift_cc_decode_sel(const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst)
{
	if (m == 0)
		return;
	/* one run of consecutive rows costs by the rows it asks for, however many: no decode of the whole chunk is cheaper */
	if (m > 1 && idx[m - 1] - idx[0] == m - 1 && nd_range_cheap(c->root))
	{
		uint32		k = 1;

		while (k < m && idx[k] == idx[0] + k)
			k++;
		if (k == m)
		{
			SIFT_INVARIANT(idx[m - 1] < c->n, "row %u is past the end of a column chunk of %u rows", idx[m - 1], c->n);
			nd_range(c->root, idx[0], m, dst);
			return;
		}
	}
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
	{
		bool		runs = false;

		/* a run of rows costs by the rows it asks for in nd_range; the short ones that chance makes in a sparse selection do not */
		for (uint32 k = 0, len = 1; k + 1 < m && !runs; k++)
		{
			len = idx[k + 1] == idx[k] + 1 ? len + 1 : 1;
			runs = len >= GATHER_RUN;
		}
		if (!runs)
		{
			for (uint32 k = 0; k < m; k++)
				SIFT_INVARIANT(idx[k] < c->n, "row %u is past the end of a column chunk of %u rows", idx[k], c->n);
			nd_gather(c->root, idx, m, dst);
			return;
		}
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

/* The order of the values of a node in row order (sift_cc_order); the nodes that map the values of a child one to one and in order, or its runs, keep it */
static int
nd_order(const CcNode *nd)
{
	int			o;

	switch (nd->tag)
	{
		case SIFT_CCN_FORBIT:
			return 2;
		case SIFT_CCN_DELTA:
			if (nd->mind >= 0)
				return 1;
			/* the packed residuals are at most 2^W - 1 above the smallest step */
			return nd->W < 32 && nd->mind + (int64) ((UINT64CONST(1) << nd->W) - 1) <= 0 ? -1 : 2;
		case SIFT_CCN_WRAP:
		case SIFT_CCN_RLE:
			return nd_order(nd->a);
		case SIFT_CCN_DICT:
			/* a sorted dictionary: the codes order as the values do */
			return nd_order(nd->b);
		case SIFT_CCN_SCALE:
			o = nd_order(nd->a);
			return nd->mind > 0 || o == 2 || o == 0 ? o : -o;
		default:
			return 0;
	}
}

int
sift_cc_order(const SiftCc *c)
{
	int			o = nd_order(c->root);

	/* the steps wrap in 64 bits: they say the order only when the keys span less than the signed range */
	return (o == 1 || o == -1) && (uint64) c->max - (uint64) c->min > (uint64) PG_INT64_MAX ? 2 : o;
}
