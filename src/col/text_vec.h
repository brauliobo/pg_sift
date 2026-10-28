/*
 * col/text_vec.h - the text kernels of the column index, written once with the GCC/Clang vector extensions and compiled by each level file at its
 * width (SIFT_V_BYTES of core/simd_isa.h). Included by col/kern_avx2.c, col/kern_avx512.c and col/kern_neon.c inside SIFT_VEC_PUSH / SIFT_VEC_POP after
 * they define SIFT_VEC_<level> and TV_FN(x), the kernels' names and storage. The tails are the scalar kernels' (col/kern.c).
 */
#include "core/simd_isa.h"

typedef uint8 tv_b __attribute__((vector_size(SIFT_V_BYTES), aligned(1), may_alias));

/*
 * str_contains over all the text of the strings at once, so the cost follows the bytes of a dictionary and not its entries. A window of SIFT_V_BYTES
 * starts is narrowed to those where the first byte of the needle at p and its last byte at p + m - 1 both match; a start is checked against the
 * string it falls in (the strings are in order: the string is a cursor that only advances), and after a hit the scan resumes at the next string.
 */
TV_FN(str_contains)(uint64 *out, const char *bytes, const uint32 *off, uint32 n, const char *needle, uint32 m)
{
	const uint8 *buf = (const uint8 *) bytes,
			   *nd = (const uint8 *) needle;
	const tv_b	first = (tv_b) {} + nd[0],
				last = (tv_b) {} + nd[m - 1];
	uint64		pos = n > 0 ? off[0] : 0,
				end = n > 0 ? off[n] : 0;
	uint32		s = 0;

	memset(out, 0, SIFT_COLK_WORDS(n) * sizeof(uint64));
	while (pos + (m - 1) + SIFT_V_BYTES <= end)
	{
		uint64		cand = SIFT_V_BITS8((*(const tv_b *) (buf + pos) == first) & (*(const tv_b *) (buf + pos + m - 1) == last)) & SIFT_V_BITS8_ONES;

		for (; cand; cand &= cand - 1)
		{
			uint64		p = pos + (__builtin_ctzll(cand) >> SIFT_V_BITS8_SHIFT);

			while (off[s + 1] <= p)
				s++;
			if (p + m <= off[s + 1] && memcmp(buf + p, nd, m) == 0)
				break;
		}
		if (cand)
		{
			out[s >> 6] |= UINT64CONST(1) << (s & 63);
			pos = off[s + 1];
		}
		else
			pos += SIFT_V_BYTES;
	}
	/* fewer than a window of bytes is left: the strings that reach it, whole, as the scalar kernel tests them */
	while (s < n && off[s + 1] <= pos)
		s++;
	for (; s < n; s++)
		if (!(out[s >> 6] >> (s & 63) & 1) && sift_colk_contains1(buf + off[s], off[s + 1] - off[s], nd, m))
			out[s >> 6] |= UINT64CONST(1) << (s & 63);
}
