/*
 * agg/kern/int.h - shared between the per-level files of the 2048-row operators (agg/kern6*.c).
 */
#ifndef SIFT_AGGKERN6_INT_H
#define SIFT_AGGKERN6_INT_H

#include "agg/kern/kern.h"
#include "core/popcount.h"

extern const SiftAggKernelsV6 sift_aggkern6_avx2;
extern const SiftAggKernelsV6 sift_aggkern6_avx512;
/* the AVX-512 table with the grouped slots by VPCONFLICTD (agg/kern/avx512.h): not the table of the level, for tests and for a core whose scatter is fast */
extern const SiftAggKernelsV6 sift_aggkern6_avx512_conflict;
extern const SiftAggKernelsV6 sift_aggkern6_neon;
/* the NEON table with the vector kernels of the slots that the table of the level leaves scalar (agg/kern/neon.h): for tests and for a core where they win */
extern const SiftAggKernelsV6 sift_aggkern6_neon_vector;

/* The slots of the granule bitmap, the dictionary pair and the Bloom filter: one file defines them for every level (agg/kern/sbbf.c) */
#define AK6_ADDED_SLOTS(X) \
	X(mask_and_gran) X(dict_pair_gid) X(sbbf_probe_any) X(sbbf_probe_all) X(sbbf_probe_vec_hash) X(sbbf_probe_vec_i64)

/* Every slot of the table, in the order of the header */
#define AK6_SLOTS(X) \
	X(mask_andn_del) X(mask_and) X(mask_to_sel16) X(key_present_apply) \
	X(cmp_i32) X(cmp_i64) X(cmp_f64) X(cmp_i128) X(cmp2_i32) X(cmp2_i64) X(cmp2_f64) X(lut_u32) X(in_set_i64) X(cmp_strview_eq) \
	X(stream_run_ne_i64) \
	X(hash_i32) X(hash_i64) X(hash_i128) X(hash_strview) \
	X(sum_i32) X(sum_i64_checked) X(sum_i64_to_i128) X(sum_i128) X(sum_f64_fast) \
	X(minmax_i32) X(minmax_i64) X(minmax_i128) X(minmax_f64) \
	X(count_gid) X(sum_gid_i32) X(sum_gid_i64) X(sum_gid_i64_to_i128) X(sum_gid_i128) X(sum_gid_f64_fast) X(minmax_gid_i64) \
	X(minmax_gid_f64) X(lin_i64) X(mulw_i64) \
	AK6_ADDED_SLOTS(X)

/* The scalar function of every slot (agg/kern/kern.c, agg/kern/cmp.c) */
#define AK6_DECLARE(s)	extern __typeof__(*((SiftAggKernelsV6 *) 0)->s) ak6_##s##_scalar;
AK6_SLOTS(AK6_DECLARE)

/* The slots that the AVX2 and AVX-512 levels share with the scalar one: one body compiled for each ISA (agg/kern/kern.c, core/fmv.h) */
#define AK6_FMV_SLOTS(X) X(lin_i64) X(mulw_i64)
#define AK6_DECLARE_FMV(s)	extern __typeof__(*((SiftAggKernelsV6 *) 0)->s) ak6_##s;
AK6_FMV_SLOTS(AK6_DECLARE_FMV)
#define AK6_INIT_FMV(s)	.s = ak6_##s,

/* The function of a slot of a level whose files other than the table's define it: ak6_<slot>_<level> */
#define AK6_DECLARE_AT(s, lvl)	extern __typeof__(*((SiftAggKernelsV6 *) 0)->s) ak6_##s##_##lvl;
#define AK6_INIT_AT(s, lvl)	.s = ak6_##s##_##lvl,

/* The added slots of the vector levels, defined for all of them in agg/kern/sbbf.c */
#define AK6_DECLARE_ADDED(s)	AK6_DECLARE_AT(s, avx2) AK6_DECLARE_AT(s, avx512) AK6_DECLARE_AT(s, neon)
AK6_ADDED_SLOTS(AK6_DECLARE_ADDED)

typedef unsigned __int128 ak6_u128;

#define AK6_LOW(k)		((UINT64CONST(1) << (k)) - 1)
#define AK6_NW(n)		(((n) + 63) >> 6)
/* The valid bits of the last word of an n-bit bitmap */
#define AK6_TAIL(n)		(((n) & 63) ? AK6_LOW((n) & 63) : ~UINT64CONST(0))
#define AK6_ROWBIT(mask, i)	(((mask)[(i) >> 6] >> ((i) & 63)) & 1)

static inline ak6_u128
ak6_u128_of(SiftI128 x)
{
	return ((ak6_u128) (uint64) x.hi << 64) | x.lo;
}

static inline SiftI128
ak6_i128_of(ak6_u128 x)
{
	SiftI128	r = {(uint64) x, (int64) (uint64) (x >> 64)};

	return r;
}

/* The exact sum of the lanes of the carry-free split of int64 values (agk6 sum_i64_to_i128): a = the sum of the low 32 bits, b = of the high 32 bits (unsigned), c = the number of negative values */
static inline ak6_u128
ak6_split_total(uint64 a, uint64 b, uint64 c)
{
	return (ak6_u128) a + ((ak6_u128) b << 32) - ((ak6_u128) c << 64);
}

static inline double
ak6_canon_nan(double x)
{
	return x != x ? __builtin_nan("") : x;
}

/* The fixed reduction tree of the eight accumulators of sum_f64_fast */
static inline double
ak6_f64_tree(const double a[8], double init)
{
	return ak6_canon_nan(init + (((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]))));
}

/* The checked int64 sum from the exact one */
static inline bool
ak6_fits_i64(SiftI128 s, int64 *out)
{
	if (s.hi != ((int64) s.lo >> 63))
		return false;
	*out = (int64) s.lo;
	return true;
}

/* The row of the i-th active row */
#define ROW_OF_SEL(sel, i)	((sel) ? (sel)[i] : (i))

#define AK6_HASH_STEP(h, x, mix)	sift_aggk6_hash1((mix) ? (((h) << 5) | ((h) >> 59)) ^ (x) : (x))

/* The indexes of the set bits of a byte, 8 bytes a row, for the selection vector of the vector levels */
extern const uint8 ak6_byte_idx[256][8];

/* ---- the float8 order: an int64 key whose signed order is it (-0.0 is +0.0, every NaN the canonical quiet NaN, which is above +Infinity) ---- */

#define AK6_F8_NAN_BITS		UINT64CONST(0x7FF8000000000000)
#define AK6_F8_FLIP			INT64_MAX

static inline int64
ak6_f8_key(double x)
{
	uint64		b;

	if (x != x)
		b = AK6_F8_NAN_BITS;
	else if (x == 0)
		b = 0;
	else
		memcpy(&b, &x, 8);
	return (int64) b ^ (((int64) b >> 63) & AK6_F8_FLIP);
}

static inline double
ak6_f8_of_key(int64 k)
{
	uint64		b = (uint64) (k ^ ((k >> 63) & AK6_F8_FLIP));
	double		d;

	memcpy(&d, &b, 8);
	return d;
}

static inline int
ak6_f8_cmp(double a, double b)
{
	if (a != a)
		return b != b ? 0 : 1;
	if (b != b)
		return -1;
	return a < b ? -1 : a > b;
}

/* ---- predicates ---- */

static inline bool
ak6_op_holds(SiftCmpOp op, int c)
{
	switch (op)
	{
		case SCMP_EQ:
			return c == 0;
		case SCMP_NE:
			return c != 0;
		case SCMP_LT:
			return c < 0;
		case SCMP_LE:
			return c <= 0;
		case SCMP_GT:
			return c > 0;
		default:
			return c >= 0;
	}
}

/* The bits where op holds from the bits where a < b and a == b (full: the bits that exist) */
static inline uint64
ak6_cmp_sel(SiftCmpOp op, uint64 lt, uint64 eq, uint64 full)
{
	switch (op)
	{
		case SCMP_EQ:
			return eq;
		case SCMP_NE:
			return ~eq & full;
		case SCMP_LT:
			return lt;
		case SCMP_LE:
			return lt | eq;
		case SCMP_GT:
			return ~(lt | eq) & full;
		default:
			return ~lt & full;
	}
}

static inline int
ak6_i128_cmp(SiftI128 a, SiftI128 b)
{
	const __int128 x = (__int128) ak6_u128_of(a),
				y = (__int128) ak6_u128_of(b);

	return x < y ? -1 : x > y;
}

/* The views are equal: the same length and bytes. Their first 8 bytes (length, prefix) tell most pairs apart without a dereference */
static inline bool
ak6_sv_eq(const SiftStrView *a, const SiftStrView *b)
{
	uint64		ha,
				hb;

	memcpy(&ha, a, 8);
	memcpy(&hb, b, 8);
	if (ha != hb)
		return false;
	if (a->len <= 12)
		return a->u.offset == b->u.offset;
	return memcmp(a->u.ptr + 4, b->u.ptr + 4, a->len - 4) == 0;
}

static inline void
ak6_sv_halves(const SiftStrView *v, uint64 *hdr, uint64 *tail)
{
	memcpy(hdr, v, 8);
	memcpy(tail, (const char *) v + 8, 8);
}

/* The hash of a view, the specification of hash_strview */
static inline uint64
ak6_sv_hash(uint64 h, const SiftStrView *v, bool mix)
{
	uint64		hdr,
				tail;

	ak6_sv_halves(v, &hdr, &tail);
	if (v->len > 12)
		return AK6_HASH_STEP(h, sift_aggk6_hash_bytes(v->u.ptr, v->len), mix);
	return AK6_HASH_STEP(AK6_HASH_STEP(h, hdr, mix), tail, true);
}

/* The rows [lo, n) of the bitmap with any bit set (a reduction found nothing when none) */
static inline bool
ak6_any_row(const uint64 *mask, uint16 n)
{
	const int	nw = AK6_NW(n);

	if (!mask)
		return n > 0;
	for (int w = 0; w < nw; w++)
		if (mask[w] & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0)))
			return true;
	return false;
}

/* ---- the update of one group by one row, the specification of the grouped slots (the vector levels use it for the rows they do not vectorize) ---- */

static inline void
ak6_upd_sum_i32(int64 *acc, uint32 g, int32 v)
{
	acc[g] = (int64) ((uint64) acc[g] + (uint64) (int64) v);
}

static inline void
ak6_upd_sum_i64(int64 *acc, uint32 g, int64 v)
{
	acc[g] = (int64) ((uint64) acc[g] + (uint64) v);
}

static inline void
ak6_upd_sum_i64_i128(SiftI128 *acc, uint32 g, int64 v)
{
	acc[g] = ak6_i128_of(ak6_u128_of(acc[g]) + (ak6_u128) (__int128) v);
}

static inline void
ak6_upd_sum_i128(SiftI128 *acc, uint32 g, SiftI128 v)
{
	acc[g] = ak6_i128_of(ak6_u128_of(acc[g]) + ak6_u128_of(v));
}

static inline void
ak6_upd_minmax_i64(int64 *mn, int64 *mx, uint32 g, int64 v)
{
	mn[g] = Min(mn[g], v);
	mx[g] = Max(mx[g], v);
}

static inline void
ak6_upd_minmax_i64_pair(int64 *mn, int64 *mx, uint32 g, int64 lo, int64 hi)
{
	mn[g] = Min(mn[g], lo);
	mx[g] = Max(mx[g], hi);
}

/* a + b, a NaN as the canonical quiet NaN: the payload of the sum of two NaNs depends on the order the code adds in, which a compiler may swap */
static inline double
ak6_add_f64(double a, double b)
{
	const double s = a + b;

	return __builtin_expect(s != s, 0) ? __builtin_nan("") : s;
}

/* A double as the minimum or maximum has it: -0.0 as +0.0 and every NaN the canonical quiet NaN (what the key of ak6_f8_key gives back) */
static inline double
ak6_f8_norm(double x)
{
	return x != x ? __builtin_nan("") : x + 0.0;
}

/* mn[g] and mx[g] with v in the float8 order (NaN above everything, -0.0 = +0.0), by compares, not by keys: the same bits as the keys give */
static inline void
ak6_upd_minmax_f64(double *mn, double *mx, uint32 g, double v)
{
	const double nv = ak6_f8_norm(v),
				a = mn[g],
				b = mx[g];
	const bool	lt = nv < a || (a != a && nv == nv),
				gt = nv > b || (nv != nv && b == b);

	mn[g] = ak6_f8_norm(lt ? nv : a);
	mx[g] = ak6_f8_norm(gt ? nv : b);
}

#endif							/* SIFT_AGGKERN6_INT_H */
