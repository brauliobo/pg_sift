/*
 * col/codec_int.h - shared between col/codec.c and the per-level kernel files (the vertical bit-packing layout and the tables), and between
 * the files of the codec itself: codec.c (decode, filters), codec_parse.c, codec_write.c, codec_policy.c, codec_blob.c.
 */
#ifndef SIFT_COLCODEC_INT_H
#define SIFT_COLCODEC_INT_H

#include "col/codec.h"

extern const SiftCcKernels sift_cc_kernels_scalar;
extern const SiftCcKernels sift_cc_kernels_avx2;
extern const SiftCcKernels sift_cc_kernels_avx512;

/* Rows of 32 values for n values */
#define SIFT_CC_ROWS(n)		(((uint64) (n) + 31) >> 5)

/* u32 words of n values bit-packed at width W in the vertical layout: full blocks of 32 rows take 32 * W, a tail of R rows 32 * ceil(R * W / 32) */
static inline uint64
sift_cc_words(uint32 n, uint32 W)
{
	uint64		rows = SIFT_CC_ROWS(n),
				w = (rows / 32) * 32 * W;

	if (rows % 32)
		w += 32 * (((rows % 32) * W + 31) / 32);
	return w;
}

static inline uint32
sift_cc_bits_of(uint64 x)
{
	uint32		w = 0;

	for (; x; x >>= 1)
		w++;
	return w;
}

static inline uint32
sift_cc_ld32(const uint32 *p)
{
	uint32		x;

	memcpy(&x, p, sizeof(x));
	return x;
}

static inline uint64
sift_cc_ld64(const void *p)
{
	uint64		x;

	memcpy(&x, p, sizeof(x));
	return x;
}

/* The decimal exponents of the ALP node: 10^e exactly as double and float (e <= 18, resp. 10) */
extern const double sift_cc_p10[19];
extern const float sift_cc_p10f[11];
#define SIFT_CC_ALP_EMAX_F8	18
#define SIFT_CC_ALP_EMAX_F4	10

#define CC_CORRUPT(...) \
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: " __VA_ARGS__)))

#define CC_BIT(tag)			(1 << (tag))
#define CC_NHDR				16
#define CC_RUP8(x)			(((x) + 7) & ~(size_t) 7)
#define CC_CAP32(n)			((size_t) SIFT_CC_CAP32(n))

static inline uint32
get32(const uint8 *p)
{
	uint32		x;

	memcpy(&x, p, 4);
	return x;
}

static inline int64
get64(const uint8 *p)
{
	int64		x;

	memcpy(&x, p, 8);
	return x;
}

static inline void
clear_tail(uint64 *bits, uint32 n)
{
	if (n % 64)
		bits[n / 64] &= (UINT64CONST(1) << (n % 64)) - 1;
}

static inline uint32
rows_for(uint32 n)
{
	return (uint32) SIFT_CC_ROWS(n);
}

typedef enum CcRole
{
	CCR_HOT,					/* streams a scan filters */
	CCR_COLD					/* dictionaries, blobs, exception lists */
} CcRole;

typedef struct CcW
{
	const SiftCcPolicy *pol;
	SiftCcKind	kind;			/* of the root node; children are plain integers */
} CcW;

typedef struct CcZTry
{
	int			alg;
	char	   *buf;
	Size		len;
	double		cost;
} CcZTry;

typedef struct CcNode
{
	uint8		tag;
	uint8		alg;
	uint8		kind;
	uint8		W,
				B,
				e;
	uint32		n;
	int64		base,			/* FORBIT, FORBYTE; CONST value; DELTA first */
				mind,
				dmin,
				dmax;
	uint32		nruns,
				d,
				nexc;
	const uint8 *words;			/* packed words (FORBIT, DELTA) or bytes (FORBYTE) */
	const uint8 *chk;
	const uint8 *ekeys;
	struct CcNode *a,
			   *b;
	uint32	   *rstart;			/* RLE: nruns + 1 */
	int64	   *dict;			/* DICT: d, ascending */
	uint32	   *pos;			/* ALP: nexc, ascending */
} CcNode;

struct SiftCc
{
	MemoryContext mcxt;
	SiftCcKind	kind;
	uint32		n;
	int64		min,
				max;
	uint8		flags;
	CcNode	   *root;
};

static inline const CcNode *
unwrap(const CcNode *nd)
{
	while (nd->tag == SIFT_CCN_WRAP)
		nd = nd->a;
	return nd;
}

/* The writer (codec_write.c) the blobs use */
extern void put32(StringInfo s, uint32 x);
extern void pad8(StringInfo s);
extern void check_policy(const SiftCcPolicy *pol);
extern bool wrap_choose(const CcW *w, const uint8 *raw, uint32 rawlen, CcRole role, CcZTry *best);
extern int	enc_node(const CcW *w, StringInfo out, const int64 *v, uint32 n, int depth, CcRole role);

/* The parser of a chunk (codec_parse.c) the blobs use */
extern void need(size_t have, size_t want);
extern CcNode *parse_node(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t avail, uint32 n, int depth, size_t *used);

/* Decoding and the bit helpers (codec.c) the parser and the blobs use */
extern void nd_u32(const CcNode *nd, int64 sub, uint32 *dst);
extern void nd_i64(const CcNode *nd, int64 *dst);
extern void sift_cc_set_ones(uint64 *bits, uint32 n);

#endif							/* SIFT_COLCODEC_INT_H */
