/* The cpu module of the extension (core/cpu.c) is not linked: the dispatched helpers of bitmap.c read sift_cpu, filled here from the hardware before main. */
#include "core/host.h"
#include "core/cpu.h"

SiftCpu		sift_cpu;

__attribute__((constructor)) static void
codec_cpu_init(void)
{
#if defined(__x86_64__)
	uint32		f = 0;

	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("popcnt"))
		f |= SIFT_CPU_X86_V3;
	if ((f & SIFT_CPU_X86_V3) && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512cd"))
		f |= SIFT_CPU_X86_V4;
	sift_cpu.flags = f;
	sift_cpu.detected = (f & SIFT_CPU_X86_V4) ? SIFT_SIMD_AVX512 : (f & SIFT_CPU_X86_V3) ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
#elif defined(SIFT_HAVE_NEON)
	sift_cpu.flags = SIFT_CPU_NEON;
	sift_cpu.detected = SIFT_SIMD_NEON;
#endif
	sift_cpu.active = sift_cpu.detected;
}
