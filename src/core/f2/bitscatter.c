/*
 * core/bitscatter.c - the dense bits of the rows that hold a value spread over the rows (core/bitscatter.h).
 */
#include "core/host.h"

#include "core/f2/bitscatter.h"
#include "core/cpu.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* The 64 bits of dense from bit k on */
static inline uint64
bits_from(const uint64 *dense, uint32 k)
{
	uint64		x = dense[k >> 6] >> (k & 63);

	return (k & 63) ? x | (dense[(k >> 6) + 1] << (64 - (k & 63))) : x;
}

static inline uint64
last_mask(uint32 nrows)
{
	return (nrows & 63) ? (UINT64CONST(1) << (nrows & 63)) - 1 : ~UINT64CONST(0);
}

void
sift_bits_scatter_valid_scalar(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows)
{
	uint32		nwords = (nrows + 63) >> 6,
				k = 0;

	for (uint32 w = 0; w < nwords; w++)
	{
		uint64		v = valid_bits[w] & (w + 1 == nwords ? last_mask(nrows) : ~UINT64CONST(0)),
					row = 0;

		if (v == ~UINT64CONST(0))
		{
			row = bits_from(dense_bits, k);
			k += 64;
		}
		else
			for (; v; v &= v - 1)
			{
				row |= ((dense_bits[k >> 6] >> (k & 63)) & 1) << __builtin_ctzll(v);
				k++;
			}
		row_bits[w] = row;
	}
}

#if defined(__x86_64__)
SIFT_TARGET_AVX2 static void
scatter_pdep_avx2(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows)
{
	uint32		nwords = (nrows + 63) >> 6,
				k = 0;

	for (uint32 w = 0; w < nwords; w++)
	{
		uint64		v = valid_bits[w] & (w + 1 == nwords ? last_mask(nrows) : ~UINT64CONST(0));

		row_bits[w] = v == 0 ? 0 : _pdep_u64(bits_from(dense_bits, k), v);
		k += __builtin_popcountll(v);
	}
}
#endif

void
sift_bits_scatter_valid(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows)
{
#if defined(__x86_64__)
	if (sift_cpu.flags & SIFT_CPU_BMI2_FAST)
	{
		scatter_pdep_avx2(row_bits, dense_bits, valid_bits, nrows);
		return;
	}
#endif
	sift_bits_scatter_valid_scalar(row_bits, dense_bits, valid_bits, nrows);
}
