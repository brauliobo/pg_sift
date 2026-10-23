/*
 * agg/kern_neon.c - AArch64 Advanced SIMD aggregation operators. Predicates compare the rows of a 64-row word and fold the compare
 * masks into its bits (core/neon.h); reductions turn the bits of the row mask into lane masks. Results are bit-identical to the
 * scalar table (agg/kern.c): float compares are mapped to the unordered predicates so that NaN is greater than everything. The slots
 * without a NEON kernel (lookups and per group accumulation are gathers and scatters, which NEON has no instruction for) are the
 * scalar functions.
 */
#include "core/host.h"

#include "agg/kern_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

static inline uint64x2_t
lanes2(uint32 b)
{
	static const uint64 bit[2] = {1, 2};

	return vtstq_u64(vdupq_n_u64(b), vld1q_u64(bit));
}

static inline uint32x4_t
lanes4(uint32 b)
{
	static const uint32 bit[4] = {1, 2, 4, 8};

	return vtstq_u32(vdupq_n_u32(b), vld1q_u32(bit));
}

/* The bits of the L rows from row i that are selected: the mask's, or all those below n */
static inline uint32
rowbits(const uint64 *mask, int i, int n, int L)
{
	return mask ? (uint32) (mask[i >> 6] >> (i & 63)) & (uint32) AGK_LOW(L) : (uint32) AGK_LANES(n - i, L);
}

/* Whether the mask selects a row below n (its bits from n on are zero) */
static inline bool
agk_any_row(const uint64 *mask, int n)
{
	for (int w = 0; w < AGK_NW(n); w++)
		if (mask[w])
			return true;
	return false;
}

/* ---- predicates ---- */

#define EQ_F8(x)	vceqq_f64(x, vc)
#define NE_F8(x)	vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(vceqq_f64(x, vc))))
#define LT_F8_(x)	vcltq_f64(x, vc)
#define LE_F8_(x)	vcleq_f64(x, vc)
#define GT_F8(x)	vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(vcleq_f64(x, vc))))
#define GE_F8(x)	vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(vcltq_f64(x, vc))))

/*
 * PostgreSQL float8 order against a constant that is not NaN: ordered predicates are false for a NaN row, the "not <=" and "not <"
 * ones are true for it (NaN is greater than everything). A NaN constant (rare) is the scalar function's.
 */
#define CMP_F8_CASE(OP) \
	case OP: \
		SIFT_COLK_WORDS_OF(double, v, n, out, CMP_WORD_##OP, vc); \
		break;

#define CMP_WORD(name, CMP) \
	static inline uint64 name(const double *p, float64x2_t vc) { return SIFT_NEON_WORD(64, vld1q_f64, p, CMP); }

CMP_WORD(CMP_WORD_SCMP_EQ, EQ_F8)
CMP_WORD(CMP_WORD_SCMP_NE, NE_F8)
CMP_WORD(CMP_WORD_SCMP_LT, LT_F8_)
CMP_WORD(CMP_WORD_SCMP_LE, LE_F8_)
CMP_WORD(CMP_WORD_SCMP_GT, GT_F8)
CMP_WORD(CMP_WORD_SCMP_GE, GE_F8)

static void
agk_cmp_f64_neon(uint64 *out, const double *v, int n, SiftCmpOp op, double c)
{
	const float64x2_t vc = vdupq_n_f64(c);

	if (c != c)
	{
		agk_cmp_f64_scalar(out, v, n, op, c);
		return;
	}
	switch (op)
	{
		CMP_F8_CASE(SCMP_EQ)
		CMP_F8_CASE(SCMP_NE)
		CMP_F8_CASE(SCMP_LT)
		CMP_F8_CASE(SCMP_LE)
		CMP_F8_CASE(SCMP_GT)
		CMP_F8_CASE(SCMP_GE)
	}
}

/* ---- reductions ---- */

/*
 * Reductions walk a full word of the mask in sixteen steps of four rows (two of two for the doubles), several accumulators wide, and
 * the rows past the last full word, or all rows when there is no mask, in steps that take their bits from rowbits(). A step with no
 * selected row costs the same as one with: a branch on it mispredicts more than it saves.
 */
#define MASKED_WORDS(mask, n, i, STEP) \
	for (; (mask) && (i) + 64 <= (n); (i) += 64) \
	{ \
		const uint64 m_ = (mask)[(i) >> 6]; \
		if (m_) \
		{ \
			AGK_UNROLL(16) \
			for (int g_ = 0; g_ < 16; g_++) \
				STEP((i) + 4 * g_, (uint32) (m_ >> (4 * g_)) & 15, g_ & 3); \
		} \
	}

static int64
agk_sum_i32_neon(const int32 *v, const uint64 *mask, int n)
{
	int64x2_t	acc[4] = {vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0)};
	int			i = 0;

	if (!mask)					/* the compiler vectorizes the plain sum as well */
		return agk_sum_i32_scalar(v, mask, n);
#define SUM_STEP(r, b, a) \
	acc[a] = vpadalq_s32(acc[a], vreinterpretq_s32_u32(vandq_u32(vreinterpretq_u32_s32(vld1q_s32(v + (r))), lanes4(b))))
	MASKED_WORDS(mask, n, i, SUM_STEP)
	for (; i < n; i += 4)
		SUM_STEP(i, rowbits(mask, i, n, 4), 0);
#undef SUM_STEP
	return vaddvq_s64(vaddq_s64(vaddq_s64(acc[0], acc[1]), vaddq_s64(acc[2], acc[3])));
}

/* Unselected lanes are ANDed to +0.0; a lane is finite when |x| <= DBL_MAX (false for NaN and Inf) */
static bool
agk_absmax_f64_neon(const double *v, const uint64 *mask, int n, double *mx)
{
	const float64x2_t big = vdupq_n_f64(DBL_MAX);
	float64x2_t m[4] = {vdupq_n_f64(0), vdupq_n_f64(0), vdupq_n_f64(0), vdupq_n_f64(0)};
	uint64x2_t	fin = vdupq_n_u64(~UINT64CONST(0));
	int			i = 0;

#define ABS_STEP2(r, b, a) \
	do { \
		const float64x2_t x_ = vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(vabsq_f64(vld1q_f64(v + (r)))), lanes2(b))); \
		fin = vandq_u64(fin, vcleq_f64(x_, big)); \
		m[a] = vmaxq_f64(m[a], x_); \
	} while (0)
#define ABS_STEP(r, b, a) do { ABS_STEP2(r, (b) & 3, a); ABS_STEP2((r) + 2, (b) >> 2, a); } while (0)
	MASKED_WORDS(mask, n, i, ABS_STEP)
	for (; i < n; i += 2)
		ABS_STEP2(i, rowbits(mask, i, n, 2), 0);
#undef ABS_STEP2
#undef ABS_STEP
	if ((vgetq_lane_u64(fin, 0) & vgetq_lane_u64(fin, 1)) == 0)
		return false;
	{
		const float64x2_t h = vmaxq_f64(vmaxq_f64(m[0], m[1]), vmaxq_f64(m[2], m[3]));

		*mx = Max(vgetq_lane_f64(h, 0), vgetq_lane_f64(h, 1));
	}
	return true;
}

static bool
agk_minmax_i32_neon(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	int32x4_t	lo[4],
				hi[4];
	int			i = 0;

	if (!mask)
		return agk_minmax_i32_scalar(mn, mx, v, mask, n);
	for (int g = 0; g < 4; g++)
		lo[g] = vdupq_n_s32(INT32_MAX), hi[g] = vdupq_n_s32(INT32_MIN);
#define MM_STEP(r, b, a) \
	do { \
		const uint32x4_t sel_ = lanes4(b); \
		const int32x4_t x_ = vld1q_s32(v + (r)); \
		lo[a] = vminq_s32(lo[a], vbslq_s32(sel_, x_, vdupq_n_s32(INT32_MAX))); \
		hi[a] = vmaxq_s32(hi[a], vbslq_s32(sel_, x_, vdupq_n_s32(INT32_MIN))); \
	} while (0)
	MASKED_WORDS(mask, n, i, MM_STEP)
	for (; i < n; i += 4)
		MM_STEP(i, rowbits(mask, i, n, 4), 0);
#undef MM_STEP
	if (!agk_any_row(mask, n))
		return false;
	*mn = vminvq_s32(vminq_s32(vminq_s32(lo[0], lo[1]), vminq_s32(lo[2], lo[3])));
	*mx = vmaxvq_s32(vmaxq_s32(vmaxq_s32(hi[0], hi[1]), vmaxq_s32(hi[2], hi[3])));
	return true;
}

/*
 * A NaN row never enters the running values (FMINNM would let a signaling NaN in): it is replaced by the identity and noted on the
 * side. The minimum is NaN only when every selected row is (the running minimum then never left +Inf and the maximum never left
 * -Inf: lo <= hi tells that a non-NaN row was seen), the maximum when any is. Unselected lanes are replaced by +-Inf.
 */
static bool
agk_minmax_f64_neon(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	float64x2_t lo[4],
				hi[4];
	uint64x2_t	anynan = vdupq_n_u64(0);
	int			i = 0;

	for (int g = 0; g < 4; g++)
		lo[g] = vdupq_n_f64(INFINITY), hi[g] = vdupq_n_f64(-INFINITY);
#define MMF_STEP2(r, b, a) \
	do { \
		const float64x2_t x_ = vld1q_f64(v + (r)); \
		const uint64x2_t sel_ = lanes2(b), ord_ = vandq_u64(sel_, vceqq_f64(x_, x_)); \
		anynan = vorrq_u64(anynan, veorq_u64(sel_, ord_)); \
		lo[a] = vminq_f64(lo[a], vbslq_f64(ord_, x_, vdupq_n_f64(INFINITY))); \
		hi[a] = vmaxq_f64(hi[a], vbslq_f64(ord_, x_, vdupq_n_f64(-INFINITY))); \
	} while (0)
#define MMF_STEP(r, b, a) do { MMF_STEP2(r, (b) & 3, a); MMF_STEP2((r) + 2, (b) >> 2, a); } while (0)
	MASKED_WORDS(mask, n, i, MMF_STEP)
	for (; !mask && i + 8 <= n; i += 8)
		for (int g = 0; g < 4; g++)
		{
			const float64x2_t x = vld1q_f64(v + i + 2 * g);
			const uint64x2_t ord = vceqq_f64(x, x);

			anynan = vorrq_u64(anynan, veorq_u64(vdupq_n_u64(~UINT64CONST(0)), ord));
			lo[g] = vminq_f64(lo[g], vbslq_f64(ord, x, vdupq_n_f64(INFINITY)));
			hi[g] = vmaxq_f64(hi[g], vbslq_f64(ord, x, vdupq_n_f64(-INFINITY)));
		}
	for (; i < n; i += 2)
		MMF_STEP2(i, rowbits(mask, i, n, 2), 0);
#undef MMF_STEP2
#undef MMF_STEP
	if (n == 0 || (mask && !agk_any_row(mask, n)))
		return false;
	{
		const float64x2_t l = vminq_f64(vminq_f64(lo[0], lo[1]), vminq_f64(lo[2], lo[3])),
					h = vmaxq_f64(vmaxq_f64(hi[0], hi[1]), vmaxq_f64(hi[2], hi[3]));
		const double lov = Min(vgetq_lane_f64(l, 0), vgetq_lane_f64(l, 1)),
					hiv = Max(vgetq_lane_f64(h, 0), vgetq_lane_f64(h, 1));

		*mn = agk_canon_minmax(lov <= hiv ? lov : __builtin_nan(""));
		*mx = agk_canon_minmax((vgetq_lane_u64(anynan, 0) | vgetq_lane_u64(anynan, 1)) ? __builtin_nan("") : hiv);
	}
	return true;
}

#define agk_minmax_i64_neon		agk_minmax_i64_scalar
#define agk_lut_i32_neon		agk_lut_i32_scalar
#define agk_gather_bits_neon	agk_gather_bits_scalar
#define agk_gid_code_neon		agk_gid_code_scalar
#define agk_gid_i32_neon		agk_gid_i32_scalar
#define agk_count_gid_neon		agk_count_gid_scalar
#define agk_sum_gid_i32_neon	agk_sum_gid_i32_scalar
#define agk_sum_gid_f64_neon	agk_sum_gid_f64_scalar

#define AGK_INIT_NEON(s)	.s = agk_##s##_neon,

const SiftAggKernels sift_aggkern_neon = {AGK_SLOTS(AGK_INIT_NEON)};

#endif
