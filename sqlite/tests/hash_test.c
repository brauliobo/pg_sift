/*
 * hash_test.c - pins the outputs of the mixers of src/core/hash.h. Group keys hash through them and an unordered group output comes out in the
 * order of the hashes, so a changed constant or step changes results. The expected values were computed from the original spelling of each mixer.
 * C library only; built by the portable flags of sqlite/Makefile (make hash-test), run by test.sh.
 */
#include <stdio.h>

#include "core/hash.h"

static const struct
{
	uint64_t	in,
				fib,				/* sift_fib64(in) */
				fold,				/* sift_fold64(SIFT_FIB64, in) */
				fmix;				/* sift_fmix64(in) */
} want[] = {
	{UINT64_C(0x0), UINT64_C(0x0), UINT64_C(0x4D0A2BBE35B3010F), UINT64_C(0x0)},
	{UINT64_C(0x1), UINT64_C(0x9E3779B97F4A7C15), UINT64_C(0x76212D05CF59037C), UINT64_C(0xB456BCFC34C2CB2C)},
	{UINT64_C(0x2), UINT64_C(0x3C6EF372FE94F82A), UINT64_C(0xFADC292F0266FC35), UINT64_C(0x3ABF2A20650683E7)},
	{UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x61C8864680B583EB), UINT64_C(0xDC0CD58963F3015E), UINT64_C(0x64B5720B4B825F21)},
	{UINT64_C(0x123456789ABCDEF), UINT64_C(0xC93A7B79AEDA89B), UINT64_C(0x707154C67448E), UINT64_C(0x87CBFBFE89022CEA)},
	{UINT64_C(0xF4243), UINT64_C(0xD7C523BAC9B6E37F), UINT64_C(0x76E830047343C962), UINT64_C(0xE7144FF0F6F2E94C)},
};

int
main(void)
{
	int			bad = 0;

	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
	{
		uint64_t	x = want[i].in;

		if (sift_fib64(x) != want[i].fib || sift_fold64(SIFT_FIB64, x) != want[i].fold || sift_fmix64(x) != want[i].fmix)
		{
			fprintf(stderr, "hash_test: mixer outputs of 0x%llx changed\n", (unsigned long long) x);
			bad++;
		}
	}
	printf("hash_test: %s\n", bad ? "FAILED" : "mixers pinned");
	return bad != 0;
}
