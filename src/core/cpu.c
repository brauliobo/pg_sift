#include "core/host.h"

#include "core/cpu.h"

#if defined(__x86_64__)
#include <cpuid.h>
#elif defined(SIFT_HAVE_NEON) && defined(__linux__)
#include <sys/auxv.h>
#endif

/* One rebind per dispatch table: BM25 kernels, bitmap codec, column kernels, shredder, aggregation, chunk codec (6 today) */
#define SIFT_MAX_REBINDS 16

SiftCpu		sift_cpu;

static SiftDispatchRebind rebinds[SIFT_MAX_REBINDS];
static int	nrebinds;

/* The names of the levels of this architecture, and the caps: a name of the other architecture is the top level, i.e. no cap */
#define SIFT_SIMD_AUTO (SIFT_SIMD_TOP + 1)
#if !defined(SIFT_HAVE_NEON)
static const char *const level_names[] = {"scalar", "avx2", "avx512"};
static const SiftEnumEntry simd_options[] = {
	{"scalar", SIFT_SIMD_SCALAR, false},
	{"avx2", SIFT_SIMD_AVX2, false},
	{"avx512", SIFT_SIMD_TOP, false},
	{"neon", SIFT_SIMD_TOP, true},
	{"auto", SIFT_SIMD_AUTO, false},
	{NULL, 0, false}
};
#else
static const char *const level_names[] = {"scalar", "neon"};
static const SiftEnumEntry simd_options[] = {
	{"scalar", SIFT_SIMD_SCALAR, false},
	{"neon", SIFT_SIMD_NEON, false},
	{"avx2", SIFT_SIMD_TOP, true},
	{"avx512", SIFT_SIMD_TOP, true},
	{"auto", SIFT_SIMD_AUTO, false},
	{NULL, 0, false}
};
#endif

static int	simd_guc = SIFT_SIMD_AUTO;

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

/* Whether the CPU has x86-64-v3 or v4, the levels of the target attributes of core/fmv.h, by the features that define them */
static bool
x86_level_supported(int level)
{
	bool		v3 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2") &&
		__builtin_cpu_supports("fma") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("f16c") &&
		__builtin_cpu_supports("movbe");

	if (level == 3)
		return v3;
	return v3 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512cd") &&
		__builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512vl");
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
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512cd"))
		f |= SIFT_CPU_AVX512;
	if (__builtin_cpu_supports("avx512vpopcntdq"))
		f |= SIFT_CPU_AVX512_VPOPCNT;
	if (__builtin_cpu_supports("avx512vbmi2"))
		f |= SIFT_CPU_AVX512_VBMI2;
	if (__builtin_cpu_supports("avx512vbmi"))
		f |= SIFT_CPU_AVX512_VBMI;
	if (x86_level_supported(3))
		f |= SIFT_CPU_X86_V3;
	if (x86_level_supported(4))
		f |= SIFT_CPU_X86_V4;
	return f;
}
#elif defined(SIFT_HAVE_NEON) && defined(__linux__)
/* The Linux AArch64 ABI numbers of the capability bits (<asm/hwcap.h>, which an old sysroot may not have) */
#define SIFT_HWCAP_CRC32	(1UL << 7)
#define SIFT_HWCAP_ASIMDDP	(1UL << 20)
#define SIFT_HWCAP_SVE		(1UL << 22)
#define SIFT_HWCAP2_SVE2	(1UL << 1)

/* Advanced SIMD is part of every AArch64 CPU; the extensions are what the kernel says the CPU has */
static uint32
detect_flags(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);
	uint32		f = SIFT_CPU_NEON;

	if (hwcap & SIFT_HWCAP_CRC32)
		f |= SIFT_CPU_ARM_CRC32;
	if (hwcap & SIFT_HWCAP_ASIMDDP)
		f |= SIFT_CPU_ARM_DOTPROD;
	if (hwcap & SIFT_HWCAP_SVE)
		f |= SIFT_CPU_ARM_SVE;
	if (hwcap2 & SIFT_HWCAP2_SVE2)
		f |= SIFT_CPU_ARM_SVE2;
	return f;
}
#elif defined(SIFT_HAVE_NEON) && defined(__APPLE__)
/* Every Apple silicon CPU (M1 and later) has CRC32 and the dot product, none has SVE */
static uint32
detect_flags(void)
{
	return SIFT_CPU_NEON | SIFT_CPU_ARM_CRC32 | SIFT_CPU_ARM_DOTPROD;
}
#elif defined(SIFT_HAVE_NEON)
static uint32
detect_flags(void)
{
	return SIFT_CPU_NEON;
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
#if defined(SIFT_HAVE_NEON)
	return (f & SIFT_CPU_NEON) ? SIFT_SIMD_NEON : SIFT_SIMD_SCALAR;
#else
	const uint32 avx2 = SIFT_CPU_AVX2 | SIFT_CPU_BMI2 | SIFT_CPU_POPCNT;
	const uint32 avx512 = avx2 | SIFT_CPU_AVX512 | SIFT_CPU_AVX512_VPOPCNT | SIFT_CPU_AVX512_VBMI | SIFT_CPU_AVX512_VBMI2;

	if ((f & avx512) == avx512)
		return SIFT_SIMD_AVX512;
	if ((f & avx2) == avx2)
		return SIFT_SIMD_AVX2;
	return SIFT_SIMD_SCALAR;
#endif
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
						  "auto uses the best level the CPU supports; avx2 and avx512 are the levels of x86-64, neon the level of AArch64, and a level of the other architecture caps nothing.",
						  &simd_guc, SIFT_SIMD_AUTO, simd_options, simd_assign);
	apply_level();
}

int
sift_cpu_parse_level(const char *name)
{
	if (name == NULL)
		return SIFT_SIMD_TOP;
	for (const SiftEnumEntry *e = simd_options; e->name != NULL; e++)
		if (strcmp(name, e->name) == 0)
			return Min(e->val, SIFT_SIMD_TOP);
	return -1;
}

void
sift_cpu_set_level(SiftSimdLevel level)
{
	simd_guc = level;
	apply_level();
}

void
sift_cpu_register(SiftDispatchRebind fn)
{
	SIFT_INVARIANT(nrebinds < SIFT_MAX_REBINDS, "sift: too many SIMD dispatch tables");
	rebinds[nrebinds++] = fn;
	fn(&sift_cpu);
}

const char *
sift_simd_level_name(SiftSimdLevel level)
{
	return level_names[Min(level, SIFT_SIMD_TOP)];
}
