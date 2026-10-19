#include "sift_host.h"

#include "sift.h"
#include "sift_cpu.h"

#if defined(__x86_64__)
#include <cpuid.h>
#endif

/* One rebind per dispatch table: BM25 kernels, bitmap codec, column kernels, shredder, aggregation, chunk codec (6 today) */
#define SIFT_MAX_REBINDS 16

SiftCpu		sift_cpu;

static int	simd_guc = SIFT_SIMD_AVX512;
static SiftDispatchRebind rebinds[SIFT_MAX_REBINDS];
static int	nrebinds;

static const SiftEnumEntry simd_options[] = {
	{"scalar", SIFT_SIMD_SCALAR, false},
	{"avx2", SIFT_SIMD_AVX2, false},
	{"avx512", SIFT_SIMD_AVX512, false},
	{"auto", SIFT_SIMD_AVX512, false},
	{NULL, 0, false}
};

#if defined(__x86_64__)
/* PDEP/PEXT are microcoded (~250 cycles) on AMD family 0x17 (Zen1/Zen2) */
static bool
pdep_is_slow(void)
{
	unsigned int eax,
				ebx,
				ecx,
				edx;

	if (!__builtin_cpu_is("amd") || !__get_cpuid(1, &eax, &ebx, &ecx, &edx))
		return false;
	return ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF) == 0x17;
}

static uint32
detect_flags(void)
{
	uint32		f = 0;

	__builtin_cpu_init();
	if (__builtin_cpu_supports("popcnt"))
		f |= SIFT_CPU_POPCNT;
	if (__builtin_cpu_supports("bmi2"))
		f |= SIFT_CPU_BMI2 | (pdep_is_slow() ? 0 : SIFT_CPU_BMI2_FAST);
	if (__builtin_cpu_supports("avx2"))
		f |= SIFT_CPU_AVX2;
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq"))
		f |= SIFT_CPU_AVX512;
	if (__builtin_cpu_supports("avx512vpopcntdq"))
		f |= SIFT_CPU_AVX512_VPOPCNT;
	if (__builtin_cpu_supports("avx512vbmi2"))
		f |= SIFT_CPU_AVX512_VBMI2;
	return f;
}
#else
static uint32
detect_flags(void)
{
	return 0;
}
#endif

static SiftSimdLevel
level_from_flags(uint32 f)
{
	const uint32 avx2 = SIFT_CPU_AVX2 | SIFT_CPU_BMI2 | SIFT_CPU_POPCNT;
	const uint32 avx512 = avx2 | SIFT_CPU_AVX512 | SIFT_CPU_AVX512_VPOPCNT | SIFT_CPU_AVX512_VBMI2;

	if ((f & avx512) == avx512)
		return SIFT_SIMD_AVX512;
	if ((f & avx2) == avx2)
		return SIFT_SIMD_AVX2;
	return SIFT_SIMD_SCALAR;
}

static void
apply_level(void)
{
	sift_cpu.active = Min((SiftSimdLevel) simd_guc, sift_cpu.detected);
	for (int i = 0; i < nrebinds; i++)
		rebinds[i] (&sift_cpu);
}

static void
simd_assign(int newval, void *extra)
{
	simd_guc = newval;
	apply_level();
}

void
sift_cpu_init(void)
{
	sift_cpu.flags = detect_flags();
	sift_cpu.detected = level_from_flags(sift_cpu.flags);
	sift_host_enum_option("sift.simd", "Caps the SIMD level used by sift kernels.",
						  "auto uses the best level the CPU supports.",
						  &simd_guc, SIFT_SIMD_AVX512, simd_options, simd_assign);
	apply_level();
}

void
sift_cpu_register(SiftDispatchRebind fn)
{
	if (nrebinds == SIFT_MAX_REBINDS)
		elog(ERROR, "sift: too many SIMD dispatch tables");
	rebinds[nrebinds++] = fn;
	fn(&sift_cpu);
}

const char *
sift_simd_level_name(SiftSimdLevel level)
{
	return simd_options[level].name;
}
