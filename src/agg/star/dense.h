/*
 * agg/star/dense.h - the dense form of the table of a dimension (agg/star/star.h): a dimension of one integer key whose rows have different keys keeps no key column and no index, only the payload
 * of the row of key k in slot k - dbase of arrays a little wider than the span of its keys, a bit for each slot that holds a row, and the payload columns in the bytes of their type. A probe is
 * one subtraction. The table leaves the form for the hash form (the rows it holds added again) at a second row of a key, a NULL key, or when the slots its keys span would pass its budget.
 */
#ifndef SIFT_AGG_V6_STAR_DENSE_H
#define SIFT_AGG_V6_STAR_DENSE_H

#include "agg/star/star.h"

/* the dimensions that start in the dense form: an equi join on one integer key whose rows no fact row needs to find again (not a RIGHT or FULL join) */
static inline bool
sift_star_dense_shape(const SiftStarShape *sh)
{
	return sh->mode != SV6J_RANGE_SMALL && !sift_star_preserves_dim(sh->mode) && sh->nkeys == 1 && sh->kcls[0] == SSC_INT;
}

static inline uint32
sift_star_pay_width(const SiftStarShape *sh, int pay)
{
	return sh->pcls[pay] == SSC_INT && sh->pwidth[pay] != 0 ? sh->pwidth[pay] : 8;
}

static inline bool
sift_star_dense_has(const SiftStarDim *d, uint64 s)
{
	return (d->dhas[s >> 6] >> (s & 63)) & 1;
}

/* the row (1-based) of key k, 0 when the table holds none */
static inline uint32
sift_star_dense_row(const SiftStarDim *d, int64 k)
{
	const uint64 s = (uint64) k - (uint64) d->dbase;

	return s < d->dcap && sift_star_dense_has(d, s) ? (uint32) s + 1 : 0;
}

/* the value of payload column pay of row i (0-based) as the hash form holds it: an int64, the bits of a float8 or the offset of a string in the arena */
static inline uint64
sift_star_dense_bits(const SiftStarDim *d, int pay, uint32 i)
{
	switch (sift_star_pay_width(&d->shape, pay))
	{
		case 1:
			return (uint64) (int64) ((const int8 *) d->dpay[pay])[i];
		case 2:
			return (uint64) (int64) ((const int16 *) d->dpay[pay])[i];
		case 4:
			return (uint64) (int64) ((const int32 *) d->dpay[pay])[i];
		default:
			return ((const uint64 *) d->dpay[pay])[i];
	}
}

static inline bool
sift_star_pay_null(const SiftStarDim *d, int pay, uint32 i)
{
	return d->dense ? (d->dnull[pay][i >> 6] >> (i & 63)) & 1 : d->null[d->shape.nkeys + pay][i];
}

static inline uint64
sift_star_pay_bits(const SiftStarDim *d, int pay, uint32 i)
{
	return d->dense ? sift_star_dense_bits(d, pay, i) : d->col[d->shape.nkeys + pay][i];
}

static inline uint32
sift_star_pay_len(const SiftStarDim *d, int pay, uint32 i)
{
	return d->dense ? d->dlen[pay][i] : d->len[d->shape.nkeys + pay][i];
}

/* the bytes the dense form takes in cap slots and an arena of arena_cap bytes */
extern uint64 sift_star_dense_bytes(const SiftStarShape *sh, uint64 cap, uint64 arena_cap);
/* room for the row in its slot (the slots widened for its key); false when the table must leave the dense form for it */
extern bool sift_star_dense_reserve(SiftStarDim *d, const SiftStarRow *row);
/* the row into the slot sift_star_dense_reserve made room in; the strings of its payload are in the arena already, pay_i their offsets */
extern void sift_star_dense_put(SiftStarDim *d, const SiftStarRow *row);
/* the row of slot s as sift_star_dim_add takes it: its key and payload (strings in the arena) */
extern void sift_star_dense_take(const SiftStarDim *d, uint32 s, SiftStarRow *row);
extern void sift_star_dense_finish(SiftStarDim *d);
extern void sift_star_dense_free(SiftStarDim *d);
/* the bytes of the arena of the table once it holds len more bytes */
extern uint64 sift_star_arena_grown(const SiftStarDim *d, uint64 len);

#endif							/* SIFT_AGG_V6_STAR_DENSE_H */
