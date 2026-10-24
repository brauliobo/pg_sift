/* Stand-ins for the backend-side cpu module (same detection rules as src/core/cpu.c). */
#include "harness.h"

#include "bm25/bitmap_kern.h"

SiftCpu		sift_cpu;

/* The bitmap kernels reach their own table only through count_mask, which no tool calls */
const sift_kernels *sift_k = &sift_kern_scalar;
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

void
kt_cpu_init(void)
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
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") ? SIFT_CPU_AVX512 : 0;
	f |= __builtin_cpu_supports("avx512vpopcntdq") ? SIFT_CPU_AVX512_VPOPCNT : 0;
	f |= __builtin_cpu_supports("avx512vbmi2") ? SIFT_CPU_AVX512_VBMI2 : 0;
	sift_cpu.flags = f;
	sift_cpu.detected = (f & avx512) == avx512 ? SIFT_SIMD_AVX512 :
		(f & avx2) == avx2 ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
#elif defined(SIFT_HAVE_NEON)
	sift_cpu.flags = SIFT_CPU_NEON;
	sift_cpu.detected = SIFT_SIMD_NEON;
#endif
	sift_cpu.active = sift_cpu.detected;
	sift_kernels_init();
}
