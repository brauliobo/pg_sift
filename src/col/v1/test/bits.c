/*
 * col/v1/test/bits.c - the reads of a bit vector stay inside the words a batch allocates for it (the memcheck lane saw one run past them: core/simd_kern.h sift_col_bits_any)
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "fmgr.h"

#include "core/simd_kern.h"
#include "col/v1/test/int.h"

PG_FUNCTION_INFO_V1(sift_test_bits_any_bounds);

/* sift._test_bits_any_bounds(n int) returns bool: whether sift_col_bits_any finds a bit in n clear bits of a vector of (n + 63) / 64 words that is followed by words that are all set */
Datum
sift_test_bits_any_bounds(PG_FUNCTION_ARGS)
{
	uint32		n = PG_GETARG_INT32(0);
	Size		words = (n + 63) / 64;
	uint64	   *bits = palloc(2 * (words + SIFT_COL_SELWORDS) * sizeof(uint64));

	memset(bits, 0, words * sizeof(uint64));
	memset(bits + words, 0xFF, (words + SIFT_COL_SELWORDS) * sizeof(uint64));
	PG_RETURN_BOOL(sift_col_bits_any(bits, n));
}
#endif
