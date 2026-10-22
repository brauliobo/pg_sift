/*
 * col/codec.h - the chunk codec of the column engine.
 *
 * A chunk is the values of one column for the rows (or array elements) of one unit, as signed 64-bit keys: integers as they are,
 * floats as the order-preserving key of their IEEE bits (sift_col_f8_key of col/col.h; float4 keys are the keys of the promoted double), dictionary
 * codes as integers. sift_cc_encode() picks the smallest lightweight cascade (constant, frame of reference with bit-packing or plain
 * bytes, delta, run-length, sorted dictionary, decimal floats) per chunk from exact sizes and layers LZ4 or zstd on the streams where
 * the cost rule of the policy allows it; the result is a self-describing blob. The reader (sift_cc_open) validates the blob, and every
 * operation after that (decode, filter on the encoded form, value at a row) is memory safe on any bytes that passed it; a blob that
 * fails is ERRCODE_DATA_CORRUPTED, one that needs an algorithm this build lacks is ERRCODE_FEATURE_NOT_SUPPORTED.
 *
 * What is written never depends on the CPU: the format is the same bytes at every SIMD level and the chooser reads no CPU state. What is
 * read gives the same values at every level: the kernel tables follow the design notes (the scalar table is the specification, AVX2 and
 * AVX-512 are bit-identical, dispatch follows sift.simd through sift_cpu_register()). Filters, widening and dictionary gathers run on
 * the column engine's table (sift_colk, col/kern.h); the table below holds what only the codec needs (the vertical bit-packing, prefix
 * sums, run expansion, decimal floats). Portable except for the compression library wrapper col/compress.h: core/host.h and portable files.
 */
#ifndef SIFT_COLCODEC_H
#define SIFT_COLCODEC_H

#include "core/host.h"

#include "core/sift.h"
#include "core/cpu.h"
#include "col/col.h"

/* ---------- format (version 1) ---------- */

#define SIFT_CC_MAGIC		0xC5
#define SIFT_CC_VERSION		1
#define SIFT_CC_HDRLEN		24		/* magic, version, kind, flags, u32 n, i64 min, i64 max; then the root node, then u32 check */
#define SIFT_CC_MAXDEPTH	4		/* nodes nested below the root node (a wrapper counts); the writer cascades MAXDEPTH - 1 levels, then wraps a leaf */
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

/* ---------- cost rule of the general-purpose layer ---------- */

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
extern SiftCc *sift_cc_open_rows(MemoryContext mcxt, const uint8 *blob, Size len, bool verify, uint32 nrows);	/* the same, for a blob of exactly nrows values */
extern void sift_cc_close(SiftCc *c);			/* frees what open allocated */
extern SiftCcKind sift_cc_kind(const SiftCc *c);
extern uint32 sift_cc_nrows(const SiftCc *c);
extern int64 sift_cc_min(const SiftCc *c);
extern int64 sift_cc_max(const SiftCc *c);
extern uint8 sift_cc_tag(const SiftCc *c);		/* tag of the root node under any wrapper */

/* dst[0..n) = the keys; dst needs n + SIFT_COL_PAD/8 slots */
extern void sift_cc_decode(const SiftCc *c, int64 *dst);
/* dst[i] = the low 32 bits of key i (the keys of an int4, a date, a dictionary code); dst needs SIFT_CC_CAP32(n) slots */
extern void sift_cc_decode_u32(const SiftCc *c, uint32 *dst);
/* dst[i] = the double whose key (sift_col_f8_key) is key i, of a F4 or F8 chunk; an ALP chunk never forms the keys; dst needs n + SIFT_COL_PAD/8 slots */
extern void sift_cc_decode_f8(const SiftCc *c, double *dst);
/* dst[k] = key of row idx[k], k < m (idx ascending or not, all < n) */
extern void sift_cc_decode_sel(const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst);
extern int64 sift_cc_get(const SiftCc *c, uint32 row);
/* The offsets key - min as u32 when SIFT_CCF_OFF32 (returns false otherwise, dst untouched); dst needs SIFT_CC_CAP32(n) slots. */
extern bool sift_cc_decode_off32(const SiftCc *c, uint32 *dst);
/* An upper bound of the offsets that writes, from the root node alone (what the blob holds is not read); UINT32_MAX when the nodes do not tell */
extern uint32 sift_cc_off32_bound(const SiftCc *c);
/*
 * bits = rows whose key is in [lo, hi] (signed), (n + 63) / 64 words, bits past n zero; bits has room for SIFT_COL_NWORDS(n) words, the column
 * kernels it runs on write the padded vector (the words past the first are zero). NULL slots hold the filler, the caller
 * ANDs the non-NULL bits. Runs on the encoded form: constants and run-length chunks never expand, bit-packed chunks are unpacked and compared in
 * registers, dictionaries translate the range to codes, decimal floats to digits.
 */
extern void sift_cc_filter(const SiftCc *c, int64 lo, int64 hi, uint64 *bits);
/* Number of rows with bit set (all rows when bits is NULL) and the sum of their keys as wrapping int64; run-length and constant chunks never expand. */
extern uint64 sift_cc_sum(const SiftCc *c, const uint64 *bits, int64 *sum);

/* Slots of a u32 array that takes the writes of every kernel for n values: whole rows of 32 values plus the pad (SIFT_COL_PAD bytes) */
#define SIFT_CC_CAP32(n)	((((uint64) (n) + 31) / 32) * 32 + SIFT_COL_PAD / 4)

/* ---------- other blobs the engine stores ---------- */

/* A list of n strings (the dictionary of a string leaf): lengths cascade + concatenated bytes, the bytes wrapped under the cold rule. */
extern void sift_cc_encode_strs(StringInfo out, const uint32 *off, const char *bytes, uint32 n, const SiftCcPolicy *pol);
/* The list of exactly n strings (its count is compared before anything is allocated for it); off[n + 1] and bytes palloc'd in mcxt */
extern void sift_cc_decode_strs(MemoryContext mcxt, const uint8 *blob, Size len, uint32 n, uint32 **off, char **bytes);
/* A bit vector of nbits: constant, raw words, or the positions of the minority bit as an integer cascade. */
extern void sift_cc_encode_bits(StringInfo out, const uint64 *words, uint32 nbits, const SiftCcPolicy *pol);
extern void sift_cc_decode_bits(const uint8 *blob, Size len, uint32 nbits, uint64 *words);

/* ---------- kernels ---------- */

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
	/* dst[0] = first; dst[i] = dst[i - 1] + a[i] + add (wrapping), 1 <= i < n; a[0] is ignored */
	void		(*prefix32) (const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add);
	void		(*prefix64) (const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add);
	/* max of a (0 when n is 0): the reader checks the codes of a dictionary against its size before the column kernels gather with them */
	uint32		(*max32) (const uint32 *a, uint32 n);
	/* dst[i] = key of ((double) (dbase + u[i])) / div, resp. the float4 ((float) (dbase + u[i])) / (float) div promoted to double */
	void		(*alp_keys_f8) (int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div);
	void		(*alp_keys_f4) (int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div);
	/* dst = the runs expanded: lens[r] copies of vals[r]; the total is exactly the caller's n (checked) */
	void		(*rle_expand32) (const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst);
	/* a[i] = base + mul * a[i], i < n, wrapping: the SCALE node on the 32-bit and the 64-bit domain */
	void		(*scale32) (uint32 *a, uint32 n, uint32 base, uint32 mul);
	void		(*scale64) (int64 *a, uint32 n, int64 base, int64 mul);
	/* the doubles alp_keys_f8 / alp_keys_f4 give the keys of: dst[i] = ((double) (dbase + u[i])) / div, resp. the float4 division promoted */
	void		(*alp_f8) (double *dst, const uint32 *u, uint32 n, int64 dbase, double div);
	void		(*alp_f4) (double *dst, const uint32 *u, uint32 n, int64 dbase, float div);
} SiftCcKernels;

extern PGDLLIMPORT const SiftCcKernels *sift_cck;
/* A level's table, for tests; never NULL, falls back downwards for a level the CPU lacks */
extern const SiftCcKernels *sift_cc_kernels_for(SiftSimdLevel level);
/* Registers the rebind with the cpu module; _PG_init */
extern void sift_cc_kernels_init(void);

#endif							/* SIFT_COLCODEC_H */
