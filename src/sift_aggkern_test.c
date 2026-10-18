/*
 * sift_aggkern_test.c - test-only SQL functions sift._test_aggkern_*, compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension. The standalone harness (tools/aggkern) is the exhaustive check; these run the same random cases
 * (sift_aggkern_cases.h) inside the backend, through the sift.simd dispatch.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#define AGK_ALLOC(n)	palloc_aligned(n, SIFT_SIMD_ALIGN, 0)
#define AGK_FREE(p)		pfree(p)

#include "sift_aggkern_cases.h"

PG_FUNCTION_INFO_V1(sift_test_aggkern_digest);
PG_FUNCTION_INFO_V1(sift_test_aggkern_equiv);
PG_FUNCTION_INFO_V1(sift_test_aggkern_level);

/* Hash of every output of the active table over the seeded random cases (the executor's init may not have run) */
Datum
sift_test_aggkern_digest(PG_FUNCTION_ARGS)
{
	AgkRng		r;
	AgkCase		c;
	AgkRes		res;
	uint64		h = UINT64CONST(0xCBF29CE484222325);

	sift_aggkern_init();
	agk_seed(&r, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		agk_case_gen(&r, &c, -1);
		agk_run(sift_aggk, &c, &res);
		h = (h ^ res.h) * UINT64CONST(0x100000001B3);
		agk_case_free(&c);
	}
	PG_RETURN_INT64((int64) h);
}

/* Cases where a SIMD level the CPU supports differs from scalar */
Datum
sift_test_aggkern_equiv(PG_FUNCTION_ARGS)
{
	AgkRng		r;
	AgkCase		c;
	AgkRes		ref,
				got;
	int64		bad = 0;

	agk_seed(&r, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		agk_case_gen(&r, &c, -1);
		agk_run(&sift_aggkern_scalar, &c, &ref);
		for (SiftSimdLevel l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_AVX512; l++)
		{
			if (sift_aggkern_for(l) == sift_aggkern_for(l - 1))
				continue;
			agk_run(sift_aggkern_for(l), &c, &got);
			bad += agk_first_diff(&ref, &got) >= 0;
		}
		agk_case_free(&c);
	}
	PG_RETURN_INT64(bad);
}

/* The level of the active table: the dispatch follows sift.simd */
Datum
sift_test_aggkern_level(PG_FUNCTION_ARGS)
{
	sift_aggkern_init();
	for (SiftSimdLevel l = SIFT_SIMD_AVX512; l > SIFT_SIMD_SCALAR; l--)
		if (sift_aggk == sift_aggkern_for(l) && sift_aggkern_for(l) != sift_aggkern_for(l - 1))
			PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(l)));
	PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(SIFT_SIMD_SCALAR)));
}

#endif							/* SIFT_TESTING */
