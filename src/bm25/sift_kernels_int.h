/*
 * sift_kernels_int.h - shared between the per-level kernel files.
 */
#ifndef SIFT_KERNELS_INT_H
#define SIFT_KERNELS_INT_H

#include "sift_kernels.h"

extern const SiftKernels sift_kernels_scalar;
extern const SiftKernels sift_kernels_avx2;
extern const SiftKernels sift_kernels_avx2_nopdep;	/* microcoded PDEP/PEXT */
extern const SiftKernels sift_kernels_avx512;

extern int	sift_kern_expand_scalar(uint64 *out, const uint64 *mask, const uint64 *base, const uint64 *rankbits,
									uint32 pos, int nwords);
extern void sift_kern_vm_scalar(uint64 *pagemask, const uint8 *vmbytes);
extern int	sift_kern_expand_pdep(uint64 *out, const uint64 *mask, const uint64 *base, const uint64 *rankbits,
								  uint32 pos, int nwords);
extern void sift_kern_vm_pext(uint64 *pagemask, const uint8 *vmbytes);

/* Row k: for each set bit of k, its rank among the set bits; 0x80 (PSHUFB zero) if clear */
extern const uint8 sift_kern_lut_expand[256][8];

#define SIFT_KERN_WORDS(npages)	((npages) * SIFT_OFFMASK_WORDS)

/* tf of an escape byte, saturating so corrupt bytes never yield tf 0 */
static inline uint8
sift_kern_esc_tf(uint8 e)
{
	return e > 255 - 17 ? 255 : e + 17;
}

static inline uint64
sift_kern_ld64(const void *p)
{
	uint64		x;

	memcpy(&x, p, sizeof(x));
	return x;
}

/* 64 bits of a bit stream starting at bit pos; reads 9 bytes */
static inline uint64
sift_kern_bits64(const uint8 *p, uint64 pos)
{
	const uint8 *q = p + (pos >> 3);
	uint32		s = pos & 7;

	return (sift_kern_ld64(q) >> s) | (((uint64) q[8] << 1) << (63 - s));
}

/* Bits [pos, pos + n) of a word array, n <= 64; reads the next word only if needed */
static inline uint64
sift_kern_rank_bits(const uint64 *rb, uint32 pos, int n)
{
	uint32		w = pos >> 6,
				s = pos & 63;
	uint64		x;

	if (n == 0)
		return 0;
	x = rb[w] >> s;
	if (s && s + n > 64)
		x |= rb[w + 1] << (64 - s);
	return x;
}

#endif							/* SIFT_KERNELS_INT_H */
