/*
 * col/f2/pred/pred.c - what a predicate says of one stored value, the same for the scan of the rows (f2_scan.c), the filters on the compressed chunks (col/granule_filter.c) and the skipping
 * indexes (col/skip/probe.c): the order of the floats and of the bytes, a comparison operator, a test of a string.
 */
#include "postgres.h"

#include "utils/lsyscache.h"

#include "col/f2/pred/pred.h"
#include "col/f2/pred/ext.h"
#include "col/like/simd.h"
#include "core/compat.h"
#include "core/hash.h"
#include "port/pg_bitutils.h"

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

static uint64
str_hash(const char *s, uint32 len)
{
	uint64		h = UINT64CONST(0xcbf29ce484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ (uint8) s[i]) * UINT64CONST(0x100000001b3);
	return sift_fmix64(h);
}

static uint64
f8_hash(double v)
{
	uint64		bits;

	v = v == 0 ? 0.0 : v;
	memcpy(&bits, &v, sizeof(bits));
	return sift_fmix64(bits);
}

SiftPredSet *
sift_f2_pred_sets_new(MemoryContext cx, int n)
{
	SiftPredSet *ps = MemoryContextAllocZero(cx, Max(n, 1) * sizeof(SiftPredSet));

	for (int k = 0; k < n; k++)
		ps[k].cx = cx;
	return ps;
}

typedef uint64 (*ValHash) (const SiftColValue *v);

static uint64
val_hash_i64(const SiftColValue *v)
{
	return sift_fmix64((uint64) v->v.i);
}

static uint64
val_hash_f8(const SiftColValue *v)
{
	return f8_hash(v->v.f);
}

static uint64
val_hash_str(const SiftColValue *v)
{
	return str_hash(v->v.s, v->len);
}

static void
pred_set_build(SiftPredSet *ps, const SiftColPred *p, ValHash hash, bool floats)
{
	ps->mask = pg_nextpower2_32(2 * (uint32) p->nvals) - 1;
	ps->slots = MemoryContextAllocZero(ps->cx, (ps->mask + 1) * sizeof(uint32));
	for (int j = 0; j < p->nvals; j++)
	{
		uint32		h;

		if (floats && isnan(p->vals[j].v.f))
		{
			ps->has_nan = true;
			continue;
		}
		for (h = (uint32) (hash(&p->vals[j]) & ps->mask); ps->slots[h] != 0; h = (h + 1) & ps->mask)
			;
		ps->slots[h] = j + 1;
	}
	ps->built = true;
}

bool
sift_f2_in_i64(SiftPredSet *ps, const SiftColPred *p, int64 v)
{
	if (ps == NULL || p->nvals < SIFT_PRED_SET_MIN_VALS)
	{
		for (int j = 0; j < p->nvals; j++)
			if (v == p->vals[j].v.i)
				return true;
		return false;
	}
	if (!ps->built)
		pred_set_build(ps, p, val_hash_i64, false);
	for (uint32 h = (uint32) (sift_fmix64((uint64) v) & ps->mask); ps->slots[h] != 0; h = (h + 1) & ps->mask)
		if (p->vals[ps->slots[h] - 1].v.i == v)
			return true;
	return false;
}

bool
sift_f2_in_f8(SiftPredSet *ps, const SiftColPred *p, double v)
{
	if (ps == NULL || p->nvals < SIFT_PRED_SET_MIN_VALS)
	{
		for (int j = 0; j < p->nvals; j++)
			if (sift_f2_f8_cmp(v, p->vals[j].v.f) == 0)
				return true;
		return false;
	}
	if (!ps->built)
		pred_set_build(ps, p, val_hash_f8, true);
	if (isnan(v))
		return ps->has_nan;
	for (uint32 h = (uint32) (f8_hash(v) & ps->mask); ps->slots[h] != 0; h = (h + 1) & ps->mask)
		if (sift_f2_f8_cmp(v, p->vals[ps->slots[h] - 1].v.f) == 0)
			return true;
	return false;
}

bool
sift_f2_in_str(SiftPredSet *ps, const SiftColPred *p, const char *s, uint32 len)
{
	if (ps == NULL || p->nvals < SIFT_PRED_SET_MIN_VALS)
	{
		for (int j = 0; j < p->nvals; j++)
			if (sift_f2_str_cmp(s, len, p->vals[j].v.s, p->vals[j].len) == 0)
				return true;
		return false;
	}
	if (!ps->built)
		pred_set_build(ps, p, val_hash_str, false);
	for (uint32 h = (uint32) (str_hash(s, len) & ps->mask); ps->slots[h] != 0; h = (h + 1) & ps->mask)
		if (sift_f2_str_cmp(s, len, p->vals[ps->slots[h] - 1].v.s, p->vals[ps->slots[h] - 1].len) == 0)
			return true;
	return false;
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
	return sift_f2_str_pred_in(NULL, p, s, len);
}

bool
sift_f2_str_pred_in(SiftPredSet *ps, const SiftColPred *p, const char *s, uint32 len)
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
			return sift_f2_in_str(ps, p, s, len) == (p->op == SIFT_COLOP_IN);
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
	return !sift_collation_deterministic(p->collid) || (sift_col_pred_is_bound(p) && !sift_collate_is_c(p->collid));
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

/* The operators of the hybrid scan the stored bytes decide: an array operator on the head of an array that keeps its image, compared by its bytes or a deterministic collation, a case folded prefix or equality under a collation whose lower() folds the ASCII capitals and no other character */
static bool
ext_decided(const SiftColDef *d, const SiftColPred *p)
{
	if (p->op == SIFT_COLOP_ARR_OVERLAP || p->op == SIFT_COLOP_ARR_CONTAINS)
		return sift_col_def_arrlen_head(d) && !(d->flags & SIFT_COLD_NOIMAGE) && (p->collid == InvalidOid || sift_collation_deterministic(p->collid));
	return d->store == SIFT_COLS_STR && !sift_col_def_is_wide(d) && d->lane == 0 && p->collid != InvalidOid && sift_ctype_is_c(p->collid);
}

/* A value the leaf holds in a form the test cannot judge (a wide number, the image of an array or a range, a pattern that folds case under a collation other than C, a string the bytes of a collation do not order): the heap decides */
bool
sift_f2_undecided(const SiftColDef *d, const SiftColPred *p)
{
	if (SIFT_COLOP_IS_EXT(p->op))
		return !ext_decided(d, p);
	if (d->type == SIFT_COLT_OTHER)
		return p->op != SIFT_COLOP_ISNULL && p->op != SIFT_COLOP_NOTNULL;	/* the image of a datum says whether it is NULL and nothing else */
	return sift_col_def_is_wide(d) || (d->lane > 0 && !sift_f2_card_lane(d)) || (d->flags & SIFT_COLD_LOSSY) || d->type == SIFT_COLT_VARLENA || d->type == SIFT_COLT_TIMETZ || p->op == SIFT_COLOP_SHAPE ||
		(p->op == SIFT_COLOP_ICONTAINS && (p->collid == InvalidOid || !sift_ctype_is_c(p->collid))) || (d->store == SIFT_COLS_STR && sift_f2_collation_undecided(p));
}

/* a numeric NaN is above every number and equals itself, whatever constant (a number) a comparison holds */
static Verdict
numeric_nan_verdict(SiftColOp op)
{
	switch (op)
	{
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
		case SIFT_COLOP_NOTNULL:
			return V_YES;
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_ISNULL:
			return V_NO;
		default:
			return V_MAYBE;
	}
}

Verdict
sift_f2_pred_row(const SiftColDef *d, const SiftColPred *p, SiftPredSet *ps, const ColView *cv, int r)
{
	bool		isnull;

	if (p->op == SIFT_COLOP_KEY_EXISTS)
		return cv->keyp[r] ? V_YES : V_NO;
	isnull = cv->isnull[r];
	if (p->op == SIFT_COLOP_JSON_TYPE)
	{
		uint8		type = sift_f2_json_type(d, isnull, cv->tag != NULL ? cv->tag[r] : 0);

		return type == SIFT_COLTAG_UNSURE ? V_MAYBE : (p->vals[0].v.i >> type) & 1 ? V_YES : V_NO;
	}
	if (isnull && cv->tag != NULL && cv->tag[r] == SIFT_COLTAG_UNSURE && d->type == SIFT_COLT_NUMERIC)
		return numeric_nan_verdict(p->op);
	if (isnull && cv->tag != NULL && cv->tag[r] != 0)
		return V_MAYBE;
	if (p->op == SIFT_COLOP_ISNULL)
		return isnull ? V_YES : V_NO;
	if (p->op == SIFT_COLOP_NOTNULL)
		return isnull ? V_NO : V_YES;
	if (isnull)
		return V_NO;
	if (sift_f2_undecided(d, p))
		return V_MAYBE;
	if (cv->kind == SIFT_CELL_STR)
		return (Verdict) (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? (sift_f2_str_pred_in(ps, p, cv->s[r], cv->slen[r]) ? SIFT_LIKE_YES : SIFT_LIKE_NO) : sift_f2_str_verdict(p, cv->s[r], cv->slen[r]));
	if (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN)
		return (cv->kind == SIFT_CELL_INT ? sift_f2_in_i64(ps, p, cv->i[r]) : sift_f2_in_f8(ps, p, cv->f[r])) == (p->op == SIFT_COLOP_IN) ? V_YES : V_NO;
	return sift_f2_cmp_op(p->op, cv->kind == SIFT_CELL_INT ? (cv->i[r] < p->vals[0].v.i ? -1 : cv->i[r] > p->vals[0].v.i) : sift_f2_f8_cmp(cv->f[r], p->vals[0].v.f)) ? V_YES : V_NO;
}
