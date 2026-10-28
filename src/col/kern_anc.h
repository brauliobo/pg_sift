/*
 * col/kern_anc.h - the anc_match kernel of the column kernels (col/kern.h), written once with the GCC/Clang vector extensions at the width of the level
 * (SIFT_V_BYTES of core/simd_isa.h). Included by col/kern_avx2.c, col/kern_avx512.c and col/kern_neon.c inside SIFT_VEC_PUSH / SIFT_VEC_POP after they define
 * SIFT_VEC_<level> and AN_FN(x), the kernel's name and storage. The tail is the specification's (sift_colk_anc_one).
 */
#include "core/simd_isa.h"

#define AN_L	(SIFT_V_BYTES / 8)

typedef uint64 an_u __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));
typedef int64 an_s __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));

AN_FN(anc_match)(const int64 *a, uint32 n, uint64 c, uint64 flip, uint64 bmin, uint64 bmax, uint64 *bits)
{
	const an_u	vc = (an_u) {} + c,
				vf = (an_u) {} + flip,
				vmin = (an_u) {} + bmin,
				vmax = (an_u) {} + bmax;
	uint32		i = 0;

	memset(bits, 0, SIFT_COLK_WORDS(n) * sizeof(uint64));
	for (; i + AN_L <= n; i += AN_L)
	{
		const an_u	x = *(const an_u *) (a + i) ^ vf,
					b = x & ((an_u) {} - x),
					t = ((vc & ((an_u) {} - b)) << 1) + b;
		const an_s	m = (an_s) (x == t) & (an_s) (b >= vmin) & (an_s) (b <= vmax);

		bits[i >> 6] |= SIFT_V_BITS64(m) << (i & 63);
	}
	for (; i < n; i++)
		bits[i >> 6] |= (uint64) sift_colk_anc_one(a[i], c, flip, bmin, bmax) << (i & 63);
}

typedef uint32 an_u32 __attribute__((vector_size(SIFT_V_BYTES), aligned(4), may_alias));

AN_FN(anc_match32)(const uint32 *a, uint32 n, uint32 base, uint32 c, uint32 bmin, uint32 bmax, uint64 *bits)
{
	const uint32 L = SIFT_V_BYTES / 4;
	const an_u32 vb = (an_u32) {} + base,
				 vc = (an_u32) {} + c,
				 vmin = (an_u32) {} + bmin,
				 vmax = (an_u32) {} + bmax;
	uint32		i = 0;

	memset(bits, 0, SIFT_COLK_WORDS(n) * sizeof(uint64));
	for (; i + L <= n; i += L)
	{
		const an_u32 x = *(const an_u32 *) (a + i) + vb,
					 b = x & ((an_u32) {} - x),
					 f = (x - b) >> 1;
		const an_u32 m = (an_u32) (f == (vc & ((an_u32) {} - b))) & (an_u32) (b >= vmin) & (an_u32) (b <= vmax);

		bits[i >> 6] |= SIFT_V_BITS32(m) << (i & 63);
	}
	for (; i < n; i++)
		bits[i >> 6] |= (uint64) sift_colk_anc32_one(a[i], base, c, bmin, bmax) << (i & 63);
}
