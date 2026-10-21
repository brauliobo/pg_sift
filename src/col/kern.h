/*
 * col/kern.h - the kernels of the column index (docs/COL.md section 8): filters on chunks, selection vectors, zone-map checks,
 * dictionary LUTs and decoding, one dispatch table per SIMD level.
 *
 * The scalar table (col/kern.c) is the specification; the AVX2 and AVX-512 tables must give bit-identical results. Dispatch follows
 * sift.simd through the cpu module's rebind registry (sift_cpu_register), never the build flags. Kernels take plain arrays: no
 * Relation, no allocation, no error. A bit vector ("bits") is an array of uint64 words, bit i of the vector is word i / 64, bit i % 64;
 * a kernel that fills a bit vector of n bits (range, lut, zone_range) writes all its SIFT_COL_NWORDS(n) words, the bits past n zero; one
 * that updates it in place (zone_set, zone_notnull) touches only the bits below n. Arrays read by a kernel must have SIFT_COL_PAD
 * readable bytes past their last used byte (contents irrelevant); bit vectors are allocated in multiples of SIFT_COL_SELWORDS words.
 *
 * This is the one table of the generic operations on bit vectors, selections and integer columns: the aggregation executor
 * (sift_aggkern.h) and the JSON shredder (col/shred.h) call these slots instead of keeping their own.
 */
#ifndef SIFT_COL_KERN_H
#define SIFT_COL_KERN_H

#include "core/host.h"

#include "core/sift.h"
#include "core/cpu.h"

#define SIFT_COL_PAD			64
#define SIFT_COL_SELWORDS		8	/* bit vectors are padded to 512 bits */
#define SIFT_COL_IDXPAD			16	/* entries of room bits_to_idx needs after the last position it returns */

/* Words of a bit vector of n bits, padded */
#define SIFT_COL_NWORDS(n)		((((uint64) (n) + 511) / 512) * SIFT_COL_SELWORDS)

/* One bit of a bit vector of 64-bit words (the test of a NULL vector is false) */
static inline bool
sift_col_bit_test(const uint64 *bits, uint32 i)
{
	return bits != NULL && ((bits[i >> 6] >> (i & 63)) & 1) != 0;
}

static inline void
sift_col_bit_set(uint64 *bits, uint32 i)
{
	bits[i >> 6] |= UINT64CONST(1) << (i & 63);
}

static inline void
sift_col_bit_clear(uint64 *bits, uint32 i)
{
	bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
}

/* Width slots of the range / for_i64 arrays */
#define SIFT_COLW_8				0
#define SIFT_COLW_16			1
#define SIFT_COLW_32			2
#define SIFT_COLW_64			3

typedef struct SiftColKernels
{
	/*
	 * Range test: bit i of bits = ((uint64) a[i] - lo) <= span (unsigned, wrapping subtraction of the element width: lo and span are
	 * already reduced to the width, i.e. < 2^width), for i < n. a is an array of n unsigned values of the slot's width (u8, u16, u32,
	 * u64). Equality is span 0, "x >= c" span = max - c, and so on; the caller splits a test that does not fit one range.
	 */
	void		(*range[4]) (const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);

	/*
	 * Lookup test: bit i of bits = bit a[i] of lutbits (a bit set indexed by the value), for i < n. The u8 / u16 slots need 256 /
	 * 65536 bits; the u32 slot (dictionary codes) needs a lutbits that covers every code in a (the caller sizes it by the dictionary).
	 */
	void		(*lut[3]) (const void *a, uint32 n, const uint64 *lutbits, uint64 *bits);

	/* dst &= src, dst |= src, dst &= ~src over nwords words */
	void		(*bits_and) (uint64 *dst, const uint64 *src, uint32 nwords);
	void		(*bits_or) (uint64 *dst, const uint64 *src, uint32 nwords);
	void		(*bits_andnot) (uint64 *dst, const uint64 *src, uint32 nwords);

	/* popcount of a, and of a & b */
	uint64		(*bits_count) (const uint64 *a, uint32 nwords);
	uint64		(*bits_and_count) (const uint64 *a, const uint64 *b, uint32 nwords);

	/*
	 * The positions of the set bits of bits (nwords words), ascending, each plus base, into idx; returns their number (idx has room for it
	 * and SIFT_COL_IDXPAD more entries: a level stores whole vectors)
	 */
	uint32		(*bits_to_idx) (const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx);

	/*
	 * Zone maps (structure of arrays, one entry per unit of a segment): bit i of keep = !(mx[i] < lo || mn[i] > hi), i < n. Signed
	 * comparisons in the key domain. keep is overwritten.
	 */
	void		(*zone_range) (const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep);

	/* keep &= bit i is set when (set[i] & probe) != 0, for 256-bit sets (4 words); set has n entries */
	void		(*zone_set) (const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep);

	/* Bit i of keep cleared where nnull[i] == nvals[i] (a unit whose leaf is entirely NULL cannot satisfy a non-null predicate) */
	void		(*zone_notnull) (const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep);

	/*
	 * Decoding. unpack: n values of `bits` bits (1..31, LSB first, blocks of 64 values, docs/COL.md 2.2) into dst (u32). for_i64:
	 * dst[i] = base + (int64) a[i] for a unsigned array of the slot's width (wrapping). for_i64_sel: the same for the n positions
	 * idx[k] of a (gather), dst[k] (with base 0 and the 64-bit slot, the gather of 8-byte values). gather_u32: dst[k] = a[idx[k]] (u32 codes).
	 */
	void		(*unpack) (const uint8 *src, uint32 bits, uint32 n, uint32 *dst);
	void		(*for_i64[4]) (const void *a, uint32 n, int64 base, int64 *dst);
	void		(*for_i64_sel[4]) (const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst);
	void		(*gather_u32) (const uint32 *a, const uint32 *idx, uint32 n, uint32 *dst);

	/*
	 * Decoding to the consumer's vector: f8_from_keys turns the n order-preserving keys of floats (sift_col_f8_key) in a into the
	 * IEEE bits of the doubles, in place. bits_to_flags: flags[i] = bit i of bits as 0 or 1, i < n; it stores whole vectors, so
	 * flags has room for n rounded up to SIFT_COL_PAD bytes (the bytes past n are undefined); bits has the words of n bits.
	 */
	void		(*f8_from_keys) (int64 *a, uint32 n);
	void		(*bits_to_flags) (const uint64 *bits, uint32 n, uint8 *flags);

	/*
	 * The owner of each element of a list column: parent[i] = p for i in [end[p - 1], end[p]) (end[-1] = 0), end non-decreasing (the
	 * running ends of np lists). A list is stored a register at a time and the next list overwrites what the last one stored past
	 * its end, so parent has room for end[np - 1] entries and SIFT_COL_PAD bytes more (the entries past end[np - 1] are undefined).
	 */
	void		(*expand_parents) (const uint32 *end, uint32 np, uint32 *parent);
} SiftColKernels;

/* The active table (follows sift.simd), and a level's table for tests; never NULL, falls back downwards */
extern PGDLLIMPORT const SiftColKernels *sift_colk;
extern const SiftColKernels *sift_col_kernels_for(SiftSimdLevel level);

/* Registers the rebind with the cpu module; _PG_init */
extern void sift_col_kernels_init(void);

/* Bit i of bits = b[i] != 0, i < n (a null flag array): the u8 range test [1, 255] */
static inline void
sift_colk_nonzero(const uint8 *b, uint32 n, uint64 *bits)
{
	sift_colk->range[SIFT_COLW_8](b, n, 1, 254, bits);
}

#endif							/* SIFT_COL_KERN_H */
