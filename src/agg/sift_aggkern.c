/*
 * sift_aggkern.c - the scalar aggregation operators (the specification every SIMD level matches bit for bit) and the
 * dispatch. Baseline x86-64 code: no POPCNT, no vector instruction needed.
 */
#include "sift_host.h"

#include "sift_aggkern_int.h"

const SiftAggKernels *sift_aggk = &sift_aggkern_scalar;

/* PostgreSQL float8 order: NaN equals NaN and is greater than everything else, -0 equals +0 */
static inline int
f8_cmp(double a, double b)
{
	if (a != a)
		return b != b ? 0 : 1;
	if (b != b)
		return -1;
	return a > b ? 1 : a < b ? -1 : 0;
}

/* Bitmap of COND over the rows I < n; the bits >= n are zero */
#define PRED_BITS(out, n, I, COND) \
	do { \
		for (int w_ = 0; w_ * 64 < (n); w_++) \
		{ \
			const int	lim_ = Min(64, (n) - w_ * 64); \
			uint64		b_ = 0; \
			for (int j_ = 0; j_ < lim_; j_++) \
			{ \
				const int	I = w_ * 64 + j_; \
				b_ |= (uint64) (COND) << j_; \
			} \
			(out)[w_] = b_; \
		} \
	} while (0)

/* BODY(i) for the selected rows in row order (mask == NULL: all n rows) */
#define FOREACH_ROW(mask, n, i, BODY) \
	do { \
		if (mask) \
		{ \
			for (int w_ = 0; w_ < AGK_NW(n); w_++) \
				for (uint64 m_ = (mask)[w_]; m_; m_ &= m_ - 1) \
				{ \
					const int	i = w_ * 64 + __builtin_ctzll(m_); \
					BODY; \
				} \
		} \
		else \
		{ \
			for (int i = 0; i < (n); i++) \
				BODY; \
		} \
	} while (0)

#define CMP_F8(a, OP, b)	(f8_cmp(a, b) OP 0)

void
agk_cmp_f64_scalar(uint64 *out, const double *v, int n, SiftCmpOp op, double c)
{
	switch (op)
	{
		case SCMP_EQ: PRED_BITS(out, n, i, CMP_F8(v[i], ==, c)); break;
		case SCMP_NE: PRED_BITS(out, n, i, CMP_F8(v[i], !=, c)); break;
		case SCMP_LT: PRED_BITS(out, n, i, CMP_F8(v[i], <, c)); break;
		case SCMP_LE: PRED_BITS(out, n, i, CMP_F8(v[i], <=, c)); break;
		case SCMP_GT: PRED_BITS(out, n, i, CMP_F8(v[i], >, c)); break;
		case SCMP_GE: PRED_BITS(out, n, i, CMP_F8(v[i], >=, c)); break;
	}
}

void
agk_between_f64_scalar(uint64 *out, const double *v, int n, double lo, double hi)
{
	PRED_BITS(out, n, i, f8_cmp(v[i], lo) >= 0 && f8_cmp(v[i], hi) <= 0);
}

void
agk_lut_i32_scalar(uint64 *out, const int32 *v, int n, int32 base, uint32 nlut, const uint8 *lut)
{
	PRED_BITS(out, n, i, (uint32) v[i] - (uint32) base < nlut && lut[(uint32) v[i] - (uint32) base] != 0);
}

void
agk_gather_bits_scalar(uint64 *out, const uint64 *rowmask, const uint32 *parent, int m)
{
	PRED_BITS(out, m, e, rowmask[parent[e] >> 6] >> (parent[e] & 63) & 1);
}

void
agk_gid_code_scalar(uint32 *gid, const uint32 *code, const uint32 *map, int n, uint32 mult)
{
	for (int i = 0; i < n; i++)
		gid[i] += map[code[i]] * mult;
}

void
agk_gid_i32_scalar(uint32 *gid, const int32 *v, int32 base, const uint32 *map, int n, uint32 mult)
{
	for (int i = 0; i < n; i++)
		gid[i] += map[(uint32) v[i] - (uint32) base] * mult;
}

int64
agk_sum_i32_scalar(const int32 *v, const uint64 *mask, int n)
{
	int64		s = 0;

	FOREACH_ROW(mask, n, i, s += v[i]);
	return s;
}

double
agk_sum_f64_scalar(const double *v, const uint64 *mask, int n)
{
	double		a[8] = {0, 0, 0, 0, 0, 0, 0, 0};

	FOREACH_ROW(mask, n, i, a[i & 7] += v[i]);
	return agk_canon_nan(((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7])));
}

bool
agk_absmax_f64_scalar(const double *v, const uint64 *mask, int n, double *mx)
{
	double		m = 0;
	bool		finite = true;

	FOREACH_ROW(mask, n, i, { const double a = fabs(v[i]); finite &= a <= DBL_MAX; m = a > m ? a : m; });
	if (finite)
		*mx = m;
	return finite;
}

#define DEF_MINMAX(fn, T, LT) \
bool \
fn(T *mn, T *mx, const T *v, const uint64 *mask, int n) \
{ \
	T			lo = 0, \
				hi = 0; \
	bool		any = false; \
\
	FOREACH_ROW(mask, n, i, \
				{ \
					const T x = v[i]; \
					if (!any) \
						lo = hi = x; \
					lo = LT(x, lo) ? x : lo; \
					hi = LT(hi, x) ? x : hi; \
					any = true; \
				}); \
	if (any) \
		*mn = lo, *mx = hi; \
	return any; \
}

#define LT_INT(a, b)	((a) < (b))
#define LT_F8(a, b)		(f8_cmp(a, b) < 0)

DEF_MINMAX(agk_minmax_i32_scalar, int32, LT_INT)
DEF_MINMAX(agk_minmax_i64_scalar, int64, LT_INT)

bool
agk_minmax_f64_scalar(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	double		lo = 0,
				hi = 0;
	bool		any = false;

	FOREACH_ROW(mask, n, i,
				{
					const double x = v[i];

					if (!any)
						lo = hi = x;
					lo = LT_F8(x, lo) ? x : lo;
					hi = LT_F8(hi, x) ? x : hi;
					any = true;
				});
	if (any)
	{
		*mn = agk_canon_minmax(lo);
		*mx = agk_canon_minmax(hi);
	}
	return any;
}

void
agk_count_gid_scalar(int64 *cnt, const uint32 *gid, const uint64 *mask, int n)
{
	FOREACH_ROW(mask, n, i, cnt[gid[i]]++);
}

void
agk_sum_gid_i32_scalar(int64 *acc, const uint32 *gid, const int32 *v, const uint64 *mask, int n)
{
	FOREACH_ROW(mask, n, i, acc[gid[i]] += v[i]);
}

void
agk_sum_gid_f64_scalar(double *acc, const uint32 *gid, const double *v, const uint64 *mask, int n)
{
	FOREACH_ROW(mask, n, i, acc[gid[i]] += v[i]);
}

int64
agk_count_changes_scalar(const uint32 *v, const uint64 *mask, int n, uint32 *prev)
{
	int64		cnt = 0;
	uint32		p = *prev;

	FOREACH_ROW(mask, n, i, { cnt += v[i] != p; p = v[i]; });
	*prev = p;
	return cnt;
}

#define AGK_INIT(s)	.s = agk_##s##_scalar,

const SiftAggKernels sift_aggkern_scalar = {AGK_SLOTS(AGK_INIT)};

const SiftAggKernels *
sift_aggkern_for(SiftSimdLevel level)
{
#if defined(__x86_64__)
	if (level >= SIFT_SIMD_AVX512 && sift_cpu.detected >= SIFT_SIMD_AVX512)
		return &sift_aggkern_avx512;
	if (level >= SIFT_SIMD_AVX2 && sift_cpu.detected >= SIFT_SIMD_AVX2)
		return &sift_aggkern_avx2;
#endif
	return &sift_aggkern_scalar;
}

static void
aggkern_rebind(const SiftCpu *cpu)
{
	sift_aggk = sift_aggkern_for(cpu->active);
}

/* Idempotent: the executor's init and the tests both call it */
void
sift_aggkern_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	sift_cpu_register(aggkern_rebind);
}
