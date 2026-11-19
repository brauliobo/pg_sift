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

/* The leaves the zones of a unit describe */
static inline int
zone_leaves(const OrderCtx *c)
{
	return Min(c->nleaf, 2);
}

/* The leaves a bound holds: those of the key up to SIFT_BOUND_COMPS, and the address when the key has no more */
static inline int
bound_leaves(const OrderCtx *c)
{
	return Min(c->nleaf, SIFT_BOUND_COMPS);
}

static BoundKey
lowest_bound(void)
{
	BoundKey	bk = {{PG_INT64_MIN, PG_INT64_MIN, PG_INT64_MIN}};

	return bk;
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
	if (bk->has_tid)
		SIFT_INVARIANT(sift_bound_push_tid(out, bk->tid), "the address of a bound fits a partition");
}

void
sift_colk_bound_unpack(const OrderCtx *ctx, const SiftBound *b, BoundKey *bk)
{
	*bk = lowest_bound();
	for (int t = 0; t < bound_leaves(ctx); t++)
	{
		SiftComp	kind = sift_bound_comp(b, t, &bk->k[t]);

		bk->nul[t] = kind == SIFT_COMP_NULL;
		if (kind == SIFT_COMP_NULL)
			bk->k[t] = 0;
	}
	bk->has_tid = sift_bound_tid(b, &bk->tid);
}

/* The bound of row r: the leaves of the order, then the address, which orders the rows of one key: that is the whole order only when the leaves fit the bound */
static void
row_bound(const OrderCtx *ctx, const SiftColBatch *b, uint32 r, SiftBound *out)
{
	sift_bound_clear(out);
	for (int t = 0; t < Min(ctx->nleaf, SIFT_BOUND_COMPS); t++)
	{
		int			leaf = ctx->leaf[t];
		bool		fits = row_null(b, r, leaf) ? sift_bound_push_null(out) : sift_bound_push_i64(out, sift_crun_key(&ctx->schema->cols[leaf], b->col[leaf].key[r]));

		SIFT_INVARIANT(fits, "a key of %d leaves fits a bound", ctx->nleaf);
	}
	if (ctx->nleaf <= SIFT_BOUND_COMPS)
	{
		bool		fits = sift_bound_push_tid(out, b->keys[r]);

		SIFT_INVARIANT(fits, "the address of a row fits a bound");
	}
}

/* Rows x and y of b in the order of a bound: key by key, NULL last, then by address when the bound holds it */
static int
row_order(const OrderCtx *ctx, const SiftColBatch *b, uint32 x, uint32 y)
{
	for (int t = 0; t < Min(ctx->nleaf, SIFT_BOUND_COMPS); t++)
	{
		int			leaf = ctx->leaf[t];
		bool		nx = row_null(b, x, leaf),
					ny = row_null(b, y, leaf);
		int64		kx,
					ky;

		if (nx || ny)
		{
			if (nx != ny)
				return nx ? 1 : -1;
			continue;
		}
		kx = sift_crun_key(&ctx->schema->cols[leaf], b->col[leaf].key[x]);
		ky = sift_crun_key(&ctx->schema->cols[leaf], b->col[leaf].key[y]);
		if (kx != ky)
			return kx < ky ? -1 : 1;
	}
	return ctx->nleaf > SIFT_BOUND_COMPS ? 0 : (b->keys[x] > b->keys[y]) - (b->keys[x] < b->keys[y]);
}

bool
sift_colk_batch_bound(const OrderCtx *ctx, const SiftColBatch *b, bool last, SiftBound *out)
{
	uint32		best = 0;

	if (b->nrows == 0 || ctx->nleaf == 0)
		return false;
	for (uint32 r = 1; r < b->nrows; r++)
	{
		int			c = row_order(ctx, b, r, best);

		if (last ? c >= 0 : c < 0)
			best = r;
	}
	row_bound(ctx, b, best, out);
	return true;
}

bool
sift_colk_segment_bound(SiftColSegment *seg, bool last, SiftBound *out)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col segment bound", ALLOCSET_SMALL_SIZES);
	OrderCtx	ctx = {seg->schema, seg->cut, seg->ncut};
	bool	   *want = MemoryContextAllocZero(cx, seg->schema->ncols * sizeof(bool));
	SiftColBatch *b = sift_cbatch_create(cx, seg->schema);
	bool		found = false;

	for (int t = 0; t < ctx.nleaf; t++)
		want[ctx.leaf[t]] = true;
	for (uint32 i = 0; i < seg->nsb && !found && ctx.nleaf > 0; i++)
	{
		sift_cseg_decode_unit_leaves(seg, last ? seg->nsb - 1 - i : i, want, b);
		/* the rows of a unit are in the order of their addresses, not of the key: the least (the greatest) of them is the bound */
		found = sift_colk_batch_bound(&ctx, b, last, out);
	}
	MemoryContextDelete(cx);
	return found;
}

/* Do the live rows of the segment lie from lo (NULL: no start) up to below hi (NULL: no end)? A segment with none lies anywhere. A key of more leaves than a bound holds has no address in it: a row may be at hi */
bool
sift_colk_rows_within(SiftColSegment *seg, const SiftBound *lo, const SiftBound *hi)
{
	SiftBound	first,
				last;
	int			c;

	if (!sift_colk_segment_bound(seg, false, &first) || !sift_colk_segment_bound(seg, true, &last))
		return true;
	c = hi == NULL ? -1 : sift_bound_cmp(&last, hi);
	return (lo == NULL || sift_bound_cmp(&first, lo) >= 0) && (seg->ncut <= SIFT_BOUND_COMPS ? c < 0 : c <= 0);
}

/* Row r of b against the bound: key by key, NULL last, then by address when the bound has one; equal on all it has, the row is at it */
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
	return !bk->has_tid || b->keys[r] == bk->tid ? 0 : b->keys[r] < bk->tid ? -1 : 1;
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

	for (int t = 0; t < SIFT_BOUND_COMPS; t++)
	{
		if (x->nul[t] != y->nul[t])
			return x->nul[t] ? 1 : -1;
		if (!x->nul[t] && x->k[t] != y->k[t])
			return x->k[t] < y->k[t] ? -1 : 1;
	}
	if (x->has_tid != y->has_tid)
		return x->has_tid ? 1 : -1;
	return !x->has_tid || x->tid == y->tid ? 0 : x->tid < y->tid ? -1 : 1;
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

			*b = lowest_bound();
			b->rows = Max(rows / SLICE_POINTS, 1);
			for (int t = 0; t < zone_leaves(ctx); t++)
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
				int			t = zone_leaves(ctx) > 1 && !nul1 && b->k[0] == hi[0] ? 1 : 0;
				int64		from = t == 1 ? lo1 : b->k[0];

				*p = lowest_bound();
				p->k[0] = b->k[0];
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

	*bk = lowest_bound();
	for (int t = 0; t < zone_leaves(ctx) && bounded; t++)
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

bool
sift_colk_cut_near(uint64 held, uint64 due_at, uint32 slack_pct, bool open)
{
	return held >= due_at && (!open || held - due_at >= due_at / 100 * slack_pct);
}
