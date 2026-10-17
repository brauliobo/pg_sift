/*
 * sqscalar.c - the build with SIMD=none: the AVX2 and AVX-512 files are not compiled, and the tables the dispatch
 * refers to for those levels hold the scalar kernels (copied at start), so no vector instruction is linked and every
 * level runs the reference code.
 */
#include "postgres.h"

#include "sift_bitmap.h"
#include "sift_kernels.h"

extern const sift_kernels sift_kern_scalar;
extern const SiftKernels sift_kernels_scalar;

sift_kernels sift_kern_avx2,
			sift_kern_avx512;
SiftKernels sift_kernels_avx2,
			sift_kernels_avx2_nopdep,
			sift_kernels_avx512;

void
sq_scalar_tables(void)
{
	sift_kern_avx2 = sift_kern_avx512 = sift_kern_scalar;
	sift_kernels_avx2 = sift_kernels_avx2_nopdep = sift_kernels_avx512 = sift_kernels_scalar;
}
