/*
 * col/f2/zone_verdict.c - what the zone of a granule says of a predicate before a chunk of the granule is read.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/pred/pred.h"

/* The text of a column held as a type (SIFT_ZONE_PROMO) is a literal of the type's spelling only when its value is in the zone: no other row can equal it, and a granule with a row that is not of the type is not ruled out (under a collation that equates other spellings, no literal is ruled out by its spelling) */
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

/* Whether every value of a granule satisfies the comparison (all), or none does (none), over the values of the zone that are not NaN, and whether a NaN satisfies it (nan) */
typedef struct FloatSat
{
	bool		none;
	bool		all;
	bool		nan;
} FloatSat;

/* The comparison of the stored values with a constant c, in the order of PostgreSQL: -Inf < finite < +Inf < NaN, and a NaN equals a NaN */
static FloatSat
float_sat(SiftColOp op, double c, double lo, double hi)
{
	const bool	cnan = isnan(c);

	switch (op)
	{
		case SIFT_COLOP_EQ:
			return (FloatSat) {.none = cnan || c < lo || c > hi,.all = !cnan && lo == c && hi == c,.nan = cnan};
		case SIFT_COLOP_NE:
			return (FloatSat) {.none = !cnan && lo == c && hi == c,.all = cnan || c < lo || c > hi,.nan = !cnan};
		case SIFT_COLOP_LT:
			return (FloatSat) {.none = !cnan && lo >= c,.all = cnan || hi < c,.nan = false};
		case SIFT_COLOP_LE:
			return (FloatSat) {.none = !cnan && lo > c,.all = cnan || hi <= c,.nan = cnan};
		case SIFT_COLOP_GT:
			return (FloatSat) {.none = cnan || hi <= c,.all = !cnan && lo > c,.nan = !cnan};
		default:
			return (FloatSat) {.none = cnan || hi < c,.all = !cnan && lo >= c,.nan = true};
	}
}

/* What the zone of a float column says of a comparison: a zone of a part written before the zones of floats has no SIFT_ZONE_FLOAT and says nothing */
static ZoneVerdict
float_verdict(const SiftColDef *d, const SiftColPred *p, const SiftZone *z, uint32 nrows)
{
	const bool	has_nan = z->flags & SIFT_ZONE_HAS_NAN,
				has_range = !(z->flags & SIFT_ZONE_NONE),
				scalar = d->parent < 0;
	bool		none = true,
				all = false;

	if (!(z->flags & SIFT_ZONE_FLOAT))
		return scalar && nrows > 0 && z->nnull >= nrows ? Z_NONE : Z_MAYBE;
	if (p->op == SIFT_COLOP_IN)
	{
		for (int j = 0; j < p->nvals; j++)
		{
			FloatSat	s = float_sat(SIFT_COLOP_EQ, p->vals[j].v.f, has_range ? z->fmin : 0, has_range ? z->fmax : 0);

			if (p->vals[j].isnull)
				continue;
			none &= (!has_range || s.none) && (!has_nan || !s.nan);
			all |= (!has_range || s.all) && (!has_nan || s.nan) && (has_range || has_nan);
		}
	}
	else if (p->op >= SIFT_COLOP_EQ && p->op <= SIFT_COLOP_GE && !p->vals[0].isnull)
	{
		FloatSat	s = float_sat(p->op, p->vals[0].v.f, has_range ? z->fmin : 0, has_range ? z->fmax : 0);

		none = (!has_range || s.none) && (!has_nan || !s.nan);
		all = (!has_range || s.all) && (!has_nan || s.nan);
	}
	else
		return Z_MAYBE;
	return none ? Z_NONE : all && scalar && z->nnull == 0 ? Z_ALL : Z_MAYBE;
}

/* What the zone of an integer column says of a comparison: ALL where no row is null and every one of them passes */
static ZoneVerdict
int_verdict(const SiftColDef *d, const SiftColPred *p, const SiftZone *z)
{
	const bool	every = d->parent < 0 && z->nnull == 0;
	const int64 c = p->vals[0].v.i;
	bool		any = false,
				in_all = false;

	if (z->flags & SIFT_ZONE_NONE)
		return p->op != SIFT_COLOP_ISNULL ? Z_NONE : Z_MAYBE;
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return c < z->min || c > z->max ? Z_NONE : every && z->min == z->max ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_NE:
			return z->min == z->max && z->min == c ? Z_NONE : every && (c < z->min || c > z->max) ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_LT:
			return z->min >= c ? Z_NONE : every && z->max < c ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_LE:
			return z->min > c ? Z_NONE : every && z->max <= c ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_GT:
			return z->max <= c ? Z_NONE : every && z->min > c ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_GE:
			return z->max < c ? Z_NONE : every && z->min >= c ? Z_ALL : Z_MAYBE;
		case SIFT_COLOP_IN:
			for (int j = 0; j < p->nvals && !any; j++)
			{
				any = p->vals[j].v.i >= z->min && p->vals[j].v.i <= z->max;
				in_all = any && !p->vals[j].isnull && z->min == z->max;
			}
			return !any ? Z_NONE : every && in_all ? Z_ALL : Z_MAYBE;
		default:
			return Z_MAYBE;
	}
}

/*
 * What the zone of a granule of nrows rows says of a predicate. A leaf that holds only nulls passes IS NULL on every row and IS NOT NULL on none, one that holds none passes the reverse, and a key
 * leaf that has the key on no row or every row passes jsonb_exists on none or all, and a promoted one rules out its granule: no chunk is read, unless a column delta moved a row of the granule. The least and the greatest of an integer or float column rule out a granule only while no row
 * of it is one the column could not hold, and say that every row of it passes a comparison where no row is null.
 */
ZoneVerdict
sift_f2_zone_verdict(const SiftColDef *d, const SiftColPred *p, const SiftZone *z, uint32 nrows, bool exceptions)
{
	if (p->op == SIFT_COLOP_KEY_EXISTS)
		return exceptions ? Z_MAYBE : (z->flags & SIFT_ZONE_KEY_ALL_ABSENT) ? Z_NONE : (z->flags & SIFT_ZONE_KEY_ALL_PRESENT) ? Z_ALL : Z_MAYBE;
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
	if ((z->flags & SIFT_ZONE_PROMO) && !exceptions && d->store == SIFT_COLS_STR && d->parent < 0 && d->lane == 0 && (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN) && !sift_f2_collation_undecided(p))
		return promo_verdict(z, p);
	if (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8)
		return exceptions || p->op == SIFT_COLOP_SHAPE ? Z_MAYBE : float_verdict(d, p, z, nrows);
	if (d->store != SIFT_COLS_INT || sift_col_def_is_wide(d) || (d->lane > 0 && !sift_f2_card_lane(d)) || exceptions || p->op == SIFT_COLOP_SHAPE)
		return Z_MAYBE;
	return int_verdict(d, p, z);
}
