/*
 * agg/kern6.h - the vectorized operators of the 2048-row engine (agg/vec.h).
 *
 * One dispatch table per SIMD level, bound through sift_cpu_register() at run time from the CPU and sift.simd. The scalar
 * table (agg/kern6.c, agg/kern6_cmp.c) is the specification: every other level returns bit-identical results, floats included (the order of
 * sum_f64_fast is fixed below, whatever the level), and every level implements every slot. sift_aggkern6_for(level) returns the table of a
 * level for tests, never NULL (a level the CPU lacks falls back downwards).
 *
 * Row sets are bitmaps of uint64 words (bit i of word i / 64 = row i); a kernel that writes one writes ceil(n / 64) whole words
 * with the bits >= n zero, one that reads one expects them zero. mask == NULL reads as all n rows. n <= SIFT_VEC_LEN. NULLs
 * are the caller's: a validity bitmap is ANDed into the mask before a kernel runs. Input arrays are SIFT_VEC_ALIGN aligned with
 * SIFT_VEC_PAD readable bytes after the last element, but a kernel reads no row >= n that it does not mask out, and writes
 * exactly its outputs, but for mask_to_sel16, whose output buffer has SIFT_SEL_SLACK entries past the last index.
 *
 * Grouped slots: row r of the i-th active row is sel[i] (sel != NULL) or i; gid and v are indexed by r, the outputs by gid[r].
 * Integer sums wrap modulo 2^64 (2^128 for SiftI128) unless a slot says it is checked.
 *
 * Doubles compare in the order of PostgreSQL float8: NaN equals NaN and is above +Infinity, -0.0 equals +0.0.
 *
 * No allocation, no ereport, no state; only the dispatch pointer and const tables are global.
 */
#ifndef SIFT_AGGKERN6_H
#define SIFT_AGGKERN6_H

#include "agg/vec.h"
#include "core/cpu.h"
#include "core/sbbf.h"

typedef enum SiftCmpOp
{
	SCMP_EQ = 0,
	SCMP_NE = 1,
	SCMP_LT = 2,
	SCMP_LE = 3,
	SCMP_GT = 4,
	SCMP_GE = 5
} SiftCmpOp;

/* the most values of an in_set_i64 set that the kernels keep in registers; a longer set is still exact */
#define SIFT_AK6_SET_REGS	16
/* the readable bytes after the last entry of a lut_u32 table */
#define SIFT_AK6_LUT_PAD	4

typedef struct SiftAggKernelsV6
{
	/* ---- bitmaps ---- */
	/* dst &= ~del over n bits; returns popcount(dst) */
	uint16		(*mask_andn_del) (uint64 *dst, const uint64 *del, uint16 n);
	/* dst = a & b over n bits; returns popcount(dst); dst may be a or b */
	uint16		(*mask_and) (uint64 *dst, const uint64 *a, const uint64 *b, uint16 n);
	/* the set bits of mask as strictly increasing uint16 indexes; returns their number */
	uint16		(*mask_to_sel16) (uint16 *sel, const uint64 *mask, uint16 n);
	/* dst &= kp (negate = false) or dst &= (~kp & doc_valid) (negate = true: doc_valid is read only then) over n bits; returns popcount(dst) */
	uint16		(*key_present_apply) (uint64 *dst, const uint64 *kp, const uint64 *doc_valid, uint16 n, bool negate);

	/* ---- predicates: out = ceil(n / 64) words, the bit of row i set when v[i] op c holds, the bits >= n zero ---- */
	void		(*cmp_i32) (uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c);
	void		(*cmp_i64) (uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c);
	void		(*cmp_f64) (uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c);
	void		(*cmp_i128) (uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c);
	/* the same between two arrays: the bit of row i is a[i] op b[i] */
	void		(*cmp2_i32) (uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op);
	void		(*cmp2_i64) (uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op);
	void		(*cmp2_f64) (uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op);
	/* bit i = (uint32) (v[i] - base) < nlut && lut[v[i] - base] != 0; lut has SIFT_AK6_LUT_PAD readable bytes after its last entry */
	void		(*lut_u32) (uint64 *out, const uint32 *v, uint16 n, uint32 base, uint32 nlut, const uint8 *lut);
	/* bit i = v[i] equals one of set[0..nset); the kernels keep up to SIFT_AK6_SET_REGS values in registers */
	void		(*in_set_i64) (uint64 *out, const int64 *v, uint16 n, const int64 *set, uint8 nset);
	/* bit i = the string v[i] equals target: the same length and bytes (views of more than 12 bytes hold a pointer, which is compared by content); the equality of a column of a deterministic collation only (sift_text_bytes_equal) */
	void		(*cmp_strview_eq) (uint64 *out, const SiftStrView *v, uint16 n, SiftStrView target);
	/*
	 * The run boundaries of a sorted column: bit i = i == 0 or v[i] differs from v[i - 1]; with valid (a bitmap, NULL = all valid) a NULL row differs from a
	 * valid one and equals another NULL row, whatever the values under the NULLs
	 */
	void		(*stream_run_ne_i64) (uint64 *out, const int64 *v, const uint64 *valid, uint16 n);

	/*
	 * ---- hashing: h[i] for the i-th active row (row sel[i], or i) ----
	 * hash = f(v); with mix, h[i] = f(rotl(h[i], 5) ^ v) so that the hash of a key of several columns is built column by column.
	 * hash_i32 hashes the sign-extended value: an int4 and an int8 key of one value agree. hash_i128 is the hash of the low half
	 * and then, mixed, of the high half; hash_strview of a view of at most 12 bytes the hash of its first and then, mixed, of its second
	 * 8 bytes, and of a longer one sift_aggk6_hash_bytes of its whole string.
	 */
	void		(*hash_i32) (uint64 *h, const int32 *v, const uint16 *sel, uint16 n, bool mix);
	void		(*hash_i64) (uint64 *h, const int64 *v, const uint16 *sel, uint16 n, bool mix);
	void		(*hash_i128) (uint64 *h, const SiftI128 *v, const uint16 *sel, uint16 n, bool mix);
	void		(*hash_strview) (uint64 *h, const SiftStrView *v, const uint16 *sel, uint16 n, bool mix);

	/* ---- reductions over the rows of mask ---- */
	int64		(*sum_i32) (const int32 *v, const uint64 *mask, uint16 n);
	/* false (*out untouched) when the exact sum does not fit int64: the caller promotes to sum_i64_to_i128 */
	bool		(*sum_i64_checked) (const int64 *v, const uint64 *mask, uint16 n, int64 *out);
	/* init + the exact sum, wrapping modulo 2^128 */
	SiftI128	(*sum_i64_to_i128) (const int64 *v, const uint64 *mask, uint16 n, SiftI128 init);
	SiftI128	(*sum_i128) (const SiftI128 *v, const uint64 *mask, uint16 n, SiftI128 init);
	/*
	 * init + the sum of the rows, in a fixed order: eight accumulators, row i adding to accumulator i % 8 (a masked-out row adds +0.0), then
	 * init + (((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7))). A NaN result is the canonical quiet NaN.
	 */
	double		(*sum_f64_fast) (const double *v, const uint64 *mask, uint16 n, double init);
	/* false (outputs untouched) when no row is selected; the doubles in the float8 order, -0.0 given as +0.0 and a NaN as the canonical quiet NaN */
	bool		(*minmax_i32) (int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, uint16 n);
	bool		(*minmax_i64) (int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, uint16 n);
	bool		(*minmax_i128) (SiftI128 *mn, SiftI128 *mx, const SiftI128 *v, const uint64 *mask, uint16 n);
	bool		(*minmax_f64) (double *mn, double *mx, const double *v, const uint64 *mask, uint16 n);

	/* ---- per group accumulation, in row order ---- */
	void		(*count_gid) (int64 *cnt, const uint32 *gid, const uint16 *sel, uint16 n);
	void		(*sum_gid_i32) (int64 *acc, const uint32 *gid, const int32 *v, const uint16 *sel, uint16 n);
	void		(*sum_gid_i64) (int64 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n);
	void		(*sum_gid_i64_to_i128) (SiftI128 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n);
	void		(*sum_gid_i128) (SiftI128 *acc, const uint32 *gid, const SiftI128 *v, const uint16 *sel, uint16 n);
	void		(*sum_gid_f64_fast) (double *acc, const uint32 *gid, const double *v, const uint16 *sel, uint16 n);
	void		(*minmax_gid_i64) (int64 *mn, int64 *mx, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n);
	/* mn[g] and mx[g] become the float8 minimum and maximum of themselves and the values, -0.0 as +0.0 and a NaN as the canonical quiet NaN */
	void		(*minmax_gid_f64) (double *mn, double *mx, const uint32 *gid, const double *v, const uint16 *sel, uint16 n);

	/* ---- numeric programs, modulo 2^64: dst[i] = a[i] * ka + b[i] * kb, dst[i] = a[i] * b[i]; dst may be an input ---- */
	void		(*lin_i64) (int64 *dst, const int64 *a, int32 ka, const int64 *b, int32 kb, uint16 n);
	void		(*mulw_i64) (int64 *dst, const int64 *a, const int64 *b, uint16 n);

	/* ---- the granule bitmap and the two key dictionary codes ---- */
	/* dst = a & b over n <= SIFT_GRAN_ROWS bits (a granule of 8192 rows, not a batch); returns popcount(dst); dst may be a or b */
	uint16		(*mask_and_gran) (uint64 *dst, const uint64 *a, const uint64 *b, uint16 n);
	/*
	 * gid[r] = code0 * dom1 + code1 (modulo 2^32) for the i-th active row r (sel[i], or i), where a code is c[r] or, when valid is not NULL and the bit
	 * of r is clear, the code of the NULL; gid is indexed by r like the grouped slots; dom1 is the size of the domain of the second key with its NULL
	 */
	void		(*dict_pair_gid) (uint32 *gid, const uint32 *c0, const uint64 *valid0, uint32 null0, const uint32 *c1, const uint64 *valid1,
								  uint32 null1, uint32 dom1, const uint16 *sel, uint16 n);

	/*
	 * ---- the split-block Bloom filter (core/sbbf.h), the one of the runtime join filter and the skipping indexes ----
	 * some / every one of the nhashes hashes is in the filter (none: false / true)
	 */
	bool		(*sbbf_probe_any) (const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes);
	bool		(*sbbf_probe_all) (const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes);
	/* io_mask &= the rows whose hash hashes[r] is in the filter (the bits >= n stay zero, a clear bit is not read); returns popcount(io_mask) */
	uint16		(*sbbf_probe_vec_hash) (const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n);
	/* the same for the rows whose key is in [min_key, max_key] and whose hash, hash_i64 of the key (no mix), is in the filter: no key outside the range is hashed */
	uint16		(*sbbf_probe_vec_i64) (const SiftSbbfBlock *blocks, uint32 nblocks, int64 min_key, int64 max_key, const int64 *keys, uint64 *io_mask,
									   uint16 n);
} SiftAggKernelsV6;

/* The rows of a granule (col/granule.h), the most a mask_and_gran takes */
#define SIFT_GRAN_ROWS		8192

extern PGDLLIMPORT const SiftAggKernelsV6 *sift_aggk6;

extern const SiftAggKernelsV6 sift_aggkern6_scalar;
extern const SiftAggKernelsV6 *sift_aggkern6_for(SiftSimdLevel level);

/* Registers the dispatch rebind (sift_cpu_register); idempotent */
extern void sift_aggkern6_init(void);

/* The hash of one value, the specification of hash_i64 */
static inline uint64
sift_aggk6_hash1(uint64 x)
{
	x = (x ^ (x >> 32)) * UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 29)) * UINT64CONST(0xBF58476D1CE4E5B9);
	return x ^ (x >> 32);
}

/* The hash of len bytes (a wide key, a string): the same bytes give the same hash at every level and on every host */
static inline uint64
sift_aggk6_hash_bytes(const void *p, uint32 len)
{
	const uint8 *b = p;
	uint64		h = sift_aggk6_hash1(len);

	for (; len >= 8; len -= 8, b += 8)
	{
		uint64		w;

		memcpy(&w, b, 8);
		h = sift_aggk6_hash1(h ^ w);
	}
	if (len > 0)
	{
		uint64		w = 0;

		memcpy(&w, b, len);
		h = sift_aggk6_hash1(h ^ w);
	}
	return h;
}

/* The bits of a hash: the first probe group (0..31), the salt (48..54), the radix partition (56..63, top radix_bits) */
#define SIFT_HASH_SALT(h)		((uint8) (0x80 | (((h) >> 48) & 0x7F)))
#define SIFT_HASH_RADIX(h, bits)	((uint32) ((h) >> (64 - (bits))))

#endif							/* SIFT_AGGKERN6_H */
