/*
 * core/simd_isa.h - the instructions of a SIMD level that the GCC/Clang vector extensions cannot express, once for every kernel family that writes its
 * loops with the extensions (agg/kern_vec.h, col/shred_vec.h). The including file defines SIFT_VEC_AVX2, SIFT_VEC_AVX512 or SIFT_VEC_NEON, and brackets
 * the vector code with SIFT_VEC_PUSH / SIFT_VEC_POP (the target options of the level; NEON needs none). The operands are vectors of the level's width
 * of any element type: the macros cast them.
 *
 *   SIFT_V_BYTES              the width of a vector
 *   SIFT_V_BLEND(T, m, a, b)  m ? a : b per lane, m all ones or zero (not on AVX-512: the compilers use a mask register)
 *   SIFT_V_MIN_I32/MAX_I32    the minimum / maximum of int32 lanes (not on AVX-512)
 *   SIFT_V_MIN_F64/MAX_F64    VMINPD / VMAXPD: a NaN first operand yields the second (x86 only: NEON's FMIN propagates the NaN)
 *   SIFT_V_ANY(m)             a lane of m is not zero (one instruction: PTEST, UMAXV)
 *   SIFT_V_WIDEN(a, b)        the product of the low signed 32 bits of each int64 lane as a full lane
 *   SIFT_V_BITS32/BITS64(m)   the lane masks of an int32 / int64 compare as the bits of its lanes
 */
#ifndef SIFT_SIMD_ISA_H
#define SIFT_SIMD_ISA_H

#include "core/cpu.h"

#if defined(SIFT_VEC_AVX2) && defined(__x86_64__)
#include <immintrin.h>

#define SIFT_V_BYTES 32
#define SIFT_V_BLEND(T, m, a, b) ((T) _mm256_blendv_epi8((__m256i) (b), (__m256i) (a), (__m256i) (m)))
#define SIFT_V_MIN_I32(a, b) ((__typeof__(a)) _mm256_min_epi32((__m256i) (a), (__m256i) (b)))
#define SIFT_V_MAX_I32(a, b) ((__typeof__(a)) _mm256_max_epi32((__m256i) (a), (__m256i) (b)))
#define SIFT_V_MIN_F64(a, b) ((__typeof__(a)) _mm256_min_pd((__m256d) (a), (__m256d) (b)))
#define SIFT_V_MAX_F64(a, b) ((__typeof__(a)) _mm256_max_pd((__m256d) (a), (__m256d) (b)))
#define SIFT_V_ANY(m) (!_mm256_testz_si256((__m256i) (m), (__m256i) (m)))
#define SIFT_V_WIDEN(a, b) _mm256_mul_epi32((__m256i) (a), (__m256i) (b))
#define SIFT_V_BITS32(m) ((uint64) (uint32) _mm256_movemask_ps((__m256) (m)))
#define SIFT_V_BITS64(m) ((uint64) (uint32) _mm256_movemask_pd((__m256d) (m)))
#if defined(__clang__)
#define SIFT_VEC_PUSH _Pragma("clang attribute push(__attribute__((target(\"avx2,bmi,bmi2,popcnt\"))), apply_to = function)")
#define SIFT_VEC_POP _Pragma("clang attribute pop")
#else
#define SIFT_VEC_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"avx2,bmi,bmi2,popcnt\")")
#define SIFT_VEC_POP _Pragma("GCC pop_options")
#endif

#elif defined(SIFT_VEC_AVX512) && defined(__x86_64__)
#include <immintrin.h>

#define SIFT_V_BYTES 64
#define SIFT_V_WIDEN(a, b) _mm512_mul_epi32((__m512i) (a), (__m512i) (b))
#define SIFT_V_BITS32(m) ((uint64) _mm512_movepi32_mask((__m512i) (m)))
#define SIFT_V_BITS64(m) ((uint64) _mm512_movepi64_mask((__m512i) (m)))
#if defined(__clang__)
#define SIFT_VEC_PUSH _Pragma("clang attribute push(__attribute__((target(\"avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt\"))), apply_to = function)")
#define SIFT_VEC_POP _Pragma("clang attribute pop")
#else
#define SIFT_VEC_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt\")")
#define SIFT_VEC_POP _Pragma("GCC pop_options")
#endif

#elif defined(SIFT_VEC_NEON) && defined(SIFT_HAVE_NEON)
#include <arm_neon.h>

#define SIFT_V_BYTES 16
#define SIFT_V_BLEND(T, m, a, b) ((T) vbslq_u8((uint8x16_t) (m), (uint8x16_t) (a), (uint8x16_t) (b)))
#define SIFT_V_MIN_I32(a, b) ((__typeof__(a)) vminq_s32((int32x4_t) (a), (int32x4_t) (b)))
#define SIFT_V_MAX_I32(a, b) ((__typeof__(a)) vmaxq_s32((int32x4_t) (a), (int32x4_t) (b)))
#define SIFT_V_ANY(m) (vmaxvq_u32((uint32x4_t) (m)) != 0)
#define SIFT_V_WIDEN(a, b) vmull_s32(vmovn_s64((int64x2_t) (a)), vmovn_s64((int64x2_t) (b)))
/* the lane masks of four int32 as bits: AND with the weight of the lane, add across */
static inline uint64
sift_v_bits32_neon(uint32x4_t m)
{
	static const uint32 weight[4] = {1, 2, 4, 8};

	return vaddvq_u32(vandq_u32(m, vld1q_u32(weight)));
}
#define SIFT_V_BITS32(m) sift_v_bits32_neon((uint32x4_t) (m))
#define SIFT_V_BITS64(m) ((uint64) (vgetq_lane_u64((uint64x2_t) (m), 0) & 1) | ((vgetq_lane_u64((uint64x2_t) (m), 1) & 1) << 1))
#define SIFT_VEC_PUSH
#define SIFT_VEC_POP
#endif

#endif							/* SIFT_SIMD_ISA_H */
