/*
 * col/f2/f2_zone_verdict.c - what the zone of a granule says of a predicate before a chunk of the granule is read.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/f2_pred.h"

/* The text of a column held as a type (SIFT_ZONE_PROMO) is a literal of the type's spelling only when its value is in the zone: no other row can equal it, and a granule with a row that is not of the type is not ruled out */
static ZoneVerdict
promo_verdict(const SiftZone *z, const SiftColPred *p)
{
	if (z->flags & SIFT_ZONE_PROMO_EXC)
		return Z_MAYBE;
	for (int j = 0; j < p->nvals; j++)
	{
		int64		v;

		if (!p->vals[j].isnull && sift_promo_literal(z->flags, p->vals[j].v.s, p->vals[j].len, &v) && !(z->flags & SIFT_ZONE_NONE) && v >= z->min && v <= z->max)
			return Z_MAYBE;
	}
	return Z_NONE;
}

/*
 * What the zone of a granule of nrows rows says of a predicate. A leaf that holds only nulls passes IS NULL on every row and IS NOT NULL on none, one that holds none passes the reverse, and a key
 * leaf that has the key on no row or every row passes jsonb_exists on none or all: no chunk is read. The least and the greatest of an integer column rule out a granule only while no row of it is
 * one the column could not hold.
 */
ZoneVerdict
sift_f2_zone_verdict(const SiftColDef *d, const SiftColPred *p, const SiftZone *z, uint32 nrows, bool exceptions)
{
	bool		any = false;

	if (p->op == SIFT_COLOP_KEY_EXISTS)
		return (z->flags & SIFT_ZONE_KEY_ALL_ABSENT) ? Z_NONE : (z->flags & SIFT_ZONE_KEY_ALL_PRESENT) ? Z_ALL : Z_MAYBE;
	if (p->op == SIFT_COLOP_JSON_TYPE)
	{
		bool		own = (p->vals[0].v.i >> sift_f2_leaf_type(d)) & 1;

		if (exceptions)
			return Z_MAYBE;
		return z->nnull == nrows || !own ? Z_NONE : z->nnull == 0 ? Z_ALL : Z_MAYBE;
	}
	if ((p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL) && d->parent < 0 && d->lane == 0 && !sift_col_def_is_wide(d) && !exceptions)
	{
		bool		nulls = p->op == SIFT_COLOP_ISNULL;

		if (z->nnull == nrows)
			return nulls ? Z_ALL : Z_NONE;
		return z->nnull == 0 ? (nulls ? Z_NONE : Z_ALL) : Z_MAYBE;
	}
	if ((z->flags & SIFT_ZONE_PROMO) && d->store == SIFT_COLS_STR && d->parent < 0 && d->lane == 0 && (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN))
		return promo_verdict(z, p);
	if (d->store != SIFT_COLS_INT || sift_col_def_is_wide(d) || (d->lane > 0 && !sift_f2_card_lane(d)) || exceptions || p->op == SIFT_COLOP_SHAPE)
		return Z_MAYBE;
	if (z->flags & SIFT_ZONE_NONE)
		return p->op != SIFT_COLOP_ISNULL ? Z_NONE : Z_MAYBE;
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return p->vals[0].v.i < z->min || p->vals[0].v.i > z->max ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_NE:
			return z->min == z->max && z->min == p->vals[0].v.i ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_LT:
			return z->min >= p->vals[0].v.i ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_LE:
			return z->min > p->vals[0].v.i ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_GT:
			return z->max <= p->vals[0].v.i ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_GE:
			return z->max < p->vals[0].v.i ? Z_NONE : Z_MAYBE;
		case SIFT_COLOP_IN:
			for (int j = 0; j < p->nvals && !any; j++)
				any = p->vals[j].v.i >= z->min && p->vals[j].v.i <= z->max;
			return any ? Z_MAYBE : Z_NONE;
		default:
			return Z_MAYBE;
	}
}
