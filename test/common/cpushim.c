/*
 * cpushim.c - the cpu module of the extension (core/cpu.c) for the standalone harnesses, with the same detection rules: sift_cpu is filled from the
 * hardware before main, kt_cpu_init() repeats it and builds the dispatch tables of the bm25 kernels. A harness that links no bm25/kernels.c
 * compiles it with -DSIFT_CPUSHIM_DETECT_ONLY, one that links bm25/bitmap.c (it defines sift_k) with -DSIFT_CPUSHIM_NO_SIFT_K.
 */
#include "postgres.h"

#include "bm25/kernels_int.h"

#include "bm25/bitmap_kern.h"
#include "core/cpu.h"

SiftCpu		sift_cpu;

#ifndef SIFT_CPUSHIM_NO_SIFT_K
/* The bitmap kernels reach their own table only through count_mask, which no tool calls */
const sift_kernels *sift_k = &sift_kern_scalar;
#endif
#if defined(SIFT_HAVE_NEON)
const char *const kt_level_name[] = {"scalar", "neon"};
#else
const char *const kt_level_name[] = {"scalar", "avx2", "avx512"};
#endif

void
sift_cpu_register(SiftDispatchRebind fn)
{
	fn(&sift_cpu);
}

static void
detect(void)
{
#if defined(__x86_64__)
	uint32		f = 0;
	const uint32 avx2 = SIFT_CPU_AVX2 | SIFT_CPU_BMI2 | SIFT_CPU_POPCNT;
	const uint32 avx512 = avx2 | SIFT_CPU_AVX512 | SIFT_CPU_AVX512_VPOPCNT | SIFT_CPU_AVX512_VBMI2;

	__builtin_cpu_init();
	f |= __builtin_cpu_supports("popcnt") ? SIFT_CPU_POPCNT : 0;
	f |= __builtin_cpu_supports("bmi2") ? SIFT_CPU_BMI2 | SIFT_CPU_BMI2_FAST : 0;
	f |= __builtin_cpu_supports("avx2") ? SIFT_CPU_AVX2 : 0;
	f |= __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512cd") ? SIFT_CPU_AVX512 : 0;
	f |= __builtin_cpu_supports("avx512vpopcntdq") ? SIFT_CPU_AVX512_VPOPCNT : 0;
	f |= __builtin_cpu_supports("avx512vbmi2") ? SIFT_CPU_AVX512_VBMI2 : 0;
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("fma") &&
		__builtin_cpu_supports("popcnt") && __builtin_cpu_supports("f16c") && __builtin_cpu_supports("movbe"))
		f |= SIFT_CPU_X86_V3;
	if ((f & SIFT_CPU_X86_V3) && (f & SIFT_CPU_AVX512))
		f |= SIFT_CPU_X86_V4;
	sift_cpu.flags = f;
	sift_cpu.detected = (f & avx512) == avx512 ? SIFT_SIMD_AVX512 :
		(f & avx2) == avx2 ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
#elif defined(SIFT_HAVE_NEON)
	sift_cpu.flags = SIFT_CPU_NEON;
	sift_cpu.detected = SIFT_SIMD_NEON;
#endif
	sift_cpu.active = sift_cpu.detected;
}

__attribute__((constructor)) static void
detect_at_load(void)
{
	detect();
}

#ifndef SIFT_CPUSHIM_DETECT_ONLY
void
kt_cpu_init(void)
{
	detect();
	sift_kernels_init();
}
#endif
