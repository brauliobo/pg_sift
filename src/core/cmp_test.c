/*
 * core/cmp_test.c - test-only SQL function sift._test_radix_sort, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: random
 * arrays of one and of two keys sorted by SIFT_DEFINE_RADIX_SORT and by SIFT_DEFINE_SORT (core/cmp.h), which must give the same array.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#include "core/cmp.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_radix_sort);

typedef struct RadixEnt
{
	int64_t		k1;
	int64_t		k2;
	uint32_t	tie;
} RadixEnt;

#define RADIX_LESS(x, y) ((x)->k1 != (y)->k1 ? (x)->k1 < (y)->k1 : (x)->k2 != (y)->k2 ? (x)->k2 < (y)->k2 : (x)->tie < (y)->tie)
SIFT_DEFINE_SORT(radix_cmp, RadixEnt, RADIX_LESS)

#define RADIX_K1(x) ((x)->k1)
#define RADIX_K2(x) ((x)->k2)
#define RADIX_NOKEY(x) ((int64_t) 0)
#define RADIX_TIE(x) ((x)->tie)

static void *
radix_alloc(size_t bytes)
{
	return palloc_extended(bytes, MCXT_ALLOC_NO_OOM);
}

SIFT_DEFINE_RADIX_SORT(radix_two, RadixEnt, RADIX_K1, RADIX_K2, RADIX_TIE, radix_alloc, pfree, radix_cmp)
SIFT_DEFINE_RADIX_SORT(radix_one, RadixEnt, RADIX_K1, RADIX_NOKEY, RADIX_TIE, radix_alloc, pfree, radix_cmp)

/*
 * (n int4, bits1 int4, bits2 int4, offset1 int8, offset2 int8, seed int8) returns bool: n entries whose keys are random of bits1 and bits2 bits (0: one value; bits2 < 0: a single key)
 * moved by an offset (negative keys, and the extremes of int8 with offset = int8 min), their ties in order of their position: the radix sort gives the array of the comparison sort.
 */
Datum
sift_test_radix_sort(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0),
				bits1 = PG_GETARG_INT32(1),
				bits2 = PG_GETARG_INT32(2);
	uint64_t	off1 = (uint64_t) PG_GETARG_INT64(3),
				off2 = (uint64_t) PG_GETARG_INT64(4),
				rs = (uint64_t) PG_GETARG_INT64(5),
				m1 = bits1 >= 64 ? UINT64_MAX : ((uint64_t) 1 << bits1) - 1,
				m2 = bits2 >= 64 ? UINT64_MAX : bits2 < 0 ? 0 : ((uint64_t) 1 << bits2) - 1;
	RadixEnt   *a = palloc(Max(n, 1) * sizeof(RadixEnt)),
			   *b = palloc(Max(n, 1) * sizeof(RadixEnt));
	bool		same = true;

	for (int32 i = 0; i < n; i++)
	{
		a[i].k1 = (int64_t) ((sift_splitmix_next(&rs) & m1) + off1);
		a[i].k2 = (int64_t) ((sift_splitmix_next(&rs) & m2) + off2);
		a[i].tie = (uint32_t) i;
	}
	memcpy(b, a, n * sizeof(RadixEnt));
	radix_cmp(a, n);
	if (bits2 < 0)
		radix_one(b, n);
	else
		radix_two(b, n);
	for (int32 i = 0; i < n; i++)
		same &= a[i].k1 == b[i].k1 && a[i].k2 == b[i].k2 && a[i].tie == b[i].tie;
	PG_RETURN_BOOL(same);
}

#endif							/* SIFT_TESTING */
