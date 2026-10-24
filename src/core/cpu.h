/*
 * core/cpu.h - runtime CPU feature detection and SIMD dispatch selection.
 *
 * Kernels are compiled per function with SIFT_TARGET_* attributes; the
 * translation unit itself targets baseline x86-64. Modules owning dispatch
 * tables register a rebind callback that runs at load and whenever the
 * sift.simd GUC changes the active level.
 *
 * The levels are ranks within the architecture: scalar, then the vector levels in increasing order of what they ask of the CPU. x86-64 has two
 * (AVX2, AVX-512); AArch64 has one, NEON (Advanced SIMD), which every AArch64 CPU has and which needs no target attribute, so it takes the number of
 * the first level. sift_simd_level_name() names a level for the architecture the library is built for. sift.simd accepts the names of both
 * architectures, so that one configuration file serves a mixed fleet, and a name of the other architecture caps nothing. SVE is not used: Apple
 * silicon has none.
 */
#ifndef SIFT_CPU_H
#define SIFT_CPU_H

typedef enum SiftSimdLevel
{
	SIFT_SIMD_SCALAR = 0,
	SIFT_SIMD_AVX2,
	SIFT_SIMD_AVX512,
	SIFT_SIMD_NEON = SIFT_SIMD_AVX2
} SiftSimdLevel;

#define SIFT_CPU_POPCNT			0x0001
#define SIFT_CPU_BMI2			0x0002	/* PDEP/PEXT present */
#define SIFT_CPU_BMI2_FAST		0x0004	/* ... and not microcoded (not Zen1/2) */
#define SIFT_CPU_AVX2			0x0008
#define SIFT_CPU_AVX512			0x0010	/* F+BW+VL+DQ */
#define SIFT_CPU_AVX512_VPOPCNT 0x0020
#define SIFT_CPU_AVX512_VBMI2	0x0040
#define SIFT_CPU_NEON			0x0080

typedef struct SiftCpu
{
	uint32		flags;
	SiftSimdLevel detected;		/* best level the CPU and OS support */
	SiftSimdLevel active;		/* min(detected, sift.simd) */
} SiftCpu;

extern PGDLLIMPORT SiftCpu sift_cpu;

/* The NEON kernels are compiled in where the compiler targets AArch64 with Advanced SIMD, which is every AArch64 target but -mgeneral-regs-only */
#if defined(__aarch64__) && defined(__ARM_NEON)
#define SIFT_HAVE_NEON 1
#define SIFT_SIMD_TOP SIFT_SIMD_NEON
#else
#define SIFT_SIMD_TOP SIFT_SIMD_AVX512
#endif

#if defined(__x86_64__)
#define SIFT_TARGET_AVX2 \
	__attribute__((target("avx2,bmi,bmi2,popcnt")))
#define SIFT_TARGET_AVX512 \
	__attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt")))
#endif

typedef void (*SiftDispatchRebind) (const SiftCpu *cpu);

/* Called once from _PG_init: detects features and defines the sift.simd GUC */
extern void sift_cpu_init(void);

/* Registers fn and calls it immediately with the current state */
extern void sift_cpu_register(SiftDispatchRebind fn);

extern const char *sift_simd_level_name(SiftSimdLevel level);

#endif							/* SIFT_CPU_H */
