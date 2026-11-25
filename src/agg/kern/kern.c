/*
 * agg/kern/kern.c - the scalar operators of the 2048-row engine (the specification every SIMD level matches bit for bit) and the
 * dispatch. Baseline x86-64 code: no POPCNT, no vector instruction needed.
 */
#include "agg/kern/int.h"
#include "core/fmv.h"

const SiftAggKernelsV6 *sift_aggk6 = &sift_aggkern6_scalar;

#define ROW_BIT(mask, i)	(((mask)[(i) >> 6] >> ((i) & 63)) & 1)

/* BODY(i) for the selected rows in row order (mask == NULL: all n rows) */
#define FOREACH_ROW(mask, n, i, BODY) \
	do { \
		for (int i = 0; i < (n); i++) \
			if (!(mask) || ROW_BIT(mask, i)) \
			{ \
				BODY; \
			} \
	} while (0)

#define ROW_OF(sel, i)		ROW_OF_SEL(sel, i)

/* ---- the index of the k-th set bit of a byte, for the selection vector of the vector levels ---- */
#define IX0(b)		((uint8) __builtin_ctz((b) | 256))
#define CL(b)		((b) & ((b) - 1))
#define BROW(b)		{IX0(b), IX0(CL(b)), IX0(CL(CL(b))), IX0(CL(CL(CL(b)))), IX0(CL(CL(CL(CL(b))))), \
					 IX0(CL(CL(CL(CL(CL(b)))))), IX0(CL(CL(CL(CL(CL(CL(b))))))), IX0(CL(CL(CL(CL(CL(CL(CL(b))))))))}
#define R4(F, m)	F(m), F(m + 1), F(m + 2), F(m + 3)
#define R16(F, m)	R4(F, m), R4(F, m + 4), R4(F, m + 8), R4(F, m + 12)
#define BR16(m)		R16(BROW, m)

const uint8 ak6_byte_idx[256][8] = {
	BR16(0), BR16(16), BR16(32), BR16(48), BR16(64), BR16(80), BR16(96), BR16(112),
	BR16(128), BR16(144), BR16(160), BR16(176), BR16(192), BR16(208), BR16(224), BR16(240)
};

/* ---- bitmaps ---- */

uint16
ak6_mask_andn_del_scalar(uint64 *dst, const uint64 *del, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		pc = 0;

	for (int w = 0; w < nw; w++)
	{
		dst[w] &= ~del[w];
		if (w == nw - 1)
			dst[w] &= AK6_TAIL(n);
		pc += sift_pc64(dst[w]);
	}
	return pc;
}

uint16
ak6_mask_and_scalar(uint64 *dst, const uint64 *a, const uint64 *b, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		pc = 0;

	for (int w = 0; w < nw; w++)
	{
		dst[w] = a[w] & b[w];
		if (w == nw - 1)
			dst[w] &= AK6_TAIL(n);
		pc += sift_pc64(dst[w]);
	}
	return pc;
}

uint16
ak6_mask_to_sel16_scalar(uint16 *sel, const uint64 *mask, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		k = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		bits = mask[w] & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));

		for (; bits; bits &= bits - 1)
			sel[k++] = (uint16) (w * 64 + __builtin_ctzll(bits));
	}
	return k;
}

uint16
ak6_key_present_apply_scalar(uint64 *dst, const uint64 *kp, const uint64 *doc_valid, uint16 n, bool negate)
{
	const int	nw = AK6_NW(n);
	uint32		pc = 0;

	for (int w = 0; w < nw; w++)
	{
		dst[w] &= negate ? ~kp[w] & doc_valid[w] : kp[w];
		if (w == nw - 1)
			dst[w] &= AK6_TAIL(n);
		pc += sift_pc64(dst[w]);
	}
	return pc;
}

/* ---- hashing ---- */

void
ak6_hash_i64_scalar(uint64 *h, const int64 *v, const uint16 *sel, uint16 n, bool mix)
{
	for (int i = 0; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) v[ROW_OF(sel, i)], mix);
}

void
ak6_hash_i32_scalar(uint64 *h, const int32 *v, const uint16 *sel, uint16 n, bool mix)
{
	for (int i = 0; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) (int64) v[ROW_OF(sel, i)], mix);
}

void
ak6_hash_i128_scalar(uint64 *h, const SiftI128 *v, const uint16 *sel, uint16 n, bool mix)
{
	for (int i = 0; i < n; i++)
	{
		const SiftI128 *x = &v[ROW_OF(sel, i)];

		h[i] = AK6_HASH_STEP(AK6_HASH_STEP(h[i], x->lo, mix), (uint64) x->hi, true);
	}
}

void
ak6_hash_strview_scalar(uint64 *h, const SiftStrView *v, const uint16 *sel, uint16 n, bool mix)
{
	for (int i = 0; i < n; i++)
		h[i] = ak6_sv_hash(h[i], &v[ROW_OF(sel, i)], mix);
}

/* ---- reductions ---- */

int64
ak6_sum_i32_scalar(const int32 *v, const uint64 *mask, uint16 n)
{
	int64		s = 0;

	FOREACH_ROW(mask, n, i, s += v[i]);
	return s;
}

SiftI128
ak6_sum_i64_to_i128_scalar(const int64 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	ak6_u128	s = ak6_u128_of(init);

	FOREACH_ROW(mask, n, i, s += (ak6_u128) (__int128) v[i]);
	return ak6_i128_of(s);
}

bool
ak6_sum_i64_checked_scalar(const int64 *v, const uint64 *mask, uint16 n, int64 *out)
{
	const SiftI128 zero = {0, 0};

	return ak6_fits_i64(ak6_sum_i64_to_i128_scalar(v, mask, n, zero), out);
}

SiftI128
ak6_sum_i128_scalar(const SiftI128 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	ak6_u128	s = ak6_u128_of(init);

	FOREACH_ROW(mask, n, i, s += ak6_u128_of(v[i]));
	return ak6_i128_of(s);
}

double
ak6_sum_f64_fast_scalar(const double *v, const uint64 *mask, uint16 n, double init)
{
	double		a[8] = {0, 0, 0, 0, 0, 0, 0, 0};

	for (int i = 0; i < n; i++)
		a[i & 7] += (!mask || ROW_BIT(mask, i)) ? v[i] : 0.0;
	return ak6_f64_tree(a, init);
}

bool
ak6_minmax_i128_scalar(SiftI128 *mn, SiftI128 *mx, const SiftI128 *v, const uint64 *mask, uint16 n)
{
	bool		any = false;
	__int128	lo = 0,
				hi = 0;

	FOREACH_ROW(mask, n, i,
				{
					__int128	x = (__int128) ak6_u128_of(v[i]);

					if (!any || x < lo)
						lo = x;
					if (!any || x > hi)
						hi = x;
					any = true;
				});
	if (any)
	{
		*mn = ak6_i128_of((ak6_u128) lo);
		*mx = ak6_i128_of((ak6_u128) hi);
	}
	return any;
}

bool
ak6_minmax_i32_scalar(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, uint16 n)
{
	bool		any = false;
	int32		lo = 0,
				hi = 0;

	FOREACH_ROW(mask, n, i,
				{
					lo = !any || v[i] < lo ? v[i] : lo;
					hi = !any || v[i] > hi ? v[i] : hi;
					any = true;
				});
	if (any)
	{
		*mn = lo;
		*mx = hi;
	}
	return any;
}

bool
ak6_minmax_i64_scalar(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, uint16 n)
{
	bool		any = false;
	int64		lo = 0,
				hi = 0;

	FOREACH_ROW(mask, n, i,
				{
					lo = !any || v[i] < lo ? v[i] : lo;
					hi = !any || v[i] > hi ? v[i] : hi;
					any = true;
				});
	if (any)
	{
		*mn = lo;
		*mx = hi;
	}
	return any;
}

bool
ak6_minmax_f64_scalar(double *mn, double *mx, const double *v, const uint64 *mask, uint16 n)
{
	bool		any = false;
	int64		lo = 0,
				hi = 0;

	FOREACH_ROW(mask, n, i,
				{
					const int64 k = ak6_f8_key(v[i]);

					lo = !any || k < lo ? k : lo;
					hi = !any || k > hi ? k : hi;
					any = true;
				});
	if (any)
	{
		*mn = ak6_f8_of_key(lo);
		*mx = ak6_f8_of_key(hi);
	}
	return any;
}

/* ---- per group accumulation ---- */

#define FOREACH_ACTIVE(sel, n, r, BODY) \
	do { \
		for (int i_ = 0; i_ < (n); i_++) \
		{ \
			const int	r = ROW_OF(sel, i_); \
			BODY; \
		} \
	} while (0)

void
ak6_count_gid_scalar(int64 *cnt, const uint32 *gid, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, cnt[gid[r]]++);
}

void
ak6_sum_gid_i32_scalar(int64 *acc, const uint32 *gid, const int32 *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_sum_i32(acc, gid[r], v[r]));
}

void
ak6_sum_gid_i64_scalar(int64 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_sum_i64(acc, gid[r], v[r]));
}

void
ak6_sum_gid_i64_to_i128_scalar(SiftI128 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_sum_i64_i128(acc, gid[r], v[r]));
}

void
ak6_sum_gid_i128_scalar(SiftI128 *acc, const uint32 *gid, const SiftI128 *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_sum_i128(acc, gid[r], v[r]));
}

void
ak6_sum_gid_f64_fast_scalar(double *acc, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, acc[gid[r]] = ak6_add_f64(acc[gid[r]], v[r]));
}

void
ak6_minmax_gid_i64_scalar(int64 *mn, int64 *mx, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_minmax_i64(mn, mx, gid[r], v[r]));
}

void
ak6_minmax_gid_f64_scalar(double *mn, double *mx, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
	FOREACH_ACTIVE(sel, n, r, ak6_upd_minmax_f64(mn, mx, gid[r], v[r]));
}

/* ---- numeric programs ---- */

/* One body for the scalar level and, compiled per ISA (core/fmv.h), for the AVX2 and AVX-512 levels: ak6_lin_i64_scalar is the specification, ak6_lin_i64 the slot of the vector levels. dst may be a or b. */
SIFT_FMV_DEFINE_VOID(, ak6_lin_i64, (int64 *dst, const int64 *a, int32 ka, const int64 *b, int32 kb, uint16 n), (dst, a, ka, b, kb, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		dst[i] = (int64) ((uint64) a[i] * (uint64) (int64) ka + (uint64) b[i] * (uint64) (int64) kb);
})

SIFT_FMV_DEFINE_VOID(, ak6_mulw_i64, (int64 *dst, const int64 *a, const int64 *b, uint16 n), (dst, a, b, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		dst[i] = (int64) ((uint64) a[i] * (uint64) b[i]);
})

#define AK6_INIT(s)	.s = ak6_##s##_scalar,

const SiftAggKernelsV6 sift_aggkern6_scalar = {AK6_SLOTS(AK6_INIT)};

const SiftAggKernelsV6 *
sift_aggkern6_for(SiftSimdLevel level)
{
#if defined(SIFT_HAVE_NEON)
	if (level >= SIFT_SIMD_NEON && sift_cpu.detected >= SIFT_SIMD_NEON)
		return &sift_aggkern6_neon;
#endif
#if defined(__x86_64__)
	if (level >= SIFT_SIMD_AVX512 && sift_cpu.detected >= SIFT_SIMD_AVX512)
		return &sift_aggkern6_avx512;
	if (level >= SIFT_SIMD_AVX2 && sift_cpu.detected >= SIFT_SIMD_AVX2)
		return &sift_aggkern6_avx2;
#endif
	return &sift_aggkern6_scalar;
}

static void
aggkern6_rebind(const SiftCpu *cpu)
{
	sift_aggk6 = sift_aggkern6_for(cpu->active);
}

void
sift_aggkern6_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	sift_cpu_register(aggkern6_rebind);
}
