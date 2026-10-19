/*
 * sift_aggkern.h - the vectorized operators of SiftAgg (docs/KERNELS.md pattern).
 *
 * One dispatch table per SIMD level, selected through sift_cpu_register() at run time from the CPU and the sift.simd
 * setting, never at build time. The scalar table (src/agg/sift_aggkern.c) is the specification: every other level must
 * return bit-identical results, including the float sums (their order is fixed below, independent of the level, so a
 * replica with another CPU answers the same) and the slots they leave unchanged. sift_aggkern_for(level) returns a
 * level's table for tests (never NULL: a level the CPU lacks falls back downwards).
 *
 * Only what is specific to the aggregation executor lives here. Every generic operation on bit vectors, selections and
 * integer columns is a slot of the column kernel table (sift_col_kern.h, sift_colk): range tests (the integer comparisons
 * and BETWEEN, the null flags: b != 0 is the u8 range [1, 255]), dictionary code lookups (lut), and / or / andnot / count,
 * bits_to_idx (the selection vector of a mask) and gather_u32 (and the 8-byte gather, for_i64_sel[SIFT_COLW_64] with base 0).
 *
 * Data model of the executor: a vector is at most SIFT_AGG_VEC rows (or array elements) of one column, decoded to one
 * of four element types: int32 (int2, int4, date, bool), int64 (int8, timestamp, timestamptz), double (float4 widened,
 * float8) and uint32 (dictionary code). Row sets are bitmaps of uint64 words, bit i of word i / 64 = row i; every
 * kernel of this table that writes a bitmap writes ceil(n / 64) whole words and leaves the bits >= n zero; every
 * kernel that reads one expects the bits >= n to be zero. The column kernels write whole SIFT_COL_SELWORDS groups of
 * words (SIFT_COL_NWORDS(n), the bits >= n zero too): the executor's bitmaps have SIFT_AGG_PADW words of slack for it.
 * NULLs are handled by the caller (a predicate result is ANDed with the not-null bitmap, NULL rows are cleared from the
 * mask before a reduction), so kernels never see them.
 *
 * Padding: input arrays are SIFT_SIMD_ALIGN aligned and the caller guarantees SIFT_AGG_PAD readable bytes after the last
 * element (contents irrelevant), so a level may load whole registers at the tail. This holds for every input array,
 * lookup tables (lut, map) and bitmaps included: a gather of a byte-indexed LUT reads a dword. Outputs are written exactly
 * (no overrun).
 *
 * No allocation, no ereport, no state; the only globals are the dispatch pointers and const tables. Portable file rules
 * (sift_host.h only) apply to the kernel files.
 */
#ifndef SIFT_AGGKERN_H
#define SIFT_AGGKERN_H

#include "sift_host.h"

#include "sift.h"
#include "sift_cpu.h"

#define SIFT_AGG_VEC		4096
#define SIFT_AGG_VECW		(SIFT_AGG_VEC / 64)
#define SIFT_AGG_PAD		64
#define SIFT_AGG_PADW		8		/* words of slack after a vector's bitmap: SIFT_COL_SELWORDS */

typedef enum SiftCmpOp
{
	SCMP_EQ,
	SCMP_NE,
	SCMP_LT,
	SCMP_LE,
	SCMP_GT,
	SCMP_GE
} SiftCmpOp;

/*
 * PostgreSQL float8 order everywhere a double is compared or ordered: NaN equals NaN and is greater than every other
 * value, -0 equals +0. min/max results normalize a zero to +0 (documented deviation from the sequential first-seen
 * rule of PostgreSQL's float8smaller/larger, which only shows for a column holding both signs of zero). A NaN result
 * (sum_f64, minmax_f64) is always the canonical quiet NaN: the payload an add or a compare would keep depends on the operand
 * order, which no level can promise.
 */
typedef struct SiftAggKernels
{
	/* ---- predicates: bit i of out = (v[i] op c) ---- */
	/* doubles in PostgreSQL's order, not the integer keys of the column kernels */
	void		(*cmp_f64) (uint64 *out, const double *v, int n, SiftCmpOp op, double c);
	/* bit i = (lo <= v[i] <= hi); lo > hi selects nothing */
	void		(*between_f64) (uint64 *out, const double *v, int n, double lo, double hi);
	/* bit i = (uint32) (v[i] - base) < nlut && lut[v[i] - base] != 0 (wrapping subtraction in uint32); a bound check fused with a byte LUT */
	void		(*lut_i32) (uint64 *out, const int32 *v, int n, int32 base, uint32 nlut, const uint8 *lut);

	/* bit e of out (m bits) = bit parent[e] of rowmask: the row mask of the elements of an array column (executor specific) */
	void		(*gather_bits) (uint64 *out, const uint64 *rowmask, const uint32 *parent, int m);

	/* ---- group ids (dense mixed radix): gid[i] += map[key(i)] * mult ---- */
	void		(*gid_code) (uint32 *gid, const uint32 *code, const uint32 *map, int n, uint32 mult);
	/* key = v[i] - base, in [0, size of map) by the caller's contract */
	void		(*gid_i32) (uint32 *gid, const int32 *v, int32 base, const uint32 *map, int n, uint32 mult);

	/*
	 * ---- reductions over the rows of mask (mask == NULL: all n rows) ----
	 * Integer sums are exact (the caller bounds n so that no overflow happens in int64: |v| < 2^31, n <= SIFT_AGG_VEC).
	 * sum_f64 has a fixed order for every level: eight accumulators a[0..7] start at +0.0; row i (selected) is added to
	 * a[i % 8] in row order; the result is ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7)). Unselected rows add
	 * nothing (they are skipped, not added as 0.0).
	 */
	int64		(*sum_i32) (const int32 *v, const uint64 *mask, int n);
	double		(*sum_f64) (const double *v, const uint64 *mask, int n);
	/*
	 * The largest absolute value of the selected rows, 0 when none is: true with *mx set, or false (*mx untouched) when a selected
	 * row is not finite (NaN, +-Inf). The float guard of a sum: only a finite input may overflow it.
	 */
	bool		(*absmax_f64) (const double *v, const uint64 *mask, int n, double *mx);
	/* false (outputs untouched) when no row is selected */
	bool		(*minmax_i32) (int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n);
	bool		(*minmax_i64) (int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n);
	bool		(*minmax_f64) (double *mn, double *mx, const double *v, const uint64 *mask, int n);

	/*
	 * ---- per group accumulation over the rows of mask (NULL: all n rows) ----
	 * Rows are applied in row order, so a group's float sum is the sequential sum of its rows; every level may share the
	 * scalar function here (the order, not the speed, is what is promised).
	 */
	void		(*count_gid) (int64 *cnt, const uint32 *gid, const uint64 *mask, int n);		/* cnt[gid[i]] += 1 */
	void		(*sum_gid_i32) (int64 *acc, const uint32 *gid, const int32 *v, const uint64 *mask, int n);
	void		(*sum_gid_f64) (double *acc, const uint32 *gid, const double *v, const uint64 *mask, int n);

	/*
	 * Adjacent distinct values: the number of selected rows whose value differs from the previous selected row's, where
	 * the row before the first one of the first call is the value *prev (the caller starts it at a value the data
	 * never holds, UINT32_MAX); *prev is left at the last selected value (unchanged when none is selected).
	 */
	int64		(*count_changes) (const uint32 *v, const uint64 *mask, int n, uint32 *prev);
} SiftAggKernels;

/* Active table; never NULL after _PG_init */
extern PGDLLIMPORT const SiftAggKernels *sift_aggk;

/* Per-level tables, for equivalence tests; a level the CPU lacks falls back to the next lower one, never NULL */
extern const SiftAggKernels *sift_aggkern_for(SiftSimdLevel level);

/* The scalar table, the specification */
extern const SiftAggKernels sift_aggkern_scalar;

/* Registers the dispatch rebind (sift_cpu_register); called from _PG_init */
extern void sift_aggkern_init(void);

#endif							/* SIFT_AGGKERN_H */
