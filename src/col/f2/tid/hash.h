/*
 * col/f2/tid/hash.h - SEC_TID_HASH (0x0103), an optional section of a part of level 1 and up of a sift_table that has an index: a hash table from an address (the ctid of a row) to the
 * row that holds it, so that a lookup of one address reads two pages of the section and then the granule or the row-pack block of the row, and not every granule whose envelope of addresses
 * covers it (a part in key order has granules whose envelopes all cover the same range). A reader that does not know the section ignores it, and a part that holds it is not joined as it is
 * (col/granule/part/join.c).
 *
 *   SiftTidHashHdr, then nbuckets + 1 entry numbers, then nrows entries (the rows, ascending in a bucket)
 *
 * Version 2 (what is written): an entry is SiftTidHashSlotV2, the row of the part with a 16-bit tag of the address and its granule, a bucket holds SIFT_TID_HASH_V2_LOAD rows on average, and
 * the tag drops the other rows of the bucket, so a lookup names the granule and the row, not a bucket of granules. Version 1 (an entry is granule << 13 | row in it, load 4, no tag) is
 * still read. The buckets are a power of two, which the header names; the hash is sift_fmix64 of the address and never changes. The tag is never trusted alone: a reader checks the address
 * in the row it names.
 */
#ifndef SIFT_F2_TID_HASH_H
#define SIFT_F2_TID_HASH_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "port/pg_bitutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/part/format.h"
#include "col/granule/granule.h"
#include "core/hash.h"

#define SIFT_TID_HASH_MAGIC		0x48444954	/* "TIDH" */
#define SIFT_TID_HASH_VER_V1	1
#define SIFT_TID_HASH_VER_V2	2
#define SIFT_TID_HASH_V1_LOAD	4
#define SIFT_TID_HASH_V2_LOAD	2
#define SIFT_TID_HASH_MIN_ROWS	4096
#define SIFT_TID_HASH_ROW_BITS	13
#define SIFT_TID_HASH_FLAG_RPACK	(UINT16_C(1) << 0)	/* the row of an entry is the row of the part's SEC_ROW_PACK */
#define SIFT_TID_HASH_NO_ROW	UINT32_MAX	/* the row of a hit of a version 1 section: not known */

StaticAssertDecl(SIFT_GRANULE_ROWS <= (1 << SIFT_TID_HASH_ROW_BITS), "a row of a granule is 13 bits");

typedef struct SiftTidHashHdr
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		log2_buckets;
	uint32		nrows;
} SiftTidHashHdr;

StaticAssertDecl(sizeof(SiftTidHashHdr) == 16, "the address hash header is 16 bytes");
StaticAssertDecl(offsetof(SiftTidHashHdr, log2_buckets) == 8, "log2_buckets is at byte 8");

typedef struct SiftTidHashSlotV2
{
	uint32		row_idx;		/* the row of the part, from 0 */
	uint16		fp16;			/* the low 16 bits of the hash of the address */
	uint16		granule_id;
} SiftTidHashSlotV2;

StaticAssertDecl(sizeof(SiftTidHashSlotV2) == 8, "an address hash entry is 8 bytes");
StaticAssertDecl(offsetof(SiftTidHashSlotV2, fp16) == 4, "fp16 is at byte 4");
StaticAssertDecl(offsetof(SiftTidHashSlotV2, granule_id) == 6, "granule_id is at byte 6");

typedef struct SiftTidHashHit
{
	uint32		row_idx;		/* SIFT_TID_HASH_NO_ROW for a version 1 section */
	uint32		granule_id;
} SiftTidHashHit;

static inline uint32
sift_tid_hash_log2(uint64 nrows, uint32 load)
{
	return (uint32) Max(1, pg_ceil_log2_64(nrows / load));
}

static inline uint32
sift_tid_hash_bucket(uint64 tid, uint32 log2_buckets)
{
	return (uint32) (sift_fmix64(tid) >> (64 - log2_buckets));
}

static inline uint16
sift_tid_hash_fp(uint64 tid)
{
	return (uint16) (sift_fmix64(tid) & 0xFFFF);
}

/* The bytes of the section of a part of level, whose rows are in key order or whose SEC_ROW_PACK is built (rpack), of a sift_table that has an index, or false when it has none */
extern bool sift_tid_hash_build(const SiftRows *rows, uint16 level, const int *gstart, int ng, bool rpack, StringInfo out);

/* The rows of the part that may hold the address, ascending: *hits is a palloc'd array of them (n), from the two pages of the section the address names. A version 2 section names the rows whose tag is the address's; a version 1 section names the granules of a bucket (no row) */
extern int	sift_tid_hash_find(Relation rel, const SiftPart *part, uint64 nrows, uint64 tid, SiftTidHashHit **hits);

/* Every bucket and entry of the directory of the part (nrows rows) is as a writer makes it: SIFT_DAMAGE_DATA otherwise */
extern void sift_tid_hash_check(Relation rel, const SiftPart *part, uint64 nrows);

#endif							/* SIFT_F2_TID_HASH_H */
