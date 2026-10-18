/*
 * sift_colcodec_vec.h - the width independent part of the SIMD kernels of the chunk codec: the vertical bit-unpacking, written once with
 * GCC/Clang vector extensions and compiled by each level file at its own width (CC_VB bytes per vector: 32 for AVX2, 64 for AVX-512)
 * under the level's target options. Included by sift_colcodec_avx2.c and sift_colcodec_avx512.c after they define CC_VB and CC_FN(x).
 */

typedef uint32 cc_vu32 __attribute__((vector_size(CC_VB), aligned(4), may_alias));

#define CC_LANES	(CC_VB / 4)		/* u32 lanes per vector */
#define CC_NV		(32 / CC_LANES)	/* vectors per row of 32 values */
#define CC_INLINE	static inline __attribute__((always_inline))

/* Row r of a block: the 32 values (CC_NV vectors) at bit r * W of the lane streams, plus add (wrapping), into out[] */
CC_INLINE void
CC_FN(row)(const uint32 *a, uint32 s, uint32 W, uint32 mask, uint32 add, cc_vu32 *out)
{
	for (int q = 0; q < CC_NV; q++)
	{
		cc_vu32		x = *(const cc_vu32 *) (a + q * CC_LANES) >> s;

		if (s + W > 32)
			x |= *(const cc_vu32 *) (a + 32 + q * CC_LANES) << (32 - s);
		out[q] = (x & mask) + add;
	}
}

static void
CC_FN(unpack32)(const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;

	if (W == 0)
	{
		for (uint64 i = 0; i < (uint64) rows * 32; i++)
			dst[i] = base;
		return;
	}
	while (rows)
	{
		uint32		R = rows > 32 ? 32 : rows;

		for (uint32 r = 0; r < R; r++)
		{
			uint32		bp = r * W;
			cc_vu32		v[CC_NV];

			CC_FN(row)(src + (size_t) (bp >> 5) * 32, bp & 31, W, mask, base, v);
			for (int q = 0; q < CC_NV; q++)
				*(cc_vu32 *) (dst + r * 32 + q * CC_LANES) = v[q];
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
		dst += R * 32;
		rows -= R;
	}
}
