/*
 * agg/kern6/neon_cmp.c - the AArch64 Advanced SIMD predicates of the 2048-row engine (agg/kern6/cmp.c is the specification). The compares
 * of an integer column fill a whole word of 64 rows at a time (the compare results narrowed to bytes and folded into bits, core/neon.h);
 * the operator is a constant of each copy of the body. Doubles take the float8 order from two masks (the NaNs of each side). The rows
 * that do not fill a word are the scalar rows (a word of 64 rows is the whole of a 2048-row batch).
 */
#include "core/host.h"

#include "agg/kern6/neon.h"

#if defined(SIFT_HAVE_NEON)

#define THREE(a, b)		((a) < (b) ? -1 : (a) > (b))
#define FOR_OPS(CASE)	CASE(SCMP_EQ) CASE(SCMP_NE) CASE(SCMP_LT) CASE(SCMP_LE) CASE(SCMP_GT) CASE(SCMP_GE)

/*
 * The words of n bits: the words of 64 rows by WORD(base), the rest of the rows by ROW(j) (the bit of row j); the rows of a word that is
 * not whole are the rows after the last whole one
 */
#define PRED_WORDS(out, n, WORD, ROW) \
	do { \
		const int nw_ = AK6_NW(n), nfull_ = (n) >> 6; \
		for (int w_ = 0; w_ < nfull_; w_++) \
		{ \
			const int base = w_ * 64; \
			(out)[w_] = (WORD); \
		} \
		if (nw_ > nfull_) \
		{ \
			uint64 word_ = 0; \
			for (int j = nfull_ * 64; j < (n); j++) \
				word_ |= (uint64) (ROW) << (j & 63); \
			(out)[nfull_] = word_; \
		} \
	} while (0)

NK uint32x4_t
nm_i32(SiftCmpOp op, int32x4_t x, int32x4_t y)
{
	switch (op)
	{
		case SCMP_EQ:
			return vceqq_s32(x, y);
		case SCMP_NE:
			return vmvnq_u32(vceqq_s32(x, y));
		case SCMP_LT:
			return vcltq_s32(x, y);
		case SCMP_LE:
			return vcleq_s32(x, y);
		case SCMP_GT:
			return vcgtq_s32(x, y);
		default:
			return vcgeq_s32(x, y);
	}
}

NK uint64x2_t
nm_i64(SiftCmpOp op, int64x2_t x, int64x2_t y)
{
	switch (op)
	{
		case SCMP_EQ:
			return vceqq_s64(x, y);
		case SCMP_NE:
			return not64(vceqq_s64(x, y));
		case SCMP_LT:
			return vcltq_s64(x, y);
		case SCMP_LE:
			return vcleq_s64(x, y);
		case SCMP_GT:
			return vcgtq_s64(x, y);
		default:
			return vcgeq_s64(x, y);
	}
}

/* yn: the lanes of y that are NaN */
NK uint64x2_t
nm_f64(SiftCmpOp op, float64x2_t x, float64x2_t y, uint64x2_t yn)
{
	const uint64x2_t xn = not64(vceqq_f64(x, x)),
				lt = vorrq_u64(vcltq_f64(x, y), vbicq_u64(yn, xn)),
				eq = vorrq_u64(vceqq_f64(x, y), vandq_u64(xn, yn));

	switch (op)
	{
		case SCMP_EQ:
			return eq;
		case SCMP_NE:
			return not64(eq);
		case SCMP_LT:
			return lt;
		case SCMP_LE:
			return vorrq_u64(lt, eq);
		case SCMP_GT:
			return not64(vorrq_u64(lt, eq));
		default:
			return not64(lt);
	}
}

#define CI32(x)		nm_i32(op, x, cv)
#define CI64(x)		nm_i64(op, x, cv)
#define CF64(x)		nm_f64(op, x, cv, cn)
#define LDI32(p)	vld1q_s32(p)
#define LDI64(p)	vld1q_s64(p)
#define LDF64(p)	vld1q_f64(p)

NK void
cmp_i32_k(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	const int32x4_t cv = vdupq_n_s32(c);

	PRED_WORDS(out, n, SIFT_NEON_WORD(32, LDI32, v + base, CI32), ak6_op_holds(op, THREE(v[j], c)));
}

NK void
cmp_i64_k(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	const int64x2_t cv = vdupq_n_s64(c);

	PRED_WORDS(out, n, SIFT_NEON_WORD(64, LDI64, v + base, CI64), ak6_op_holds(op, THREE(v[j], c)));
}

NK void
cmp_f64_k(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	const float64x2_t cv = vdupq_n_f64(c);
	const uint64x2_t cn = not64(vceqq_f64(cv, cv));

	PRED_WORDS(out, n, SIFT_NEON_WORD(64, LDF64, v + base, CF64), ak6_op_holds(op, ak6_f8_cmp(v[j], c)));
}

/* Between two arrays: the masks of the four (two) rows at base + k */
NK uint32x4_t
nm2_i32(SiftCmpOp op, const int32 *a, const int32 *b, int base, int k)
{
	return nm_i32(op, vld1q_s32(a + base + k), vld1q_s32(b + base + k));
}

NK uint64x2_t
nm2_i64(SiftCmpOp op, const int64 *a, const int64 *b, int base, int k)
{
	return nm_i64(op, vld1q_s64(a + base + k), vld1q_s64(b + base + k));
}

NK uint64x2_t
nm2_f64(SiftCmpOp op, const double *a, const double *b, int base, int k)
{
	const float64x2_t y = vld1q_f64(b + base + k);

	return nm_f64(op, vld1q_f64(a + base + k), y, not64(vceqq_f64(y, y)));
}

/* A word of 64 rows of two arrays: the 16 byte masks of four rows at a time in the order of the rows */
#define WORD2(W, FN, a, b, base) \
	sift_neon_movemask64(PACK16_##W(FN, a, b, base, 0), PACK16_##W(FN, a, b, base, 16), \
						 PACK16_##W(FN, a, b, base, 32), PACK16_##W(FN, a, b, base, 48))
#define PACK16_32(FN, a, b, base, o) \
	sift_neon_pack16(sift_neon_pack32(FN(op, a, b, base, (o)), FN(op, a, b, base, (o) + 4)), \
					 sift_neon_pack32(FN(op, a, b, base, (o) + 8), FN(op, a, b, base, (o) + 12)))
#define PACK16_64(FN, a, b, base, o) \
	sift_neon_pack16(sift_neon_pack32(sift_neon_pack64(FN(op, a, b, base, (o)), FN(op, a, b, base, (o) + 2)), \
									  sift_neon_pack64(FN(op, a, b, base, (o) + 4), FN(op, a, b, base, (o) + 6))), \
					 sift_neon_pack32(sift_neon_pack64(FN(op, a, b, base, (o) + 8), FN(op, a, b, base, (o) + 10)), \
									  sift_neon_pack64(FN(op, a, b, base, (o) + 12), FN(op, a, b, base, (o) + 14))))

NK void
cmp2_i32_k(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	PRED_WORDS(out, n, WORD2(32, nm2_i32, a, b, base), ak6_op_holds(op, THREE(a[j], b[j])));
}

NK void
cmp2_i64_k(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	PRED_WORDS(out, n, WORD2(64, nm2_i64, a, b, base), ak6_op_holds(op, THREE(a[j], b[j])));
}

NK void
cmp2_f64_k(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	PRED_WORDS(out, n, WORD2(64, nm2_f64, a, b, base), ak6_op_holds(op, ak6_f8_cmp(a[j], b[j])));
}

#define DEF_CMP(name, ARGS, CALLARGS) \
void \
ak6_##name##_neon ARGS \
{ \
	switch (op) \
	{ \
		case SCMP_EQ: name##_k(CALLARGS(SCMP_EQ)); return; \
		case SCMP_NE: name##_k(CALLARGS(SCMP_NE)); return; \
		case SCMP_LT: name##_k(CALLARGS(SCMP_LT)); return; \
		case SCMP_LE: name##_k(CALLARGS(SCMP_LE)); return; \
		case SCMP_GT: name##_k(CALLARGS(SCMP_GT)); return; \
		case SCMP_GE: name##_k(CALLARGS(SCMP_GE)); return; \
	} \
}

#define ARGS_CONST(o)	out, v, n, o, c
#define ARGS_PAIR(o)	out, a, b, n, o
DEF_CMP(cmp_i32, (uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c), ARGS_CONST)
DEF_CMP(cmp_i64, (uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c), ARGS_CONST)
DEF_CMP(cmp_f64, (uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c), ARGS_CONST)
DEF_CMP(cmp2_i32, (uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op), ARGS_PAIR)
DEF_CMP(cmp2_i64, (uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op), ARGS_PAIR)
DEF_CMP(cmp2_f64, (uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op), ARGS_PAIR)

/* ---- int128: a value a register, its lanes (low, high); the order of a pair is that of its high half (signed), then its low half (unsigned) ---- */

NK bool
i128_holds(SiftCmpOp op, uint64x2_t x, uint64x2_t c)
{
	const uint64x2_t ltu = vcltq_u64(x, c),
				lts = vcltq_s64(vreinterpretq_s64_u64(x), vreinterpretq_s64_u64(c)),
				eq = vceqq_u64(x, c);
	const bool	hi_eq = vgetq_lane_u64(eq, 1) != 0,
				lt = vgetq_lane_u64(lts, 1) || (hi_eq && vgetq_lane_u64(ltu, 0)),
				eq128 = hi_eq && vgetq_lane_u64(eq, 0);

	return ak6_cmp_sel(op, lt, eq128, 1);
}

void
ak6_cmp_i128_neon(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	const uint64x2_t cv = vld1q_u64((const uint64 *) &c);
	const int	nw = AK6_NW(n);

	for (int w = 0; w < nw; w++)
	{
		const int	end = Min(w * 64 + 64, (int) n);
		uint64		word = 0;

		for (int j = w * 64; j < end; j++)
			word |= (uint64) i128_holds(op, vld1q_u64((const uint64 *) (v + j)), cv) << (j & 63);
		out[w] = word;
	}
}

/* ---- a lookup table, a set, a string ---- */

void
ak6_lut_u32_neon(uint64 *out, const uint32 *v, uint16 n, uint32 base, uint32 nlut, const uint8 *lut)
{
	const uint32x4_t vbase = vdupq_n_u32(base),
				vn = vdupq_n_u32(nlut);
	const int	nw = AK6_NW(n);

	for (int w = 0; w < nw; w++)
	{
		const int	end = Min(w * 64 + 64, (int) n);
		int			j = w * 64;
		uint64		word = 0;

		for (; j + 4 <= end; j += 4)
		{
			const uint32x4_t idx = vsubq_u32(vld1q_u32(v + j), vbase);
			const uint32 in = mm4(vcltq_u32(idx, vn));
			uint32		ix[4];
			uint32		bits = 0;

			if (!in)
				continue;
			vst1q_u32(ix, idx);
			for (int k = 0; k < 4; k++)
				bits |= (uint32) (((in >> k) & 1) && lut[ix[k]] != 0) << k;
			word |= (uint64) bits << (j & 63);
		}
		for (; j < end; j++)
			word |= (uint64) ((uint32) (v[j] - base) < nlut && lut[v[j] - base] != 0) << (j & 63);
		out[w] = word;
	}
}

void
ak6_in_set_i64_neon(uint64 *out, const int64 *v, uint16 n, const int64 *set, uint8 nset)
{
	const int	nw = AK6_NW(n);

	for (int w = 0; w < nw; w++)
	{
		const int	end = Min(w * 64 + 64, (int) n);
		int			j = w * 64;
		uint64		word = 0;

		for (; j + 2 <= end; j += 2)
		{
			const int64x2_t x = vld1q_s64(v + j);
			uint64x2_t	acc = vdupq_n_u64(0);

			for (int k = 0; k < nset; k++)
				acc = vorrq_u64(acc, vceqq_s64(x, vdupq_n_s64(set[k])));
			word |= (uint64) mm2(acc) << (j & 63);
		}
		for (; j < end; j++)
		{
			bool		in = false;

			for (int k = 0; k < nset; k++)
				in |= v[j] == set[k];
			word |= (uint64) in << (j & 63);
		}
		out[w] = word;
	}
}

/* A view a register: the equal halves say it, and only a view of more than 12 bytes with an equal first half is compared by content */
void
ak6_cmp_strview_eq_neon(uint64 *out, const SiftStrView *v, uint16 n, SiftStrView target)
{
	const uint64x2_t t = vld1q_u64((const uint64 *) &target);
	const bool	tlong = target.len > 12;
	const int	nw = AK6_NW(n);

	for (int w = 0; w < nw; w++)
	{
		const int	end = Min(w * 64 + 64, (int) n);
		uint64		word = 0;

		for (int j = w * 64; j < end; j++)
		{
			const uint64x2_t eq = vceqq_u64(vld1q_u64((const uint64 *) (v + j)), t);
			const bool	hdr = vgetq_lane_u64(eq, 0) != 0,
						hit = tlong ? hdr && ak6_sv_eq(&v[j], &target) : hdr && vgetq_lane_u64(eq, 1) != 0;

			word |= (uint64) hit << (j & 63);
		}
		out[w] = word;
	}
}

/* ---- the run boundaries: the first two rows are the scalar rows ---- */

void
ak6_stream_run_ne_i64_neon(uint64 *out, const int64 *v, const uint64 *valid, uint16 n)
{
	const int	nw = AK6_NW(n);

	for (int w = 0; w < nw; w++)
	{
		const int	end = Min(w * 64 + 64, (int) n);
		int			j = w * 64;
		uint64		word = 0;

		for (; j < end && j < 2; j++)
			word |= (uint64) (j == 0 || v[j] != v[j - 1]) << (j & 63);
		for (; j + 2 <= end; j += 2)
			word |= (uint64) (mm2(vceqq_s64(vld1q_s64(v + j), vld1q_s64(v + j - 1))) ^ 3) << (j & 63);
		for (; j < end; j++)
			word |= (uint64) (v[j] != v[j - 1]) << (j & 63);
		out[w] = word;
	}
	if (valid)
		for (int w = 0; w < nw; w++)
		{
			const uint64 vw = valid[w],
						prev = (vw << 1) | (w ? valid[w - 1] >> 63 : 0);

			out[w] = ((vw ^ prev) | (vw & out[w])) & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));
		}
	if (nw)
		out[0] |= 1;
}

#endif							/* SIFT_HAVE_NEON */
