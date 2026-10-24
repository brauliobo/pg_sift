/*
 * col/codec_neon.c - AArch64 Advanced SIMD kernels of the chunk codec. Bit-identical to the scalar table in col/codec_kern.c, which
 * lists the scalar kernels for the slots without a NEON kernel.
 */
#include "core/host.h"

#include "col/codec_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

#define CC_VB 16
#define CC_FN(x) cc_##x##_neon
#include "col/codec_vec.h"

/* ... exported for the NEON table of col/codec_kern.c, next to the scalar kernels it keeps for the other slots */
void
sift_cc_unpack32_neon(const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst)
{
	cc_unpack32_neon(src, W, rows, base, dst);
}

#endif
