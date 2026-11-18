/*
 * col/order_bound.c - the bounds of a range of the key of a merge: the slices of a merge, the window of a rewrite and the head of the next unit of an input, from the zones of
 * the first two leaves of the order (no row is decoded to take them)
 */
#include "postgres.h"

#include "col/order_bound.h"
#include "col/runs.h"

static inline bool
row_null(const SiftColBatch *b, uint32 r, int leaf)
{
	const SiftCVals *v = &b->col[leaf];

	return v->nulls != NULL && sift_col_bit_test(v->nulls, r);
}

static inline int
bound_leaves(const OrderCtx *c)
{
	return Min(c->nleaf, 2);
}

void
sift_colk_bound_pack(const OrderCtx *ctx, const BoundKey *bk, SiftBound *out)
{
	sift_bound_clear(out);
	for (int t = 0; t < bound_leaves(ctx); t++)
	{
		bool		fits = bk->nul[t] ? sift_bound_push_null(out) : sift_bound_push_i64(out, bk->k[t]);

		SIFT_INVARIANT(fits, "a bound of %d leaves fits a partition", bound_leaves(ctx));
	}
}

void
sift_colk_bound_unpack(const OrderCtx *ctx, const SiftBound *b, BoundKey *bk)
{
	memset(bk, 0, sizeof(BoundKey));
	for (int t = 0; t < bound_leaves(ctx); t++)
	{
		SiftComp	kind = sift_bound_comp(b, t, &bk->k[t]);

		bk->nul[t] = kind == SIFT_COMP_NULL;
		if (kind != SIFT_COMP_VALUE)
			bk->k[t] = kind == SIFT_COMP_NONE ? PG_INT64_MIN : 0;
	}
}

/* Row r of b against the bound: key by key, NULL last; equal on the leaves the bound has, the row is at it */
int
sift_colk_bound_cmp(const OrderCtx *ctx, const SiftColBatch *b, uint32 r, const BoundKey *bk)
{
	for (int t = 0; t < bound_leaves(ctx); t++)
	{
		int			leaf = ctx->leaf[t];
		bool		nr = row_null(b, r, leaf);
		int64		kr;

		if (nr || bk->nul[t])
		{
			if (nr != bk->nul[t])
				return nr ? 1 : -1;
			continue;
		}
		kr = sift_crun_key(&ctx->schema->cols[leaf], b->col[leaf].key[r]);
		if (kr != bk->k[t])
			return kr < bk->k[t] ? -1 : 1;
	}
	return 0;
}

/* Is every row of unit u of seg below lo? The leading leaf's zone says so when the unit has no NULL (they sort last) and its largest key is below: false is always safe */
static bool
unit_below(const OrderCtx *ctx, SiftColSegment *seg, uint32 u, const BoundKey *lo)
{
	SiftCZone	z;

	sift_cseg_zone(seg, ctx->leaf[0], u, &z);
	return z.nnull == 0 && (lo->nul[0] || z.max < lo->k[0]);
}

/* The first unit of seg that may hold a row at or above lo: the units follow each other in the order of the key, so the ones below are found by halving */
uint32
sift_colk_first_unit(const OrderCtx *ctx, SiftColSegment *seg, const BoundKey *lo)
{
	uint32		a = 0,
				b = seg->nsb;

	while (a < b)
	{
		uint32		mid = a + (b - a) / 2;

		if (unit_below(ctx, seg, mid, lo))
			a = mid + 1;
		else
			b = mid;
	}
	return a;
}

int
sift_colk_bound_order(const void *a, const void *b)
{
	const BoundKey *x = a,
			   *y = b;

	for (int t = 0; t < 2; t++)
	{
		if (x->nul[t] != y->nul[t])
			return x->nul[t] ? 1 : -1;
		if (!x->nul[t] && x->k[t] != y->k[t])
			return x->k[t] < y->k[t] ? -1 : 1;
	}
	return 0;
}

/*
 * The bounds of the slices of a merge: points that cut the rows of the inputs into slices of about the same count, from the zones of the first two leaves. A unit holds
 * thousands of rows between its smallest and largest key of the first leaf (a build leaves a few units in each segment, every segment over the whole range), so the
 * smallest keys alone crowd at a few values and most slices hold no row: SLICE_POINTS points are taken in each unit, the first its smallest keys and the others
 * evenly between the smallest and largest of the first leaf (the rows of a unit counted as even in that range), and sorted; the bound before slice j of nslices is
 * the one that j / nslices of the rows come before. The same for every slice that asks, whatever the keys each input holds (the participants of a build hold the
 * rows of the superblocks they claimed). Returns the sorted bounds, in *n.
 */
#define SLICE_POINTS 8

BoundKey *
sift_colk_slice_bounds(const OrderCtx *ctx, SiftColSegment *const *seg, int nseg, uint64 *n)
{
	uint64		units = 0;
	BoundKey   *bk;

	for (int i = 0; i < nseg; i++)
		units += seg[i]->nsb;
	bk = palloc(Max(units, 1) * SLICE_POINTS * sizeof(BoundKey));
	*n = 0;
	for (int i = 0; i < nseg; i++)
		for (uint32 u = 0; u < seg[i]->nsb; u++)
		{
			BoundKey   *b = &bk[(*n)++];
			int64		hi[2] = {0, 0},
						lo1;
			bool		nul1;
			uint32		rows = seg[i]->sbdir[u].nrows;

			memset(b, 0, sizeof(BoundKey));
			b->rows = Max(rows / SLICE_POINTS, 1);
			for (int t = 0; t < bound_leaves(ctx); t++)
			{
				SiftCZone	z;
				const SiftColDef *col = &seg[i]->schema->cols[ctx->leaf[t]];

				sift_cseg_zone(seg[i], ctx->leaf[t], u, &z);
				b->nul[t] = z.nnull >= z.nvals;
				b->k[t] = b->nul[t] ? 0 : sift_crun_key(col, z.min);
				hi[t] = b->nul[t] ? 0 : sift_crun_key(col, z.max);
			}
			lo1 = b->k[1];
			nul1 = b->nul[1];
			/* the unit's smallest second key bounds this unit, not the others that hold the same first key: the point stands before every row of that first key (depth 1) */
			b->k[1] = PG_INT64_MIN;
			b->nul[1] = false;
			if (b->nul[0])
			{
				b->rows = rows;
				continue;
			}
			/* a unit of one value of the first leaf is cut by the second */
			for (int q = 1; q < SLICE_POINTS; q++)
			{
				BoundKey   *p = &bk[(*n)++];
				int			t = bound_leaves(ctx) > 1 && !nul1 && b->k[0] == hi[0] ? 1 : 0;
				int64		from = t == 1 ? lo1 : b->k[0];

				memset(p, 0, sizeof(BoundKey));
				p->k[0] = b->k[0];
				p->k[1] = PG_INT64_MIN;
				p->k[t] = from + (int64) (((__int128) hi[t] - from) * q / SLICE_POINTS);
				p->rows = Max(rows / SLICE_POINTS, 1);
			}
		}
	qsort(bk, *n, sizeof(BoundKey), sift_colk_bound_order);
	return bk;
}

/* How a bound cuts the key: 1 before every row of a value of the first leaf, 2 inside the rows of one */
static uint32
bound_depth(const BoundKey *bk)
{
	return bk->k[1] == PG_INT64_MIN && !bk->nul[1] ? 1 : 2;
}

static uint64
distance(uint64 a, uint64 b)
{
	return a > b ? a - b : b - a;
}

/*
 * The bound before slice j of nslices: the point that j / nslices of the rows come before. Within slack_pct of a slice's rows either side of it, the nearest point that cuts between values of the
 * first leaf (depth 1) is taken instead, so that a value that fits a slice is in one segment; when there is none, the value is bigger than the slack allows for and the exact point is taken, inside it
 * (depth 2). *depth, when asked for, says which.
 */
const BoundKey *
sift_colk_slice_bound(const BoundKey *bk, uint64 n, int j, int nslices, uint32 slack_pct, uint32 *depth)
{
	uint64		total = 0,
				want,
				slack,
				exact = n - 1,
				best = n,
				best_gap = 0;

	for (uint64 i = 0; i < n; i++)
		total += bk[i].rows;
	want = total * j / nslices;
	slack = total / nslices * slack_pct / 100;
	for (uint64 i = 0, before = 0; i < n; before += bk[i++].rows)
		if (before >= want)
		{
			exact = i;
			break;
		}
	for (uint64 i = 0, seen = 0; i < n; seen += bk[i++].rows)
		if (distance(seen, want) <= slack && bound_depth(&bk[i]) == 1 && (best == n || distance(seen, want) < best_gap))
		{
			best = i;
			best_gap = distance(seen, want);
		}
	if (depth != NULL)
		*depth = best == n ? bound_depth(&bk[exact]) : 1;
	return &bk[best == n ? exact : best];
}

/*
 * The rows of a unit are not below its zones' minima on the leaves of the key, in the order of the key: the first of the two bound leaves (a leaf the zone does not
 * describe, a string, an overflowed or NaN value, bounds nothing) and the second after it. A leaf that is not bounded is the lowest key, so the unit is reached at once.
 */
void
sift_colk_unit_head(const OrderCtx *ctx, SiftColSegment *seg, uint32 u, BoundKey *bk)
{
	bool		bounded = true;

	*bk = (BoundKey) {{PG_INT64_MIN, PG_INT64_MIN}};
	for (int t = 0; t < bound_leaves(ctx) && bounded; t++)
	{
		const SiftColDef *col = &seg->schema->cols[ctx->leaf[t]];
		SiftCZone	z;

		sift_cseg_zone(seg, ctx->leaf[t], u, &z);
		bounded = col->store != SIFT_COLS_STR && !(z.flags & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE | SIFT_COLZ_NAN));
		if (bounded)
		{
			bk->nul[t] = z.nnull >= z.nvals;
			bk->k[t] = bk->nul[t] ? 0 : sift_crun_key(col, z.min);
		}
	}
}
