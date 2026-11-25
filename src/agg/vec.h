/*
 * agg/vec.h - the physical column types and the 2048-row batch of the v6 aggregation engine.
 *
 * A column chunk decodes to a native array of one SiftVecType; a row set is a bitmap of uint64 words (bit i of word i / 64 =
 * row i, the bits >= len zero); validity is a bitmap too (bit set = not NULL), never a byte per row. The numeric (p, s) of a
 * column is a scaled integer: value * 10^s in int64 (p <= 18) or SiftI128 (p <= 38); a PostgreSQL Numeric is built once per
 * output group, never per input row.
 */
#ifndef SIFT_AGG_VEC_H
#define SIFT_AGG_VEC_H

#include "core/host.h"

#define SIFT_VEC_LEN		2048
#define SIFT_VEC_WORDS		(SIFT_VEC_LEN / 64)
#define SIFT_VEC_ALIGN		64
#define SIFT_VEC_PAD		64
/* a selection vector is written in groups of 8 indexes: its buffer has this many entries past SIFT_VEC_LEN */
#define SIFT_SEL_SLACK		8
/* the batch is kept as a bitmap while at least n_raw / SIFT_SEL_DENSE_DIV rows are active, then as a selection vector */
#define SIFT_SEL_DENSE_DIV	4

/* The physical element type of a decoded chunk (the logical type of a leaf is col/api/leaf.h SiftColType) */
typedef enum SiftVecType
{
	SVEC_BOOL,					/* uint8[n], 0 or 1 */
	SVEC_I32,					/* int32[n]: int2, int4, date */
	SVEC_I64,					/* int64[n]: int8, timestamp, timestamptz, time */
	SVEC_F64,					/* double[n]: float4 (widened on decode), float8 */
	SVEC_NUM64,					/* int64[n]: numeric(p <= 18, s) scaled by 10^s */
	SVEC_NUM128,				/* SiftI128[n]: numeric(19 <= p <= 38, s) scaled by 10^s */
	SVEC_UUID,					/* SiftUuid16[n] */
	SVEC_STRVIEW,				/* SiftStrView[n] */
	SVEC_DICT32,				/* uint32[n]: a dictionary code */
	SVEC_INTERVAL,				/* SiftInterval16[n] */
	SVEC_DATUM,					/* Datum[n]: arbitrary PostgreSQL type fallback (typid/typlen/typbyval on SiftVec) */
	SVEC_NUM256					/* SiftI256[n]: the scaled integer of a numeric product past 2^95 (agg/v6/expr/num.h); only the groups of SUM and AVG read it */
} SiftVecType;

typedef struct __attribute__((aligned(16))) SiftI128
{
	uint64		lo;
	int64		hi;
} SiftI128;

/* a signed integer below 2^191 in two's complement, l[0] the low limb; l[3] only extends the sign of l[2] */
typedef struct __attribute__((aligned(16))) SiftI256
{
	uint64		l[4];
} SiftI256;

typedef struct __attribute__((aligned(16))) SiftUuid16
{
	uint64		hi;				/* bytes 0..7, big endian: an unsigned compare of (hi, lo) is the uuid order */
	uint64		lo;				/* bytes 8..15 */
} SiftUuid16;

typedef struct __attribute__((aligned(16))) SiftInterval16
{
	int64		time;
	int32		day;
	int32		month;
} SiftInterval16;

/*
 * The first four bytes of a string sit next to its length in the first 8 bytes, whatever its length: a string of at most 12 bytes is prefix[4] + tail[0..7],
 * zero padded, so two inline views are equal when both 64-bit halves are; a longer one keeps its first four bytes there and a pointer (a granule buffer in
 * memory) or an offset (a shared slab) in the second half, so that a different length or prefix is told without a dereference.
 */
typedef struct __attribute__((aligned(16))) SiftStrView
{
	uint32		len;
	uint8		prefix[4];
	union
	{
		uint8		tail[8];
		const char *ptr;
		uint64		offset;
	}			u;
} SiftStrView;

_Static_assert(sizeof(SiftI128) == 16, "SiftI128 is 16 bytes");
_Static_assert(sizeof(SiftI256) == 32, "SiftI256 is 32 bytes");
_Static_assert(sizeof(SiftUuid16) == 16, "SiftUuid16 is 16 bytes");
_Static_assert(sizeof(SiftInterval16) == 16, "SiftInterval16 is 16 bytes");
_Static_assert(sizeof(SiftStrView) == 16, "SiftStrView is 16 bytes");

/* The fixed scale and width of a NUMERIC column chunk; has_special: the chunk has rows in its exception stream (NaN, +-Infinity, more than 38 digits) */
typedef struct SiftNumMeta
{
	int16		scale;
	uint8		precision;
	bool		has_special;
} SiftNumMeta;

/*
 * One column of up to SIFT_VEC_LEN rows. dense: the active rows are [0, len) and sel is NULL; else they are sel[0..len-1], strictly increasing uint16.
 * all_valid: valid is NULL and no active row is NULL; else valid is SIFT_VEC_WORDS words (bit set = not NULL, bits >= the row count zero).
 */
typedef struct SiftVec
{
	void	   *data;			/* SIFT_VEC_ALIGN aligned, SIFT_VEC_PAD readable bytes after the last element */
	uint64	   *valid;
	uint16	   *sel;
	uint16		len;
	bool		all_valid;
	bool		dense;
	SiftVecType type;
	int16		scale;
	const void *dict;
	const uint64 *nan;			/* a numeric: bit set = the row holds a NaN, which valid says is NULL; NULL when no row of the batch does */
	uint32		typid;			/* SVEC_DATUM: PostgreSQL Oid of the Datum values (InvalidOid for native vectors) */
	int16		typlen;			/* SVEC_DATUM: pg_type.typlen */
	bool		typbyval;		/* SVEC_DATUM: pg_type.typbyval */
} SiftVec;

/* The active rows of a batch of n_raw <= SIFT_VEC_LEN rows: the bitmap always, the selection vector once an operator asks for it */
typedef struct SiftBatchSel
{
	uint64		mask[SIFT_VEC_WORDS] __attribute__((aligned(64)));
	uint16		sel[SIFT_VEC_LEN + SIFT_SEL_SLACK] __attribute__((aligned(64)));
	uint16		n_raw;
	uint16		n_active;
	bool		all_active;
	bool		has_sel;
} SiftBatchSel;

typedef enum SiftVariantKind
{
	SVAR_JSONB_SCALAR,			/* a scalar of another type than the path's: a string in an int64 path */
	SVAR_NUM_SPECIAL,			/* NaN, +-Infinity or a numeric of more than 38 digits */
	SVAR_JSONB_DOC,				/* an object or array where a scalar was expected */
	SVAR_CAST_ERROR				/* a value whose cast raises if the row survives the predicates */
} SiftVariantKind;

/* One exception of a granule, sorted by row_idx; the payload of entry i is payload_buf[entries[i].payload_off, entries[i + 1].payload_off) */
typedef struct SiftVariantExcEntry
{
	uint16		row_idx;		/* in [0, 8191] */
	uint8		kind;			/* SiftVariantKind */
	uint8		reserved;
	uint32		payload_off;
} SiftVariantExcEntry;

_Static_assert(sizeof(SiftVariantExcEntry) == 8, "SiftVariantExcEntry is 8 bytes");

/* The exceptions of one granule (NULL where there are none): the typed sub-column holds every other row */
typedef struct SiftVariantExcStream
{
	uint16		n_entries;
	uint16		pad;
	uint32		payload_bytes;
	const uint64 *exc_mask;		/* 128 words: bit row_idx set = the row is an exception */
	const SiftVariantExcEntry *entries;	/* n_entries + 1, the last one only gives the end of the payload */
	const uint8 *payload_buf;
} SiftVariantExcStream;

/*
 * The elements of the rows of an array column: the parent rows in blocks of 64, so that the element mask of a row mask is built from runs and the
 * parent of an element is never stored. block_elem_off has one entry past the last block.
 */
typedef struct SiftArrayLayout
{
	uint32		n_parents;		/* <= 8192 */
	uint32		n_elems;
	const uint32 *block_elem_off;	/* [ceil(n_parents / 64) + 1] */
	const uint32 *lengths;		/* [n_parents] */
	const uint32 *offsets;		/* [n_parents + 1]: the prefix sums of lengths */
} SiftArrayLayout;

/* The kind of a member of an element of an array of objects, as the vector (SVEC_I64) of the members of a leaf gives it: it holds a value of the leaf's type, it is a JSON null, or it is missing; a member of another JSON type is no kind, the rows that have one are the consumer's to read from the heap */
#define SIFT_ELEM_KIND_ABSENT	0
#define SIFT_ELEM_KIND_VALUE	1
#define SIFT_ELEM_KIND_JNULL	7


#endif							/* SIFT_AGG_VEC_H */
