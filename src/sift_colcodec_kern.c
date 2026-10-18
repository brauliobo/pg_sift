/*
 * sift_colcodec_kern.c - scalar kernels of the chunk codec (the specification every SIMD level matches bit for bit), the decimal power
 * tables and the dispatch (docs/COL-CODEC.md section 4, docs/KERNELS.md).
 */
#include "sift_host.h"

#include "sift_colcodec_int.h"

const double sift_cc_p10[19] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
const float sift_cc_p10f[11] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

const SiftCcKernels *sift_cck = &sift_cc_kernels_scalar;

static void
unpack32_scalar(const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst)
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
			uint32		bp = r * W,
						s = bp & 31;
			const uint32 *a = src + (size_t) (bp >> 5) * 32;

			for (uint32 l = 0; l < 32; l++)
			{
				uint32		x = sift_cc_ld32(a + l) >> s;

				if (s + W > 32)
					x |= sift_cc_ld32(a + 32 + l) << (32 - s);
				dst[r * 32 + l] = base + (x & mask);
			}
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
		dst += R * 32;
		rows -= R;
	}
}

static void
unpack_range32_scalar(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;

	for (uint32 w = 0; w < (rows + 1) / 2; w++)
		bits[w] = 0;
	while (row < rows)
	{
		uint32		R = rows - row > 32 ? 32 : rows - row;

		for (uint32 r = 0; r < R; r++, row++)
		{
			uint32		bp = r * W,
						s = bp & 31,
						m = 0;
			const uint32 *a = src + (size_t) (bp >> 5) * 32;

			for (uint32 l = 0; l < 32; l++)
			{
				uint32		x = W ? sift_cc_ld32(a + l) >> s : 0;

				if (W && s + W > 32)
					x |= sift_cc_ld32(a + 32 + l) << (32 - s);
				m |= (uint32) (((x & mask) - lo) <= span) << l;
			}
			bits[row / 2] |= (uint64) m << ((row & 1) * 32);
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
	}
}

static void
prefix32_scalar(const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add)
{
	uint32		run = first;

	if (n == 0)
		return;
	dst[0] = run;
	for (uint32 i = 1; i < n; i++)
	{
		run += a[i] + add;
		dst[i] = run;
	}
}

static void
prefix64_scalar(const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add)
{
	uint64		run = (uint64) first;

	if (n == 0)
		return;
	dst[0] = (int64) run;
	for (uint32 i = 1; i < n; i++)
	{
		run += (uint64) a[i] + (uint64) add;
		dst[i] = (int64) run;
	}
}

static uint32
max32_scalar(const uint32 *a, uint32 n)
{
	uint32		m = 0;

	for (uint32 i = 0; i < n; i++)
		m = a[i] > m ? a[i] : m;
	return m;
}

static void
alp_keys_f8_scalar(int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	for (uint32 i = 0; i < n; i++)
		dst[i] = sift_col_f8_key((double) (int64) ((uint64) dbase + u[i]) / div);
}

static void
alp_keys_f4_scalar(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	for (uint32 i = 0; i < n; i++)
		dst[i] = sift_col_f8_key((double) ((float) (int32) (uint32) ((uint64) dbase + u[i]) / div));
}

static void
rle_expand32_scalar(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	for (uint32 r = 0; r < nruns; r++)
		for (uint32 j = 0; j < lens[r]; j++)
			*dst++ = vals[r];
}

const SiftCcKernels sift_cc_kernels_scalar = {
	.unpack32 = unpack32_scalar,
	.unpack_range32 = unpack_range32_scalar,
	.prefix32 = prefix32_scalar,
	.prefix64 = prefix64_scalar,
	.max32 = max32_scalar,
	.alp_keys_f8 = alp_keys_f8_scalar,
	.alp_keys_f4 = alp_keys_f4_scalar,
	.rle_expand32 = rle_expand32_scalar,
};

const SiftCcKernels *
sift_cc_kernels_for(SiftSimdLevel level)
{
#if defined(__x86_64__)
	if (level > sift_cpu.detected)
		level = sift_cpu.detected;
	if (level == SIFT_SIMD_AVX512)
		return &sift_cc_kernels_avx512;
	if (level == SIFT_SIMD_AVX2)
		return &sift_cc_kernels_avx2;
#endif
	return &sift_cc_kernels_scalar;
}

static void
cc_rebind(const SiftCpu *cpu)
{
	sift_cck = sift_cc_kernels_for(cpu->active);
}

void
sift_cc_kernels_init(void)
{
	sift_cpu_register(cc_rebind);
}
