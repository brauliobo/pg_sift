/*
 * col/order_bound.h - the bounds of a range of the key of a merge, from the zones of the units (col/order_bound.c)
 */
#ifndef SIFT_COL_ORDER_BOUND_H
#define SIFT_COL_ORDER_BOUND_H

#include "col/enc.h"
#include "core/range.h"
#include "col/seg.h"

typedef struct OrderCtx
{
	const SiftColSchema *schema;
	const int  *leaf;
	int			nleaf;
} OrderCtx;

/* How far from the exact point of a slice, in percent of the rows of a slice, a cut moves to fall between two values of the first key leaf */
#define SIFT_SLICE_SLACK_PCT 12

/* A bound of a slice of the key range, or of the rows of a unit: the first two leaves of the order, from the zone of a unit (no row is decoded to take it) */
typedef struct BoundKey
{
	int64		k[2];
	bool		nul[2];
	uint32		rows;			/* the rows of the unit the point stands for, a share of the points of the unit */
} BoundKey;

/* A bound as the lower bound of a partition of the directory (core/range.h), and back: the leaves it has; a bound that stops short reads as the lowest key of the leaves it lacks */
extern void sift_colk_bound_pack(const OrderCtx *ctx, const BoundKey *bk, SiftBound *out);
extern void sift_colk_bound_unpack(const OrderCtx *ctx, const SiftBound *b, BoundKey *bk);
/* The bound of the least live row of a segment cut by a key, or of its greatest (the leaves of its cut and, up to SIFT_BOUND_COMPS of them, the address): false when it has none or records no cut */
extern bool sift_colk_segment_bound(SiftColSegment *seg, bool last, SiftBound *out);
/* Do the live rows of the segment lie from lo (NULL: no start) up to below hi (NULL: no end)? */
extern bool sift_colk_rows_within(SiftColSegment *seg, const SiftBound *lo, const SiftBound *hi);
/* Row r of b against the bound: key by key, NULL last; equal on the leaves the bound has, the row is at it */
extern int	sift_colk_bound_cmp(const OrderCtx *ctx, const SiftColBatch *b, uint32 r, const BoundKey *bk);
/* The order of two bounds (qsort); a bound on fewer leaves holds the lowest key of the others */
extern int	sift_colk_bound_order(const void *a, const void *b);
/* The first unit of seg that may hold a row at or above lo */
extern uint32 sift_colk_first_unit(const OrderCtx *ctx, SiftColSegment *seg, const BoundKey *lo);
/* A bound the rows of unit u of seg do not go below, from its zones */
extern void sift_colk_unit_head(const OrderCtx *ctx, SiftColSegment *seg, uint32 u, BoundKey *bk);
/* The sorted points that cut the rows of nseg segments into slices, in *n, and the bound before slice j of nslices (the point at j / nslices of the rows) */
extern BoundKey *sift_colk_slice_bounds(const OrderCtx *ctx, SiftColSegment *const *seg, int nseg, uint64 *n);
/* ... aligned to the values of the first leaf within slack_pct of a slice either side; *depth: 1 when it is, 2 when the value is bigger than that */
extern const BoundKey *sift_colk_slice_bound(const BoundKey *bk, uint64 n, int j, int nslices, uint32 slack_pct, uint32 *depth);
/*
 * Is a segment of held bytes, written in the order of the key, done after a unit? It is due at due_at bytes; a cut there that would split a value of the first key leaf (open) waits for
 * the value to end, up to slack_pct of due_at more bytes: a value that fits is in one segment, and a bigger one is cut inside, at the first unit past the slack. So a segment holds at most
 * due_at * (100 + slack_pct) / 100 bytes and the unit that took it past that.
 */
extern bool sift_colk_cut_near(uint64 held, uint64 due_at, uint32 slack_pct, bool open);

#endif							/* SIFT_COL_ORDER_BOUND_H */
