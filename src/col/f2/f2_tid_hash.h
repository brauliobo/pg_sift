/*
 * col/f2/f2_tid_hash.h - SEC_TID_HASH (0x0103), an optional section of a part of level 1 and up of a sift_table whose rows are in key order: a hash table from an address (the ctid of a
 * row) to the rows that hold it (granule << 13 | row in it), so that a lookup of one address reads two pages of the section and the granule of the row, and not every granule whose envelope
 * of addresses covers it (a part in key order has granules whose envelopes all cover the same range). A reader that does not know the section ignores it, and a part that holds it is not
 * joined as it is (col/granule/part_join.c).
 *
 *   SiftTidHashHdr, then nbuckets + 1 entry numbers, then nrows entries (the rows, ascending in a bucket)
 *
 * The buckets are a power of two, the least that gives a load of SIFT_TID_HASH_LOAD rows a bucket, so that the reader finds the table from the rows of the part alone; the hash is
 * sift_fmix64 of the address and never changes.
 */
#ifndef SIFT_F2_TID_HASH_H
#define SIFT_F2_TID_HASH_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "port/pg_bitutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/part_format.h"
#include "col/granule/granule.h"
#include "core/hash.h"

#define SIFT_TID_HASH_MAGIC		0x48444954	/* "TIDH" */
#define SIFT_TID_HASH_VERSION	1
#define SIFT_TID_HASH_LOAD		4
#define SIFT_TID_HASH_MIN_ROWS	4096
#define SIFT_TID_HASH_ROW_BITS	13

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

static inline uint32
sift_tid_hash_log2(uint64 nrows)
{
	return (uint32) Max(1, pg_ceil_log2_64(nrows / SIFT_TID_HASH_LOAD));
}

static inline uint32
sift_tid_hash_bucket(uint64 tid, uint32 log2_buckets)
{
	return (uint32) (sift_fmix64(tid) >> (64 - log2_buckets));
}

/* The bytes of the section of a part of level, whose rows are in key order of a sift_table, or false when it has none */
extern bool sift_tid_hash_build(const SiftRows *rows, uint16 level, const int *gstart, int ng, StringInfo out);

/* The granules of the part that may hold the address, ascending: *gids is a palloc'd array of them (n), from the two pages of the section the address names: a bucket holds about SIFT_TID_HASH_LOAD rows, the granules of other addresses among them */
extern int	sift_tid_hash_granules(Relation rel, const SiftPart *part, uint64 nrows, uint64 tid, uint32 **gids);

#endif							/* SIFT_F2_TID_HASH_H */
