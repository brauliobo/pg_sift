/*
 * agg/kern6/test.c - test-only SQL functions sift._test_aggkern6_*, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. They run the random cases of agg/kern6/cases.h inside the backend, through the sift.simd dispatch (the standalone harness,
 * tools/aggkern6, is the exhaustive check).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#define AK6C_ALLOC(n)	palloc_aligned(n, SIFT_VEC_ALIGN, 0)
#define AK6C_FREE(p)	pfree(p)

#include "agg/kern6/cases.h"

PG_FUNCTION_INFO_V1(sift_test_aggkern6_equiv);
PG_FUNCTION_INFO_V1(sift_test_aggkern6_level);

/* Cases where a SIMD level the CPU supports differs from scalar */
Datum
sift_test_aggkern6_equiv(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		for (SiftSimdLevel l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_TOP; l++)
			if (sift_aggkern6_for(l) != sift_aggkern6_for(l - 1))
				bad += ak6c_equiv(&sift_aggkern6_scalar, sift_aggkern6_for(l), PG_GETARG_INT64(0) + i, -1);
#if defined(__x86_64__)
		if (sift_aggkern6_for(SIFT_SIMD_AVX512) == &sift_aggkern6_avx512)
			bad += ak6c_equiv(&sift_aggkern6_scalar, &sift_aggkern6_avx512_conflict, PG_GETARG_INT64(0) + i, -1);
#elif defined(SIFT_HAVE_NEON)
		if (sift_aggkern6_for(SIFT_SIMD_NEON) == &sift_aggkern6_neon)
			bad += ak6c_equiv(&sift_aggkern6_scalar, &sift_aggkern6_neon_vector, PG_GETARG_INT64(0) + i, -1);
#endif
	}
	PG_RETURN_INT64(bad);
}

/* The level of the active table: the dispatch follows sift.simd */
Datum
sift_test_aggkern6_level(PG_FUNCTION_ARGS)
{
	sift_aggkern6_init();
	for (SiftSimdLevel l = SIFT_SIMD_TOP; l > SIFT_SIMD_SCALAR; l--)
		if (sift_aggk6 == sift_aggkern6_for(l) && sift_aggkern6_for(l) != sift_aggkern6_for(l - 1))
			PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(l)));
	PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(SIFT_SIMD_SCALAR)));
}

#endif							/* SIFT_TESTING */
