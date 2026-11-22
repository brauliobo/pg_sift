/*
 * col/f2/f2_pred.c - what a predicate says of one stored value, the same for the scan of the rows (f2_scan.c), the filters on the compressed chunks (col/granule_filter.c) and the skipping
 * indexes (col/skip/probe.c): the order of the floats and of the bytes, a comparison operator, a test of a string.
 */
#include "postgres.h"

#include "utils/lsyscache.h"

#include "col/f2/f2_pred.h"
#include "col/f2/f2_pred_ext.h"
#include "col/like_simd.h"
#include "core/compat.h"

int
sift_f2_f8_cmp(double a, double b)
{
	if (isnan(a))
		return isnan(b) ? 0 : 1;
	return isnan(b) ? -1 : (a < b ? -1 : a > b);
}

int
sift_f2_str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c != 0 ? c : (alen < blen ? -1 : alen > blen);
}

bool
sift_f2_cmp_op(SiftColOp op, int c)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return c == 0;
		case SIFT_COLOP_NE:
			return c != 0;
		case SIFT_COLOP_LT:
			return c < 0;
		case SIFT_COLOP_LE:
			return c <= 0;
		case SIFT_COLOP_GT:
			return c > 0;
		default:
			return c >= 0;
	}
}

static bool
str_contains(const char *s, uint32 len, const char *needle, uint32 nlen)
{
	return nlen == 0 || (len >= nlen && memmem(s, len, needle, nlen) != NULL);
}

/* ILIKE '%abc%' under the C collation, whose lower() folds the ASCII capitals and no other character: the needle is lower case already, so the string is folded as it is searched */
static bool
str_icontains(const char *s, uint32 len, const char *needle, uint32 nlen)
{
	SiftLikeNeedle nd;

	sift_like_needle_borrow(&nd, (const uint8 *) needle, nlen, true);
	return sift_like_test(&nd, (const uint8 *) s, len) == SIFT_LIKE_YES;
}

bool
sift_f2_str_pred(const SiftColPred *p, const char *s, uint32 len)
{
	const SiftColValue *v = &p->vals[0];

	switch (p->op)
	{
		case SIFT_COLOP_PREFIX:
			return len >= v->len && memcmp(s, v->v.s, v->len) == 0;
		case SIFT_COLOP_SUFFIX:
			return len >= v->len && memcmp(s + len - v->len, v->v.s, v->len) == 0;
		case SIFT_COLOP_CONTAINS:
			return str_contains(s, len, v->v.s, v->len);
		case SIFT_COLOP_ICONTAINS:
			return str_icontains(s, len, v->v.s, v->len);
		case SIFT_COLOP_NOTCONTAINS:
			return !str_contains(s, len, v->v.s, v->len);
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			{
				bool		in = false;

				for (int j = 0; j < p->nvals && !in; j++)
					in = sift_f2_str_cmp(s, len, p->vals[j].v.s, p->vals[j].len) == 0;
				return in == (p->op == SIFT_COLOP_IN);
			}
		default:
			return sift_f2_cmp_op(p->op, sift_f2_str_cmp(s, len, v->v.s, v->len));
	}
}

/* The bytes of a string decide a test of the collation only where equal strings are equal bytes (a deterministic collation) and, for a range, where the bytes are the order (the C collation) */
bool
sift_f2_collation_undecided(const SiftColPred *p)
{
	if (p->collid == InvalidOid)
		return false;
	return !get_collation_isdeterministic(p->collid) || (sift_col_pred_is_bound(p) && !sift_collate_is_c(p->collid));
}

uint8
sift_f2_leaf_type(const SiftColDef *d)
{
	if (d->store == SIFT_COLS_STR)
		return SIFT_COLTAG_STRING;
	return d->type == SIFT_COLT_BOOL ? SIFT_COLTAG_BOOLEAN : SIFT_COLTAG_NUMBER;
}

uint8
sift_f2_json_type(const SiftColDef *d, bool isnull, uint8 tag)
{
	if (!isnull)
		return tag != 0 ? tag : sift_f2_leaf_type(d);
	return (tag >= SIFT_COLTAG_STRING && tag <= SIFT_COLTAG_ARRAY) || tag == SIFT_COLTAG_JNULL || tag == SIFT_COLTAG_UNSURE ? tag : 0;
}

SiftLikeVerdict
sift_f2_str_verdict(const SiftColPred *p, const char *s, uint32 len)
{
	switch (p->op)
	{
		case SIFT_COLOP_IPREFIX:
		case SIFT_COLOP_IEQ:
			return sift_f2_iprefix_test(s, len, (const uint8 *) p->vals[0].v.s, p->vals[0].len, p->op == SIFT_COLOP_IEQ, p->vals[1].v.i != 0);
		case SIFT_COLOP_ARR_OVERLAP:
		case SIFT_COLOP_ARR_CONTAINS:
			return sift_f2_arr_match((SiftArrQuery *) p->vals[0].v.s, (const uint8 *) s, len, p->op == SIFT_COLOP_ARR_CONTAINS);
		default:
			return sift_f2_str_pred(p, s, len) ? SIFT_LIKE_YES : SIFT_LIKE_NO;
	}
}

/* The operators of the hybrid scan the stored bytes decide: an array operator on the head of an array that keeps its image, a case folded prefix or equality under the C collation (whose lower() folds the ASCII capitals and no other character) */
static bool
ext_decided(const SiftColDef *d, const SiftColPred *p)
{
	if (p->op == SIFT_COLOP_ARR_OVERLAP || p->op == SIFT_COLOP_ARR_CONTAINS)
		return sift_col_def_arrlen_head(d) && !(d->flags & SIFT_COLD_NOIMAGE);
	return d->store == SIFT_COLS_STR && !sift_col_def_is_wide(d) && d->lane == 0 && p->collid != InvalidOid && sift_collate_is_c(p->collid);
}

/* A value the leaf holds in a form the test cannot judge (a wide number, the image of an array or a range, a pattern that folds case under a collation other than C, a string the bytes of a collation do not order): the heap decides */
bool
sift_f2_undecided(const SiftColDef *d, const SiftColPred *p)
{
	if (SIFT_COLOP_IS_EXT(p->op))
		return !ext_decided(d, p);
	return sift_col_def_is_wide(d) || (d->lane > 0 && !sift_f2_card_lane(d)) || (d->flags & SIFT_COLD_LOSSY) || d->type == SIFT_COLT_VARLENA || d->type == SIFT_COLT_TIMETZ || p->op == SIFT_COLOP_SHAPE ||
		(p->op == SIFT_COLOP_ICONTAINS && (p->collid == InvalidOid || !sift_collate_is_c(p->collid))) || (d->store == SIFT_COLS_STR && sift_f2_collation_undecided(p));
}

