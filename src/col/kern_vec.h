/*
 * col/kern_vec.h - the column kernels that widen or rewrite 64-bit lanes (for_i64, f8_from_keys), written once with the GCC/Clang vector extensions
 * at the width of the level (SIFT_V_BYTES of core/simd_isa.h). Included by col/kern_avx2.c and col/kern_neon.c inside SIFT_VEC_PUSH / SIFT_VEC_POP
 * after they define SIFT_VEC_<level> and CV_FN(x), the kernels' names and storage. The tails are the scalar kernels'.
 */
#include "core/simd_isa.h"

#define CV_L	(SIFT_V_BYTES / 8)

typedef uint64 cv_u __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));
typedef int64 cv_s __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));

/*
 * dst = base + the zero extended elements, wrapping; a step reads 16 bytes (a vector when that is wider) in vectors of CV_LANES(T) elements: one
 * vector for a level that widens a whole 16 bytes in a few instructions (NEON), else the lanes of a 64-bit vector (GCC at AVX2 makes a long
 * sequence of the wide conversion)
 */
#define CV_STEP(T)	((uint32) (16 / sizeof(T) > CV_L ? 16 / sizeof(T) : CV_L))
#ifndef CV_LANES
#define CV_LANES(T)	CV_L
#endif

#define CV_FOR_I64(W, T) \
CV_FN(for_i64_##W)(const void *a, uint32 n, int64 base, int64 *dst) \
{ \
	typedef T cv_n_##W __attribute__((vector_size(CV_LANES(T) * sizeof(T)), aligned(1), may_alias)); \
	typedef uint64 cv_w_##W __attribute__((vector_size(CV_LANES(T) * 8), aligned(8), may_alias)); \
	const T    *p = a; \
	const cv_w_##W b = (cv_w_##W) {} + (uint64) base; \
	uint32		i = 0; \
	for (; i + CV_STEP(T) <= n; i += CV_STEP(T)) \
	{ \
		SIFT_COLK_UNROLL(16) \
		for (uint32 j = 0; j < CV_STEP(T); j += CV_LANES(T)) \
			*(cv_w_##W *) (dst + i + j) = __builtin_convertvector(*(const cv_n_##W *) (p + i + j), cv_w_##W) + b; \
	} \
	sift_colk_for_i64_##W##_scalar(p + i, n - i, base, dst + i); \
}

CV_FOR_I64(u8, uint8)
CV_FOR_I64(u16, uint16)
CV_FOR_I64(u32, uint32)
CV_FOR_I64(u64, uint64)

/* the order preserving int64 of a double: k ^ ((k >> 63 arithmetic) >> 1 logical) */
CV_FN(f8_from_keys)(int64 *a, uint32 n)
{
	uint32		i = 0;

	for (; i + CV_L <= n; i += CV_L)
	{
		const cv_s	k = *(const cv_s *) (a + i);

		*(cv_s *) (a + i) = k ^ (cv_s) ((cv_u) (k < 0) >> 1);
	}
	sift_colk_f8_from_keys_scalar(a + i, n - i);
}
