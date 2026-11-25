/*
 * agg/argext/argext.h - sift.arg_max(val, cmp) and sift.arg_min(val, cmp) in SiftAggV6, with ClickHouse's argMax / argMin semantics: the value of val on the row where cmp is greatest
 * (least); a row whose cmp is NULL is ignored, a NULL val on the winning row is the result (NULL), and a group with no row whose cmp is not NULL has no result (NULL). When two rows tie on cmp the first
 * one offered wins, as with ORDER BY .. LIMIT 1 over a stable scan; which of them PostgreSQL's own aggregate returns after a parallel combine is as unspecified as its ORDER BY is.
 *
 * Every comparison is of an order-preserving key: one 64-bit word whose unsigned order is the order of PostgreSQL.
 */
#ifndef SIFT_AGG_V6_ARGEXT_H
#define SIFT_AGG_V6_ARGEXT_H

#include "core/host.h"

#include "agg/vec.h"

typedef enum SiftArgDir
{
	SIFT_ARG_MAX,
	SIFT_ARG_MIN
} SiftArgDir;

#define SIFT_ORD_SIGN64		(UINT64_C(1) << 63)

/* The 64-bit key of an int8 and of a float8 for ASC: the unsigned order of the word is the order of the value */
static inline uint64
sift_ord_u64_of_i64(int64 v)
{
	return (uint64) v ^ SIFT_ORD_SIGN64;
}

static inline uint64
sift_ord_u64_of_f64(double v)
{
	uint64		bits;

	if (v != v)
		bits = UINT64_C(0x7FF8000000000000);
	else if (v == 0)
		bits = 0;
	else
		memcpy(&bits, &v, sizeof(bits));
	return (bits & SIFT_ORD_SIGN64) ? ~bits : bits | SIFT_ORD_SIGN64;
}

/*
 * The state of one group in the 32-byte accumulator of agg/group/group.c (n, v0, v1, pad): n counts the rows whose cmp is not NULL (0: no result), key is the order key of the best cmp, val its val, val_null
 * says the val is NULL.
 */
static inline bool
sift_argacc_better(SiftArgDir dir, uint64 cmp_key, uint64 best_key)
{
	return dir == SIFT_ARG_MAX ? cmp_key > best_key : cmp_key < best_key;
}

static inline void
sift_argacc_offer(int64 *n, uint64 *key, int64 *val, int64 *val_null, SiftArgDir dir, uint64 cmp_key, int64 v, bool v_null)
{
	if (*n == 0 || sift_argacc_better(dir, cmp_key, *key))
	{
		*key = cmp_key;
		*val = v;
		*val_null = v_null;
	}
	(*n)++;
}

static inline void
sift_argacc_merge(int64 *dn, uint64 *dkey, int64 *dval, int64 *dnull, SiftArgDir dir, int64 sn, uint64 skey, int64 sval, int64 snull)
{
	if (sn == 0)
		return;
	if (*dn == 0 || sift_argacc_better(dir, skey, *dkey))
	{
		*dkey = skey;
		*dval = sval;
		*dnull = snull;
	}
	*dn += sn;
}

/* The order key of row r of a cmp vector (I32, I64 or F64): the unsigned order of the word is the order of PostgreSQL */
static inline uint64
sift_argext_cmp_key(const SiftVec *cmp, uint32 r)
{
	switch (cmp->type)
	{
		case SVEC_I32:
			return sift_ord_u64_of_i64(((const int32 *) cmp->data)[r]);
		case SVEC_F64:
			return sift_ord_u64_of_f64(((const double *) cmp->data)[r]);
		default:
			return sift_ord_u64_of_i64(((const int64 *) cmp->data)[r]);
	}
}

#endif							/* SIFT_AGG_V6_ARGEXT_H */
