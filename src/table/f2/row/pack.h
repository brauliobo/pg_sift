/*
 * table/f2/row/pack.h - SEC_ROW_PACK (0x0104), an optional section of a part of level 1 and up of a sift_table that has an index: the rows of the part in blocks of 64, each a small row store
 * (the address of a row, its null bits, its fixed-width values, then its strings) compressed with LZ4, so that the fetch of one row by address reads one block of about 2 KB and decodes the
 * columns it asks for, not one chunk of each column of a granule. Analytical scans never read it. A string over SIFT_ROW_PACK_INLINE_MAX bytes, a value a column cannot hold (a wide value kept
 * by reference) and a string of a column of exceptions are not in it (the length 0xFFFF): the reader takes that cell from SEC_GRANULES.
 *
 *   SiftRowPackHdr, SiftRowPackCol[ncols], SiftRowPackBlockEntry[nblocks] (at dir_off), the blocks (8-aligned, from data_off)
 *
 * A block, uncompressed: uint32 row_off[nrows + 1] (from the start of the block), then the rows. A row: uint64 tid, uint8 null_bm[(ncols + 7) / 8] (a set bit: NULL), uint16 fixed_bytes,
 * the non-null integer and float columns in column order (an integer is its distance from SiftRowPackCol.base in `width` bytes, a float is 8 bytes), then the non-null string columns in
 * column order (uint16 length, or 0xFFFF, and the bytes). The columns are those of the part, in the order of its SEC_SCHEMA. The section is written only when it is at most
 * SIFT_ROW_PACK_SHARE_PERCENT of SEC_GRANULES (measured on the first SIFT_ROW_PACK_SAMPLE_BLOCKS blocks) and the part has SEC_TID_HASH, whose entries name the rows of the pack.
 */
#ifndef SIFT_TBL_F2_ROW_PACK_H
#define SIFT_TBL_F2_ROW_PACK_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "col/f2/gbuf/gbuf.h"

#define SIFT_ROW_PACK_MAGIC			0x52504B32U	/* "RPK2" */
#define SIFT_ROW_PACK_VER			1
#define SIFT_ROW_PACK_ROWS			64
#define SIFT_ROW_PACK_INLINE_MAX	128
#define SIFT_ROW_PACK_EXT_SENTINEL	UINT16_C(0xFFFF)
#define SIFT_ROW_PACK_SHARE_PERCENT	40
#define SIFT_ROW_PACK_SAMPLE_BLOCKS	4

#define SIFT_RPACK_CODEC_RAW		0
#define SIFT_RPACK_CODEC_LZ4		1
#define SIFT_RPACK_CODEC_MASK		UINT16_C(0xF)

#define SIFT_RPACK_FLAG_NONE		UINT16_C(0)
#define SIFT_RPACK_FLAG_HAS_WIDE	(UINT16_C(1) << 0)

typedef struct SiftRowPackHdr
{
	uint32		magic;
	uint16		version;
	uint16		flags;			/* SIFT_RPACK_FLAG_* */
	uint16		ncols;			/* the columns of the part */
	uint16		rows_per_blk;	/* SIFT_ROW_PACK_ROWS */
	uint32		nblocks;
	uint32		nrows;
	uint32		dir_off;		/* from the start of the section to SiftRowPackBlockEntry[nblocks] */
	uint64		data_off;		/* from the start of the section to the blocks */
} SiftRowPackHdr;

StaticAssertDecl(sizeof(SiftRowPackHdr) == 32, "SiftRowPackHdr must be 32 bytes");
StaticAssertDecl(offsetof(SiftRowPackHdr, nblocks) == 12, "nblocks is at byte 12");
StaticAssertDecl(offsetof(SiftRowPackHdr, data_off) == 24, "data_off is at byte 24");

typedef struct SiftRowPackBlockEntry
{
	uint32		off_qwords;		/* from data_off, in 8-byte units */
	uint32		comp_len;		/* the bytes on disk (raw_len with the raw codec) */
	uint32		raw_len;
	uint16		nrows;			/* 1..SIFT_ROW_PACK_ROWS */
	uint16		codec_flags;	/* bits 0..3: SIFT_RPACK_CODEC_*; the others 0 */
} SiftRowPackBlockEntry;

StaticAssertDecl(sizeof(SiftRowPackBlockEntry) == 16, "SiftRowPackBlockEntry must be 16 bytes");
StaticAssertDecl(offsetof(SiftRowPackBlockEntry, raw_len) == 8, "raw_len is at byte 8");
StaticAssertDecl(offsetof(SiftRowPackBlockEntry, codec_flags) == 14, "codec_flags is at byte 14");

/* How a column of the part is stored: kind a SiftCellKind; width the bytes of an integer (1, 2, 4 or 8); base what an integer's stored distance is from */
typedef struct SiftRowPackCol
{
	uint8		kind;
	uint8		width;
	uint16		pad16;
	uint32		pad32;
	int64		base;
} SiftRowPackCol;

StaticAssertDecl(sizeof(SiftRowPackCol) == 16, "SiftRowPackCol must be 16 bytes");

/* The bytes of the section of a part written from rows (SiftRows), or false when the part gets none: a column of arrays of objects or an exception in an integer or float column, or the pack would be over SIFT_ROW_PACK_SHARE_PERCENT of the granules_len bytes of SEC_GRANULES */
extern bool sift_row_pack_build(const SiftRows *rows, uint64 granules_len, StringInfo out);

/* A block of the part, decompressed: one allocation in the current memory context, `len` bytes of rows behind its columns */
typedef struct SiftRowPackBlk
{
	uint32		nrows;
	uint32		len;
	uint16		ncols;
	uint16		pad;
} SiftRowPackBlk;

extern SiftRowPackBlk *sift_row_pack_read_block(Relation rel, const SiftPart *part, uint32 blk);

/* Every block of the pack of the part (nrows rows) decodes, and its rows are the rows of the part: SIFT_DAMAGE_DATA otherwise */
extern void sift_row_pack_check(Relation rel, const SiftPart *part, uint64 nrows);

/* The size of a decoded block, for the budget of the cache */
extern Size sift_row_pack_blk_bytes(const SiftRowPackBlk *b);

/* Whether the row of the block holds the address (the tag of the hash is no proof of it) */
extern bool sift_row_pack_holds(const SiftRowPackBlk *b, uint32 row, uint64 tid);

/* One row of a block, copied to the current memory context with its columns and the places of its values */
typedef struct SiftRowPackRow SiftRowPackRow;

extern SiftRowPackRow *sift_row_pack_row(const SiftRowPackBlk *b, uint32 row);

typedef enum SiftRowPackCell
{
	SIFT_RPACK_NULL,
	SIFT_RPACK_VALUE,
	SIFT_RPACK_EXTERNAL			/* a cell of SEC_GRANULES */
} SiftRowPackCell;

/* The value of column col of the part (an integer, a float or a string, in cell), as the row has it; a string points into the row */
extern SiftRowPackCell sift_row_pack_cell(const SiftRowPackRow *r, int col, SiftF2Cell *cell);

#endif							/* SIFT_TBL_F2_ROW_PACK_H */
