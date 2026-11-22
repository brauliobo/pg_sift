/*
 * core/fmv.h - function multi-versioning (FMV) of the hot leaf loops: one portable C body compiled for several instruction sets.
 *
 *   glibc Linux x86-64 (GCC 11, Clang 16): ELF IFUNC target_clones "default", "arch=x86-64-v3", "arch=x86-64-v4"
 *   glibc Linux AArch64 (GCC 14, the first with a function version dispatcher there, or a Clang whose probe in areas.mk found the resolver): "default", "sve", "sve2"
 *   musl, macOS, older compilers: the same body in explicit target() functions, picked through sift_cpu.flags (no IFUNC relocation,
 *     which musl refuses at dlopen)
 *   SIMD=none, SIFT_FMV=off, the sanitizers (SIFT_FMV_DISABLE): the baseline body only
 *
 * The entry point checks sift_cpu.active, so sift.simd = 'scalar' and SIFT_SIMD=scalar run the baseline body on every platform.
 *
 * What a body may be:
 *   1. Plain C: no #ifdef __AVX2__, __AVX512F__ or __POPCNT__ (inside a clone they name the baseline of the file, not the clone).
 *   2. sift_pc64() for bit counts, never __builtin_popcountll (the baseline clone would call libgcc's __popcountdi2).
 *   3. SIFT_RESTRICT on pointers that do not alias, or GCC leaves the vector clones scalar; an inner loop reads a block through a pointer to its start (block[j]) or an unsigned index,
 *      since under -fwrapv a signed s[p + j] keeps the loop scalar.
 *   4. Floating point: one summation order for all clones (-ffp-contract=off is global), no fast-math.
 *   5. Static (or hidden) functions only: no exported IFUNC symbol.
 */
#ifndef SIFT_CORE_FMV_H
#define SIFT_CORE_FMV_H

#include "core/host.h"

#include "core/cpu.h"
#include "core/popcount.h"

#define SIFT_RESTRICT		restrict
#define SIFT_FMV_HIDDEN		__attribute__((visibility("hidden")))
#define SIFT_FMV_NOINLINE	__attribute__((noinline))

#if defined(__has_feature)
#define SIFT_HAS_FEAT(x) __has_feature(x)
#else
#define SIFT_HAS_FEAT(x) 0
#endif

/* Before a loop whose rows are independent although the pointers may be the same (dst = a: a row reads what it writes): the vectorizer then skips the overlap check
 * that an equal pointer fails, which is the check that would send an in-place call down the scalar loop */
#if defined(__clang__)
#define SIFT_IVDEP _Pragma("clang loop vectorize(assume_safety)")
#else
#define SIFT_IVDEP _Pragma("GCC ivdep")
#endif

#if defined(SQ_SIMD_NONE) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
	SIFT_HAS_FEAT(address_sanitizer) || SIFT_HAS_FEAT(thread_sanitizer)
#ifndef SIFT_FMV_DISABLE
#define SIFT_FMV_DISABLE 1
#endif
#endif

/* GCC's -O2 leaves widening reductions and pointer loops of the vector clones scalar; Clang vectorizes at -O2 and warns on the attribute */
#if defined(__GNUC__) && !defined(__clang__)
#define SIFT_FMV_OPT __attribute__((optimize("O3")))
#else
#define SIFT_FMV_OPT
#endif

/* ELF IFUNC target_clones: glibc only (musl refuses R_*_IRELATIVE at dlopen, Mach-O has none). SIFT_FMV_TARGET_ONLY forces the target() path, to test it on glibc. */
#if !defined(SIFT_FMV_DISABLE) && !defined(SIFT_FMV_TARGET_ONLY) && defined(__linux__) && defined(__GLIBC__) && !defined(__APPLE__)
#if defined(__x86_64__)
#if (defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11) || (defined(__clang__) && __clang_major__ >= 16)
#define SIFT_HAVE_IFUNC_FMV_X86 1
#endif
#elif defined(__aarch64__)
/* GCC 13 has no function version dispatcher on aarch64 (target_clones is an error there); Clang on aarch64-linux-gnu drops target_clones without a libgcc of GCC 14 or compiler-rt:
 * areas.mk probes the resolver and defines SIFT_CLANG_AARCH64_FMV */
#if (defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14) || (defined(__clang__) && defined(SIFT_CLANG_AARCH64_FMV))
#define SIFT_HAVE_IFUNC_FMV_ARM64 1
#endif
#endif
#endif

#ifndef SIFT_HAVE_IFUNC_FMV_X86
#define SIFT_HAVE_IFUNC_FMV_X86 0
#endif
#ifndef SIFT_HAVE_IFUNC_FMV_ARM64
#define SIFT_HAVE_IFUNC_FMV_ARM64 0
#endif
#define SIFT_HAVE_IFUNC_FMV (SIFT_HAVE_IFUNC_FMV_X86 || SIFT_HAVE_IFUNC_FMV_ARM64)

/* The explicit target() clones, where there is no IFUNC */
#if !defined(SIFT_FMV_DISABLE) && defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define SIFT_HAVE_TARGET_X86 1
#define SIFT_TARGET_X86_V3 __attribute__((target("arch=x86-64-v3"))) SIFT_FMV_OPT
#define SIFT_TARGET_X86_V4 __attribute__((target("arch=x86-64-v4"))) SIFT_FMV_OPT
#else
#define SIFT_HAVE_TARGET_X86 0
#endif

#if !defined(SIFT_FMV_DISABLE) && defined(__aarch64__) && !defined(__APPLE__) && \
	((defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11) || (defined(__clang__) && __clang_major__ >= 14))
#define SIFT_HAVE_TARGET_ARM64_EXT 1
#define SIFT_TARGET_ARM64_SVE  __attribute__((target("+sve"))) SIFT_FMV_OPT
#define SIFT_TARGET_ARM64_SVE2 __attribute__((target("+sve2"))) SIFT_FMV_OPT
#else
#define SIFT_HAVE_TARGET_ARM64_EXT 0
#endif

/* Clang refuses noinline on a multiversioned function (every call goes through the resolver anyway) */
#if defined(__clang__)
#define SIFT_FMV_CLONES_NOINLINE
#else
#define SIFT_FMV_CLONES_NOINLINE SIFT_FMV_NOINLINE
#endif

#if SIFT_HAVE_IFUNC_FMV_X86
#define SIFT_FMV_CLONES SIFT_FMV_CLONES_NOINLINE __attribute__((target_clones("default", "arch=x86-64-v3", "arch=x86-64-v4"))) SIFT_FMV_OPT
#elif SIFT_HAVE_IFUNC_FMV_ARM64
#define SIFT_FMV_CLONES SIFT_FMV_CLONES_NOINLINE __attribute__((target_clones("default", "sve", "sve2"))) SIFT_FMV_OPT
#endif

/*
 * SIFT_FMV_DEFINE(LINKAGE, RET, NAME, PARAMS, ARGS, BODY) defines NAME(PARAMS) from one BODY (a compound statement; its commas are
 * part of the variadic argument). PARAMS and ARGS are parenthesized lists. NAME_scalar is the baseline body, always there.
 */
#if SIFT_HAVE_IFUNC_FMV

#define SIFT_FMV_DEFINE(LINKAGE, RET, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE RET NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_CLONES RET NAME##_fmv PARAMS __VA_ARGS__							\
	LINKAGE RET NAME PARAMS																\
	{																					\
		if (__builtin_expect(sift_cpu.active == SIFT_SIMD_SCALAR, 0))					\
			return NAME##_scalar ARGS;													\
		return NAME##_fmv ARGS;															\
	}

#define SIFT_FMV_DEFINE_VOID(LINKAGE, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE void NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_CLONES void NAME##_fmv PARAMS __VA_ARGS__							\
	LINKAGE void NAME PARAMS															\
	{																					\
		if (__builtin_expect(sift_cpu.active == SIFT_SIMD_SCALAR, 0))					\
			NAME##_scalar ARGS;															\
		else																			\
			NAME##_fmv ARGS;															\
	}

#elif SIFT_HAVE_TARGET_X86

#define SIFT_FMV_DEFINE(LINKAGE, RET, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE RET NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_NOINLINE SIFT_TARGET_X86_V3 RET NAME##_x86_v3 PARAMS __VA_ARGS__	\
	static SIFT_FMV_NOINLINE SIFT_TARGET_X86_V4 RET NAME##_x86_v4 PARAMS __VA_ARGS__	\
	LINKAGE RET NAME PARAMS																\
	{																					\
		if (sift_cpu.active >= SIFT_SIMD_AVX512 && (sift_cpu.flags & SIFT_CPU_X86_V4))	\
			return NAME##_x86_v4 ARGS;													\
		if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_X86_V3))	\
			return NAME##_x86_v3 ARGS;													\
		return NAME##_scalar ARGS;														\
	}

#define SIFT_FMV_DEFINE_VOID(LINKAGE, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE void NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_NOINLINE SIFT_TARGET_X86_V3 void NAME##_x86_v3 PARAMS __VA_ARGS__	\
	static SIFT_FMV_NOINLINE SIFT_TARGET_X86_V4 void NAME##_x86_v4 PARAMS __VA_ARGS__	\
	LINKAGE void NAME PARAMS															\
	{																					\
		if (sift_cpu.active >= SIFT_SIMD_AVX512 && (sift_cpu.flags & SIFT_CPU_X86_V4))	\
			NAME##_x86_v4 ARGS;															\
		else if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_X86_V3)) \
			NAME##_x86_v3 ARGS;															\
		else																			\
			NAME##_scalar ARGS;															\
	}

#elif SIFT_HAVE_TARGET_ARM64_EXT

#define SIFT_FMV_DEFINE(LINKAGE, RET, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE RET NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_NOINLINE SIFT_TARGET_ARM64_SVE RET NAME##_arm_sve PARAMS __VA_ARGS__ \
	static SIFT_FMV_NOINLINE SIFT_TARGET_ARM64_SVE2 RET NAME##_arm_sve2 PARAMS __VA_ARGS__ \
	LINKAGE RET NAME PARAMS																\
	{																					\
		if (sift_cpu.active > SIFT_SIMD_SCALAR && (sift_cpu.flags & SIFT_CPU_ARM_SVE2))	\
			return NAME##_arm_sve2 ARGS;												\
		if (sift_cpu.active > SIFT_SIMD_SCALAR && (sift_cpu.flags & SIFT_CPU_ARM_SVE))	\
			return NAME##_arm_sve ARGS;													\
		return NAME##_scalar ARGS;														\
	}

#define SIFT_FMV_DEFINE_VOID(LINKAGE, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE void NAME##_scalar PARAMS __VA_ARGS__						\
	static SIFT_FMV_NOINLINE SIFT_TARGET_ARM64_SVE void NAME##_arm_sve PARAMS __VA_ARGS__ \
	static SIFT_FMV_NOINLINE SIFT_TARGET_ARM64_SVE2 void NAME##_arm_sve2 PARAMS __VA_ARGS__ \
	LINKAGE void NAME PARAMS															\
	{																					\
		if (sift_cpu.active > SIFT_SIMD_SCALAR && (sift_cpu.flags & SIFT_CPU_ARM_SVE2))	\
			NAME##_arm_sve2 ARGS;														\
		else if (sift_cpu.active > SIFT_SIMD_SCALAR && (sift_cpu.flags & SIFT_CPU_ARM_SVE)) \
			NAME##_arm_sve ARGS;														\
		else																			\
			NAME##_scalar ARGS;															\
	}

#else							/* SIMD=none, SIFT_FMV=off, a sanitizer, Apple silicon (NEON in the baseline) */

#define SIFT_FMV_DEFINE(LINKAGE, RET, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE RET NAME##_scalar PARAMS __VA_ARGS__						\
	LINKAGE RET NAME PARAMS { return NAME##_scalar ARGS; }

#define SIFT_FMV_DEFINE_VOID(LINKAGE, NAME, PARAMS, ARGS, ...)							\
	LINKAGE SIFT_FMV_NOINLINE void NAME##_scalar PARAMS __VA_ARGS__						\
	LINKAGE void NAME PARAMS { NAME##_scalar ARGS; }

#endif

#endif							/* SIFT_CORE_FMV_H */
