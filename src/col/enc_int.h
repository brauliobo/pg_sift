/*
 * col/enc_int.h - shared between the files of L1 of the column engine (enc.c, schema.c, dict.c, batch.c, chunk.c, ctest.c, unit.c).
 */
#ifndef SIFT_ENC_INT_H
#define SIFT_ENC_INT_H

#include "col/enc.h"

#define COLU_MAGIC		0x554C4F43	/* "COLU" */
#define COLK_TABFIRST	(UINT32_C(1) << 24)	/* in the widths word of a COLK head: the chunk table follows the 24-byte head and the arrays of the directory follow the table (column format minor 34) */
#define COLK_MAGIC		0x4B4C4F43	/* "COLK": the unit of a key-cut segment, explicit TIDs */
#define CODEC_BLOCK		4096	/* values unpacked at a time (a multiple of 64) */

/* Appends the n values of v, bits each, LSB first, padded to a byte */
extern void put_packed(StringInfo out, const uint32 *v, uint32 n, int bits);

static inline uint32
words_of(uint32 n)
{
	return (n + 63) / 64;
}

static inline void
put_bytes(StringInfo out, const void *p, Size n)
{
	appendBinaryStringInfo(out, (const char *) p, (int) n);
}

#define put_val(out, v)	put_bytes(out, &(v), sizeof(v))

static inline bool
leaf_is_str(const SiftColDef *d)
{
	return d->store == SIFT_COLS_STR && d->kind == SIFT_COLK_SCALAR;
}

static inline uint32
cdict_len(const SiftCDict *d, uint32 code)
{
	return d->off[code + 1] - d->off[code];
}

static inline uint32
codec_slot(uint8 bits)
{
	return pg_rightmost_one_pos32(bits) - 3;	/* 8, 16, 32, 64 -> SIFT_COLW_8 .. SIFT_COLW_64 */
}

static inline bool
codec_valid(uint8 enc, uint8 bits)
{
	switch (enc)
	{
		case SIFT_COLENC_CONST:
			return bits == 0;
		case SIFT_COLENC_FOR:
			return bits == 8 || bits == 16 || bits == 32 || bits == 64;
		case SIFT_COLENC_PACK:
			return bits >= 1 && bits <= 31 && bits != 8 && bits != 16;
		case SIFT_COLENC_CC:
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			return bits == 0;
		default:
			return false;
	}
}

/* Payload bytes of n values */
static inline uint64
codec_paylen(uint8 enc, uint8 bits, uint32 n)
{
	if (enc == SIFT_COLENC_CONST)
		return 0;
	return enc == SIFT_COLENC_FOR ? (uint64) n * (bits / 8) : (uint64) words_of(n) * 8 * bits;
}

/* The bytes of the tag nibbles of nov overflow slots: half a byte each, padded to 8 */
static inline Size
tags_bytes(uint32 nov)
{
	return ((Size) (nov + 1) / 2 + 7) & ~(Size) 7;
}

/* dict.c */
extern void cdict_reset(SiftCDict *d);

/* chunk.c */
extern SiftCc *cc_open(const SiftCChunk *c, bool verify);
extern bool istr_split(const SiftCChunk *c, const uint8 **pre, uint32 *prelen, const uint8 **strs, uint32 *strslen);
extern uint32 bitmap_count(const uint64 *bits, uint32 n);

#endif							/* SIFT_ENC_INT_H */
