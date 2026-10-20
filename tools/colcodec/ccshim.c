/*
 * ccshim.c - the CPU dispatch of the chunk codec's standalone test and benchmark, on top of tools/pgshim.c (the backend symbols). The
 * cpu module is tools/kernels/shim.c.
 */
#include "postgres.h"

#include "core/sift.h"
#include "harness.h"
#include "ccshim.h"

void
cc_init(void)
{
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
}

int
cc_levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (!n || sift_cc_kernels_for((SiftSimdLevel) l) != sift_cc_kernels_for(lv[n - 1]) ||
			sift_col_kernels_for((SiftSimdLevel) l) != sift_col_kernels_for(lv[n - 1]))
			lv[n++] = (SiftSimdLevel) l;
	return n;
}

void
cc_use_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
}
