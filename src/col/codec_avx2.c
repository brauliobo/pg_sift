/*
 * col/codec_avx2.c - AVX2 kernels of the chunk codec (x86-64-v3 function-level target, safe to link on any x86-64), the width and the compare-to-bits primitive of col/codec_vec.h.
 */
#if defined(__x86_64__)
#include "core/host.h"

#include "col/codec_int.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define CC_VB 32
#define CC_FN(x) cc_##x##_avx2
#define CC_MOVEMASK(m) (uint32) _mm256_movemask_ps((__m256) (m))
#include "col/codec_vec.h"

CC_TABLE(avx2);

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#endif							/* __x86_64__ */
