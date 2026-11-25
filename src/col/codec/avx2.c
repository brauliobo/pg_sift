/*
 * col/codec/avx2.c - AVX2 kernels of the chunk codec (x86-64-v3 function-level target, safe to link on any x86-64), the width and the compare-to-bits primitive of col/codec/vec.h.
 */
#if defined(__x86_64__)

#include "col/codec/int.h"

#define SIFT_VEC_AVX2
#include "core/simd_isa.h"
SIFT_VEC_PUSH

#define CC_VB 32
#define CC_FN(x) cc_##x##_avx2
#define CC_MOVEMASK(m) (uint32) _mm256_movemask_ps((__m256) (m))
#include "col/codec/vec.h"

CC_TABLE(avx2);

SIFT_VEC_POP
#endif							/* __x86_64__ */
