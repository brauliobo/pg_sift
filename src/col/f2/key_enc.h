/*
 * col/f2/key_enc.h - a sort key of up to eight columns as bytes whose memcmp order is the order of the key. A column is a tag byte (the NULLs before or after the values, as key_order says) and
 * its value in an encoding that no other value's is a prefix of, so that the encodings of the columns laid one after the other order tuples lexicographically; a descending column has the bytes
 * of its value complemented. The marks of a part (col/f2/key_marks.h) and the ranges a scan derives from its predicates (col/f2/key_plan.h) are made of these bytes.
 *
 *   bool, int2, int4, int8     big endian, the sign bit flipped (date is int4; timestamp and timestamptz are int8)
 *   float4, float8             the IEEE bits with the sign handling that orders them; -0 is 0, every NaN is one NaN, greater than every number
 *   uuid                       the 16 bytes
 *   numeric                    from its decimal text: a tag of the sign (-Infinity, negative, zero, positive, Infinity, NaN), the exponent and the digits, ended by a byte below every digit
 *   text, varchar, dictionary  the bytes with 0x00 as 0x00 0xFF, ended by 0x00 0x01 (the order of the C collation; the caller keeps other collations out)
 */
#ifndef SIFT_KEY_ENC_H
#define SIFT_KEY_ENC_H

#include "core/host.h"

#define SIFT_KEY_COLS_MAX		8		/* SIFT_KEY_ORDER_MAX of core/range.h */
#define SIFT_KEY_BYTES_MAX		40
#define SIFT_KEY_BYTES_MIN		8

/* The flags of a key column: those of SIFT_KEY_DESC and SIFT_KEY_NULLS_FIRST of core/range.h */
#define SIFT_KEYCOL_DESC		1
#define SIFT_KEYCOL_NULLS_FIRST	2

typedef enum SiftKeyKind
{
	SIFT_KK_BOOL = 1,
	SIFT_KK_INT2,
	SIFT_KK_INT4,
	SIFT_KK_INT8,
	SIFT_KK_FLOAT4,
	SIFT_KK_FLOAT8,
	SIFT_KK_UUID,
	SIFT_KK_NUMERIC,
	SIFT_KK_TEXT
} SiftKeyKind;

#define SIFT_KK_DATE			SIFT_KK_INT4
#define SIFT_KK_TIMESTAMP		SIFT_KK_INT8
#define SIFT_KK_TIMESTAMPTZ		SIFT_KK_INT8

typedef struct SiftKeyCol
{
	uint8		kind;			/* SiftKeyKind */
	uint8		flags;			/* SIFT_KEYCOL_* */
} SiftKeyCol;

typedef struct SiftKeySpec
{
	uint8		ncols;			/* 1 .. SIFT_KEY_COLS_MAX */
	uint8		key_bytes;		/* bytes of a tuple kept in a mark: a multiple of 8 in SIFT_KEY_BYTES_MIN .. SIFT_KEY_BYTES_MAX */
	SiftKeyCol	cols[SIFT_KEY_COLS_MAX];
} SiftKeySpec;

/* A value of a key column: i for bool and the integers, f for the floats (a float4 promoted), p and len for the 16 bytes of a uuid, the bytes of a text, the decimal text of a numeric */
typedef struct SiftKeyVal
{
	bool		isnull;
	int64		i;
	double		f;
	const uint8 *p;
	uint32		len;
} SiftKeyVal;

/* A spec the marks can be made with: false for a column count, a stride or a kind that is none */
extern bool sift_key_spec_valid(const SiftKeySpec *s);

/*
 * The encoding of a value of column c, at most cap bytes of it written to out: the number of bytes written. *truncated is set when the value takes more than cap (it is not cleared otherwise).
 * A malformed numeric text or a uuid that is not 16 bytes is an error.
 */
extern uint32 sift_key_enc_val(const SiftKeyCol *c, const SiftKeyVal *v, uint8 *out, uint32 cap, bool *truncated);

/* The first n components of a tuple, in the key_bytes of out zero padded (what a mark holds); the bytes written before the padding; *truncated: the tuple takes more than key_bytes */
extern uint32 sift_key_enc_tuple(const SiftKeySpec *s, const SiftKeyVal *vals, int n, uint8 *out, bool *truncated);

/* The order of two values of a column as the values go (not as the flags of the column order them): below zero, zero, above zero */
extern int	sift_key_val_cmp(const SiftKeyCol *c, const SiftKeyVal *a, const SiftKeyVal *b);

/* The tag bytes: a NULL of the column and a value of it */
extern uint8 sift_key_null_tag(const SiftKeyCol *c);
#define SIFT_KEY_VALUE_TAG		0x01

#endif							/* SIFT_KEY_ENC_H */
