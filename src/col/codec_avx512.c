/*
 * col/codec_avx512.c - AVX-512 kernels of the chunk codec (F, BW, VL, DQ + the options of SIFT_TARGET_AVX512), the width and the compare-to-bits primitive of col/codec_vec.h.
 */
#if defined(__x86_64__)
#include "core/host.h"

#include "col/codec_int.h"

#define SIFT_VEC_AVX512
#include "core/simd_isa.h"
SIFT_VEC_PUSH

#define CC_VB 64
#define CC_FN(x) cc_##x##_avx512
/* a compare into a mask register directly: the extensions give a vector, which GCC turns back into a mask */
#define CC_ROWMASK(v, span) \
	((uint32) _mm512_cmple_epu32_mask((__m512i) (v)[0], (__m512i) (span)) | (uint32) _mm512_cmple_epu32_mask((__m512i) (v)[1], (__m512i) (span)) << 16)
#include "col/codec_vec.h"

CC_TABLE(avx512);

SIFT_VEC_POP
#endif							/* __x86_64__ */
