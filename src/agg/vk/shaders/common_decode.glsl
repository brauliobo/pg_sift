/*
 * common_decode.glsl - bindings, layouts and the access to the decoded columns of the Vulkan compute engine (agg/vk/vk.h). Included by
 * every shader; the C side is agg/vk/layout.h, which must agree word for word.
 *
 * A decoded column (DCol) of a granule is the values of one column, row by row: value = base + field, the field w bytes wide (w = 0: the
 * column is the constant base), the nulls a bit per row (1: valid) or none. Floats are held as their order-preserving key (sift_col_f8_key).
 */
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_atomic_int64 : require

#ifndef ARENA_Q
#define ARENA_Q readonly
#endif

layout(std430, set = 0, binding = 0) ARENA_Q buffer A32 { uint a32[]; };
layout(std430, set = 0, binding = 1) ARENA_Q buffer A8 { uint8_t a8[]; };
layout(std430, set = 0, binding = 2) ARENA_Q buffer A16 { uint16_t a16[]; };
layout(std430, set = 0, binding = 3) ARENA_Q buffer A64 { uint64_t a64[]; };
layout(std430, set = 0, binding = 4) coherent buffer W32 { uint w32[]; };
layout(std430, set = 0, binding = 5) coherent buffer W8 { uint8_t w8[]; };
layout(std430, set = 0, binding = 6) coherent buffer W64 { uint64_t w64[]; };
layout(std430, set = 0, binding = 7) readonly buffer Dj { uint dj[]; };
layout(std430, set = 0, binding = 8) readonly buffer Ctl { uint ctl[]; };
layout(std430, set = 0, binding = 9) coherent buffer Res { int64_t res[]; };
layout(std430, set = 0, binding = 10) coherent buffer Ht { int64_t ht[]; };
layout(std430, set = 0, binding = 11) coherent buffer Out { int64_t out64[]; };

layout(push_constant) uniform PC { uint pc0; uint pc1; uint pc2; uint pc3; };

/* the Res words */
#define RES_ERR			0u		/* the first error of the scan, packed (granule, row, op, code), ~0 none */
#define RES_OVF			1u		/* the hash table filled */
#define RES_HTUSED		2u		/* groups of the hash table */
#define RES_NCAND		3u		/* candidates of the top N */
#define RES_NCOMP		4u		/* groups the compaction wrote */
#define RES_DEC_OFF		64u		/* a status word of every decode job */
#define RES_DEC_CAP		16384u
#define RES_ACC_OFF		(RES_DEC_OFF + RES_DEC_CAP)

/* the job header words of ctl */
#define J_NGRAN			0u
#define J_NCOLS			1u
#define J_NOPS			2u
#define J_NAGGS			3u
#define J_NKEYS			4u
#define J_WHERE			5u
#define J_GRAN0			6u
#define J_FLAGS			7u
#define J_GT			8u
#define J_DT			9u
#define J_OPS			10u
#define J_POOL			11u
#define J_DEAD			12u
#define J_KEYREG		16u		/* 8 words */
#define J_AGGKIND		24u		/* 16 words */
#define J_AGGVREG		40u
#define J_AGGFREG		56u
#define J_DENSE_KMIN	72u		/* 2 keys x (lo, hi) */
#define J_DENSE_SPAN	76u		/* 2 words: the values of a key, the null slot included */
#define J_DENSE_G		78u
#define J_HT_MASK		80u
#define J_HT_STRIDE		81u		/* u64 words of a slot */
#define J_HT_MAXLOAD	82u
#define J_THETA			84u		/* lo, hi */
#define J_TOPN_DESC		86u
#define J_TOPN_NULLS	87u
#define J_CAND_CAP		88u
#define J_SORTREG		89u
#define J_FAST_IMM		96u		/* 4 x (lo, hi): the constant of each comparison of a fast kernel */
#define J_CNT_ROWS		112u	/* bit a: the counts of aggregate a are the rows of its group (fast kernels: no nulls, no filter) */
#define J_HDR_WORDS		128u

#define AK_COUNT_STAR	0u
#define AK_COUNT		1u
#define AK_SUM_I		2u
#define AK_SUM_F		3u
#define AK_MIN			4u
#define AK_MAX			5u
#define AK_MIN_F		6u
#define AK_MAX_F		7u

#define NONE8			255u
#define DC_ALL_VALID	0xFFFFFFFFu
#define DC_ALL_NULL		0xFFFFFFFEu
#define GRAN_ROWS		8192u


#define CMP_EQ		0
#define CMP_NE		1
#define CMP_LT		2
#define CMP_LE		3
#define CMP_GT		4
#define CMP_GE		5

const int64_t I64_MIN = int64_t(0x8000000000000000ul);
const int64_t I64_MAX = int64_t(0x7FFFFFFFFFFFFFFFul);

/* the order-preserving key of a double and back (col/api/leaf.h) */
int64_t f8_key(double f)
{
	uint64_t b = isnan(f) ? 0x7FF8000000000000ul : doubleBitsToUint64(f);
	return int64_t(b ^ ((uint64_t(int64_t(b) >> 63)) >> 1));
}

double f8_from_key(int64_t key)
{
	uint64_t b = uint64_t(key) ^ ((uint64_t(key >> 63)) >> 1);
	return uint64BitsToDouble(b);
}

double dbl(int64_t v)
{
	return int64BitsToDouble(v);
}

int64_t bitsd(double d)
{
	return doubleBitsToInt64(d);
}

int fcmp(double a, double b)
{
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	if (a == b)
		return 0;
	return isnan(a) ? (isnan(b) ? 0 : 1) : -1;
}

bool cmp_ok(int c, int op)
{
	return op == CMP_EQ ? c == 0 : op == CMP_NE ? c != 0 : op == CMP_LT ? c < 0 : op == CMP_LE ? c <= 0 : op == CMP_GT ? c > 0 : c >= 0;
}

uint64_t u64of(uint lo, uint hi)
{
	return (uint64_t(hi) << 32) | uint64_t(lo);
}

#ifndef DECODE_SHADER
/* a value of a decoded column, desc = the word of its descriptor in ctl */
int64_t dc_value(uint desc, uint row)
{
	uint w = ctl[desc + 4u];
	uint64_t base = u64of(ctl[desc + 2u], ctl[desc + 3u]);
	uint off = ctl[desc];
	uint64_t f = 0ul;
	if (w == 1u)
		f = uint64_t(a8[off * 4u + row]);
	else if (w == 2u)
		f = uint64_t(a16[off * 2u + row]);
	else if (w == 4u)
		f = uint64_t(a32[off + row]);
	else if (w == 8u)
		f = a64[(off >> 1) + row];
	return int64_t(base + f);
}

bool dc_valid(uint desc, uint row)
{
	uint vo = ctl[desc + 1u];
	if (vo == DC_ALL_VALID)
		return true;
	if (vo == DC_ALL_NULL)
		return false;
	return ((a32[vo + (row >> 5)] >> (row & 31u)) & 1u) != 0u;
}
#endif
