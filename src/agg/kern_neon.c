/*
 * agg/kern_neon.c - AArch64 Advanced SIMD aggregation operators. Predicates compare the rows of a 64-row word and fold the compare
 * masks into its bits (core/neon.h); reductions turn the bits of the row mask into lane masks. Results are bit-identical to the
 * scalar table (agg/kern.c): float compares are mapped to the unordered predicates so that NaN is greater than everything. The slots
 * without a NEON kernel (lookups and per group accumulation are gathers and scatters, which NEON has no instruction for) are the
 * scalar functions.
 */
#include "core/host.h"

#define SIFT_VEC_NEON
#include "core/simd_isa.h"

#include "agg/kern_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

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

/*
 * The masked sum walks a full word of the mask in sixteen steps of four rows, four accumulators wide (vpadal widens and adds in one instruction,
 * which the bias of the shared sum does not beat here); the plain sum is the compiler's vectorized loop.
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

/* the scalar loop is faster than two 64-bit lanes on an arm64 core: both stay scalar */
#define AKV_NO_ABSMAX
#define AKV_RFN(x) agk_##x##_v_neon
#define AKV_NO_SUM_I32
#define AKV_NO_SUM_I64
#define AKV_LANES64(k) ((akv_s) vtstq_u64(vdupq_n_u64(k), (uint64x2_t) akv_w64))
#define AKV_LANES32(k) ((akv_i32) vtstq_u32(vdupq_n_u32(k), (uint32x4_t) akv_w32))
#define AKV_NO_CMP2_I64
#define AKV_FN(x) agk_##x##_neon
#include "agg/kern_vec.h"

#define agk_absmax_i64_neon		agk_absmax_i64_scalar
#define agk_sum_i64_neon		agk_sum_i64_scalar
#define agk_cmp2_i64_neon		agk_cmp2_i64_scalar
#define agk_minmax_i64_neon		agk_minmax_i64_scalar
#define agk_minmax_f64_neon		agk_minmax_f64_v_neon

#define agk_lut_i32_neon		agk_lut_i32_scalar
#define agk_gather_bits_neon	agk_gather_bits_scalar
#define agk_gid_code_neon		agk_gid_code_scalar
#define agk_gid_i32_neon		agk_gid_i32_scalar
#define agk_count_gid_neon		agk_count_gid_scalar
#define agk_sum_gid_i32_neon	agk_sum_gid_i32_scalar
#define agk_sum_gid_f64_neon	agk_sum_gid_f64_scalar
#define agk_sum_gid_i64_neon	agk_sum_gid_i64_scalar

/* a mask the lane compares build costs as much as the scalar loop (absmax_f64 922 ns for 4096 rows at 50 %, arm64), and a plain min and max is the compiler's vectorized loop (minmax_i32 141 against 430 ns) */
static bool
agk_absmax_f64_neon(const double *v, const uint64 *mask, int n, double *mx)
{
	return mask ? agk_absmax_f64_scalar(v, mask, n, mx) : agk_absmax_f64_v_neon(v, mask, n, mx);
}

static bool
agk_minmax_i32_neon(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	return mask ? agk_minmax_i32_v_neon(mn, mx, v, mask, n) : agk_minmax_i32_scalar(mn, mx, v, mask, n);
}

#define AGK_INIT_NEON(s)	.s = agk_##s##_neon,

const SiftAggKernels sift_aggkern_neon = {AGK_SLOTS(AGK_INIT_NEON)};

#endif
