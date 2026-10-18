/*
 * sift_colcodec.h - the chunk codec of the column engine (docs/COL-CODEC.md, research in docs/perf/COMPRESSION.md).
 *
 * A chunk is the values of one column for the rows (or array elements) of one unit, as signed 64-bit keys: integers as they are,
 * floats as the order-preserving key of their IEEE bits (sift_col_f8_key; float4 keys are the keys of the promoted double), dictionary
 * codes as integers. sift_cc_encode() picks the smallest lightweight cascade (constant, frame of reference with bit-packing or plain
 * bytes, delta, run-length, sorted dictionary, decimal floats) per chunk from exact sizes and layers LZ4 or zstd on the streams where
 * the cost rule of the policy allows it; the result is a self-describing blob. The reader (sift_cc_open) validates the blob, and every
 * operation after that (decode, filter on the encoded form, value at a row) is memory safe on any bytes that passed it; a blob that
 * fails is ERRCODE_DATA_CORRUPTED, one that needs an algorithm this build lacks is ERRCODE_FEATURE_NOT_SUPPORTED.
 *
 * What is written never depends on the CPU: the format is the same bytes at every SIMD level and the chooser reads no CPU state. What is
 * read gives the same values at every level: the kernel tables below follow docs/KERNELS.md (the scalar table is the specification, AVX2
 * and AVX-512 are bit-identical, sift_cc_kernels_for() returns a level's table for tests, dispatch follows sift.simd through
 * sift_cpu_register()). Portable: sift_host.h and the compression library wrapper sift_colz.h only.
 */
#ifndef SIFT_COLCODEC_H
#define SIFT_COLCODEC_H

#include "sift_host.h"

#include "sift.h"
#include "sift_cpu.h"

/* ---------- format (version 1) ---------- */

#define SIFT_CC_MAGIC		0xC5
#define SIFT_CC_VERSION		1
#define SIFT_CC_HDRLEN		24		/* magic, version, kind, flags, u32 n, i64 min, i64 max; then the root node, then u32 check */
#define SIFT_CC_MAXDEPTH	4		/* nodes nested below the root node (a wrapper counts) */
#define SIFT_CC_MAXROWS		(1 << 24)

typedef enum SiftCcKind
{
	SIFT_CC_INT = 0,			/* keys are the integers */
	SIFT_CC_F4 = 1,				/* keys of promoted float4 values */
	SIFT_CC_F8 = 2				/* keys of float8 values */
} SiftCcKind;

#define SIFT_CCF_OFF32		0x1		/* header flag: max - min < 2^32, so offsets from min fit u32 */

/* Node tags (one byte). 10..31 are reserved for new encodings, 32..255 for experiments; an unknown tag is ERRCODE_FEATURE_NOT_SUPPORTED. */
#define SIFT_CCN_CONST		1
#define SIFT_CCN_FORBIT		2
#define SIFT_CCN_FORBYTE	3
#define SIFT_CCN_DELTA		4
#define SIFT_CCN_RLE		5
#define SIFT_CCN_DICT		6
#define SIFT_CCN_ALP		7
#define SIFT_CCN_WRAP		8
#define SIFT_CCN_SCALE		9		/* base + g * child: common divisor of the offsets from the minimum */

/* General-purpose algorithm ids of a WRAP node (the decoder is the id, the level is writer-side only). 3..15 reserved. */
#define SIFT_CC_ALG_NONE	0
#define SIFT_CC_ALG_LZ4		1
#define SIFT_CC_ALG_ZSTD	2
#define SIFT_CC_ALG_AUTO	0xFF	/* policy only: LZ4 or zstd, whichever the cost rule accepts */

/* ---------- cost rule of the general-purpose layer (docs/perf/COMPRESSION.md 5.4) ---------- */

#define SIFT_CC_NS_LZ4		0.25	/* measured ns per output byte of an unwrap */
#define SIFT_CC_NS_ZSTD		0.70
#define SIFT_CC_RHO_HOT		1.0		/* bytes one ns of per-scan decode must save: streams a scan filters */
#define SIFT_CC_RHO_COLD	0.15	/* dictionaries, string blobs, exception lists, checkpoint arrays */
#define SIFT_CC_MIN_GAIN	0.03	/* an explicit lz4/zstd policy still needs this share of the stream saved */
#define SIFT_CC_MIN_WRAP	128		/* streams below this many bytes are never wrapped */

/* ---------- policy: the column option `compression` ---------- */

typedef enum SiftCcMode
{
	SIFT_CCM_AUTO = 0,			/* cascade + cost rule */
	SIFT_CCM_NONE,				/* FORBYTE at the natural width, no wrapper: the engine's plain FOR chunk */
	SIFT_CCM_LAYER				/* cascade + the named algorithm on every stream that shrinks by SIFT_CC_MIN_GAIN */
} SiftCcMode;

typedef struct SiftCcPolicy
{
	uint8		mode;			/* SiftCcMode */
	uint8		alg;			/* SIFT_CC_ALG_*; with AUTO mode and ALG_AUTO the rule picks */
	uint8		level;			/* algorithm level, 0 = the default (lz4 1, zstd 3) */
	uint8		explicit_encs;	/* 1: only the encodings in encs (plus CONST and FORBYTE) */
	uint16		encs;			/* bit (1 << tag) per allowed lightweight encoding when explicit_encs */
} SiftCcPolicy;

/*
 * Parses one policy: auto | none | lz4[:N] | zstd[:N] | ENC[+ENC...][+lz4|zstd[:N]] with ENC in for, delta, rle, dict, alp, scale. Returns NULL
 * and fills *p, or an error message (a constant string) for the caller's ereport (ERRCODE_INVALID_PARAMETER_VALUE).
 */
extern const char *sift_cc_policy_parse(const char *spec, SiftCcPolicy *p);
/* The column option list "policy" or "policy; col=policy; ...": the policy of colname (the first entry without '=' is the default). */
extern const char *sift_cc_policy_for_column(const char *spec, const char *colname, SiftCcPolicy *p);
/* Text of a policy as sift_cc_policy_parse() reads it (palloc'd). */
extern char *sift_cc_policy_name(const SiftCcPolicy *p);
extern void sift_cc_policy_default(SiftCcPolicy *p);

/* ---------- writing ---------- */

typedef struct SiftCcInfo
{
	uint32		n;
	int64		min;			/* of the non-NULL keys; 0 when all NULL */
	int64		max;
	uint32		nnull;
	uint8		tag;			/* root node tag (8 = wrapped) */
	uint8		inner;			/* tag of the lightweight encoding under a wrapper */
	uint32		bytes;			/* length of the blob */
} SiftCcInfo;

/*
 * Appends the blob of keys[0..n) to out. nulls (bit set = NULL, whole 64-bit words, may be NULL) only tells the writer which slots are free:
 * they are overwritten by the previous non-NULL value so runs and deltas survive, and decode returns that filler there (the caller masks).
 * n <= SIFT_CC_MAXROWS. info may be NULL.
 */
extern void sift_cc_encode(StringInfo out, SiftCcKind kind, const int64 *keys, uint32 n, const uint64 *nulls, const SiftCcPolicy *pol,
						   SiftCcInfo *info);

/* ---------- reading ---------- */

typedef struct SiftCc SiftCc;

/* Validates the blob (header, lengths, widths, tags, sums; the check word when verify) and unwraps general-purpose layers into mcxt. */
extern SiftCc *sift_cc_open(MemoryContext mcxt, const uint8 *blob, Size len, bool verify);
extern void sift_cc_close(SiftCc *c);			/* frees what open allocated */
extern SiftCcKind sift_cc_kind(const SiftCc *c);
extern uint32 sift_cc_nrows(const SiftCc *c);
extern int64 sift_cc_min(const SiftCc *c);
extern int64 sift_cc_max(const SiftCc *c);
extern uint8 sift_cc_tag(const SiftCc *c);		/* tag of the root node under any wrapper */

/* dst[0..n) = the keys; dst needs n + SIFT_CC_PAD/8 slots */
extern void sift_cc_decode(const SiftCc *c, int64 *dst);
/* dst[k] = key of row idx[k], k < m (idx ascending or not, all < n) */
extern void sift_cc_decode_sel(const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst);
extern int64 sift_cc_get(const SiftCc *c, uint32 row);
/* The offsets key - min as u32 when SIFT_CCF_OFF32 (returns false otherwise, dst untouched); dst needs SIFT_CC_CAP32(n) slots. */
extern bool sift_cc_decode_off32(const SiftCc *c, uint32 *dst);
/*
 * bits = rows whose key is in [lo, hi] (signed), (n + 63) / 64 words, bits past n zero; NULL slots hold the filler, the caller ANDs the non-NULL
 * bits. Runs on the encoded form: constants and run-length chunks never expand, bit-packed chunks are unpacked and compared in
 * registers, dictionaries translate the range to codes, decimal floats to digits.
 */
extern void sift_cc_filter(const SiftCc *c, int64 lo, int64 hi, uint64 *bits);
/* Number of rows with bit set (all rows when bits is NULL) and the sum of their keys as wrapping int64; run-length and constant chunks never expand. */
extern uint64 sift_cc_sum(const SiftCc *c, const uint64 *bits, int64 *sum);

#define SIFT_CC_PAD			64		/* readable/writable bytes past the used end of arrays the kernels handle */
/* Slots of a u32 array that takes the writes of every kernel for n values: whole rows of 32 values plus the pad */
#define SIFT_CC_CAP32(n)	((((uint64) (n) + 31) / 32) * 32 + SIFT_CC_PAD / 4)

/* ---------- other blobs the engine stores ---------- */

/* A list of n strings (the dictionary of a string leaf): lengths cascade + concatenated bytes, the bytes wrapped under the cold rule. */
extern void sift_cc_encode_strs(StringInfo out, const uint32 *off, const char *bytes, uint32 n, const SiftCcPolicy *pol);
/* off[n + 1] and bytes palloc'd in mcxt; returns n */
extern uint32 sift_cc_decode_strs(MemoryContext mcxt, const uint8 *blob, Size len, uint32 **off, char **bytes);
/* A bit vector of nbits: constant, raw words, or the positions of the minority bit as an integer cascade. */
extern void sift_cc_encode_bits(StringInfo out, const uint64 *words, uint32 nbits, const SiftCcPolicy *pol);
extern void sift_cc_decode_bits(const uint8 *blob, Size len, uint32 nbits, uint64 *words);

/* ---------- kernels (docs/KERNELS.md) ---------- */

/*
 * FORBIT layout: blocks of 1024 values = 32 rows of 32 lanes; value i is row i / 32, lane i % 32; lane l is a bit stream of its rows (LSB
 * first, W bits each), word k of lane l is src[32 * k + l]. A full block takes 32 * W words, a tail of R < 32 rows takes 32 * ceil(R * W / 32).
 * "rows" below is the number of rows of 32 values (n / 32 rounded up); values past n are padding.
 */
typedef struct SiftCcKernels
{
	/* dst[i] = base + value i, i < 32 * rows (wrapping u32); W 0..32; dst needs 32 * rows slots */
	void		(*unpack32) (const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst);
	/* bit i of bits = ((value i - lo) <= span) unsigned wrapping, i < 32 * rows; writes ceil(32 * rows / 64) whole words */
	void		(*unpack_range32) (const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits);
	/* bit i of bits = ((a[i] - lo) <= span) for i < n, a of u8, u16, u32, u64; lo and span reduced to the width; writes ceil(n / 64) words, bits past n zero */
	void		(*range[4]) (const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);
	/* dst[i] = base + (int64) a[i] (a unsigned) */
	void		(*widen_add64) (const uint32 *a, uint32 n, int64 base, int64 *dst);
	/* dst[0] = first; dst[i] = dst[i - 1] + a[i] + add (wrapping), 1 <= i < n; a[0] is ignored */
	void		(*prefix32) (const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add);
	void		(*prefix64) (const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add);
	/* dst[i] = dict[codes[i]]; every code is below the dictionary size (the reader checked with max32) */
	void		(*gather32) (const uint32 *dict, const uint32 *codes, uint32 n, uint32 *dst);
	void		(*gather64) (const int64 *dict, const uint32 *codes, uint32 n, int64 *dst);
	uint32		(*max32) (const uint32 *a, uint32 n);
	/* dst[i] = key of ((double) (dbase + u[i])) / div, resp. the float4 ((float) (dbase + u[i])) / (float) div promoted to double */
	void		(*alp_keys_f8) (int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div);
	void		(*alp_keys_f4) (int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div);
	/* dst = the runs expanded: lens[r] copies of vals[r]; the total is exactly the caller's n (checked) */
	void		(*rle_expand32) (const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst);
} SiftCcKernels;

extern PGDLLIMPORT const SiftCcKernels *sift_cck;
/* A level's table, for tests; never NULL, falls back downwards for a level the CPU lacks */
extern const SiftCcKernels *sift_cc_kernels_for(SiftSimdLevel level);
/* Registers the rebind with the cpu module; _PG_init */
extern void sift_cc_kernels_init(void);

/*
 * Key transform of float8 (and promoted float4) values, the same bits as sift_col_f8_key of sift_col.h (-0 is +0, every NaN is one NaN above
 * +Inf, signed order = float order); the codec only needs it to turn decimal digits back into keys, and the tests compare both.
 */
static inline int64
sift_cc_f8_key(double f)
{
	uint64		b;

	if (f != f)
		b = UINT64CONST(0x7FF8000000000000);
	else
	{
		if (f == 0)
			f = 0;
		memcpy(&b, &f, sizeof(b));
	}
	return (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1));
}

static inline double
sift_cc_f8_from_key(int64 key)
{
	uint64		b = (uint64) key ^ ((uint64) (key >> 63) >> 1);
	double		f;

	memcpy(&f, &b, 8);
	return f;
}

#endif							/* SIFT_COLCODEC_H */
