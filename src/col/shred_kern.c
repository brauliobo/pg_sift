/*
 * col/shred_kern.c - the scalar kernels of the shredder output (the specification; col/shred_kern_avx2.c and
 * col/shred_kern_avx512.c must match them bit for bit) and the runtime dispatch (docs/KERNELS.md).
 */
#include "sift_host.h"

#include "col/shred.h"

static void
state_counts_scalar(const uint8 *state, uint32 n, uint32 counts[4])
{
	counts[0] = counts[1] = counts[2] = counts[3] = 0;
	for (uint32 i = 0; i < n; i++)
		counts[state[i]]++;
}

static void
state_bits_scalar(const uint8 *state, uint32 n, uint64 *nulls, uint64 *ovf)
{
	memset(nulls, 0, (n + 63) / 64 * sizeof(uint64));
	memset(ovf, 0, (n + 63) / 64 * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		nulls[i / 64] |= (uint64) (state[i] != SIFT_SHRED_VALUE) << (i % 64);
		ovf[i / 64] |= (uint64) (state[i] == SIFT_SHRED_OVERFLOW) << (i % 64);
	}
}

static bool
minmax_i64_scalar(const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max)
{
	bool		found = false;
	int64		lo = 0,
				hi = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		if (!found || val[i] < lo)
			lo = val[i];
		if (!found || val[i] > hi)
			hi = val[i];
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

static bool
minmax_f64_scalar(const uint8 *state, const double *val, uint32 n, double *min, double *max)
{
	bool		found = false;
	double		lo = 0,
				hi = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		if (!found || val[i] < lo)
			lo = val[i];
		if (!found || val[i] > hi)
			hi = val[i];
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

const SiftShredKernels sift_shred_kernels_scalar = {
	.state_counts = state_counts_scalar,
	.state_bits = state_bits_scalar,
	.minmax_i64 = minmax_i64_scalar,
	.minmax_f64 = minmax_f64_scalar
};

const SiftShredKernels *sift_shred_kern = &sift_shred_kernels_scalar;

const SiftShredKernels *
sift_shred_kernels_for(SiftSimdLevel level)
{
	if (level > sift_cpu.detected)
		return NULL;
#if defined(__x86_64__)
	if (level == SIFT_SIMD_AVX512)
		return &sift_shred_kernels_avx512;
	if (level == SIFT_SIMD_AVX2)
		return &sift_shred_kernels_avx2;
#endif
	return &sift_shred_kernels_scalar;
}

static void
shred_rebind(const SiftCpu *cpu)
{
	sift_shred_kern = sift_shred_kernels_for(cpu->active);
}

/* Idempotent: sift_shred_create calls it too, so a user of the shredder needs no hook in _PG_init */
void
sift_shred_init(void)
{
	static bool done = false;

	if (!done)
	{
		done = true;
		sift_cpu_register(shred_rebind);
	}
}
