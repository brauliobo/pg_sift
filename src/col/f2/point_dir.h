/*
 * col/f2/point_dir.h - SEC_POINT (0x0209), an optional section of a part of level 1 and up of an index (keyed or not): for each of at most SIFT_POINT_MAX_COLS keys whose values are nearly
 * all distinct, a hash table from a value to the rows that hold it (granule << 13 | row in it), so that a scan for one value or a few reads three pages of the section and the granules
 * of the rows it names, and not every granule of the part. A key is one column (an integer, a uuid, a text) or two to four columns whose values together are nearly all distinct. The lead
 * column of a keyed part has its marks (SEC_KEY_MARKS) and no directory. A reader that does not know the section, or its version, ignores it, and a part that holds it is joined with others
 * only by a rewrite; a part that moves alone carries it as it is (col/granule/part/join.c).
 *
 *   SiftPointHdr, then one SiftPointCol for each key, then for each key the bucket table (nbuckets + 1 entry numbers) and its entries (the rows, ascending in a bucket)
 *
 * The hashes are those of the format and never change (they are not the hashes of the aggregates, which may): the value of an integer is sift_fmix64 of it, of a string the words of its bytes
 * folded by sift_fmix64 (the length first, the last word padded with zeros), of a key of several columns the hashes of its columns folded in the order of SiftPointCol.leaves.
 */
#ifndef SIFT_F2_POINT_DIR_H
#define SIFT_F2_POINT_DIR_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/part/format.h"
#include "col/granule/granule.h"
#include "core/hash.h"

#define SIFT_POINT_MAGIC		0x44544E50	/* "PNTD" */
#define SIFT_POINT_VERSION		2
#define SIFT_POINT_MAX_COLS		8
#define SIFT_POINT_MAX_COMP		4		/* columns of one key */
#define SIFT_POINT_NO_LEAF		UINT16_MAX
#define SIFT_POINT_MIN_ROWS		16384
#define SIFT_POINT_SAMPLE		4096
#define SIFT_POINT_LOAD			8		/* entries of a bucket, on average */
#define SIFT_POINT_MAX_VALUES	64
#define SIFT_POINT_ROW_BITS		13
#define SIFT_POINT_MAX_PCT		25		/* the section is at most this share of the bytes of the granules of its part: a narrow part is not made twice as large for lookups it serves quickly anyway */

StaticAssertDecl(SIFT_GRANULE_ROWS <= (1 << SIFT_POINT_ROW_BITS), "a row of a granule is 13 bits");

typedef enum SiftPointKeyKind
{
	SIFT_PK_INT64 = 1,			/* an integer column: int2, int4, int8, date, timestamp, a numeric of the scaled integer class */
	SIFT_PK_UUID16 = 2,			/* a uuid column: its 16 bytes */
	SIFT_PK_TEXT = 3,			/* a text column: its bytes, equal where the strings are equal (a deterministic collation) */
	SIFT_PK_COMPOSITE = 4		/* two to four columns */
} SiftPointKeyKind;

#define SIFT_PCOL_UNIQUE		(UINT16_C(1) << 0)	/* no two rows of the part hold the same key */

static inline uint64
sift_point_hash_i64(int64 v)
{
	return sift_fmix64((uint64) v);
}

static inline uint64
sift_point_hash_bytes(const void *p, uint32 len)
{
	const uint8 *b = p;
	uint64		h = sift_fmix64(len);

	for (; len >= 8; len -= 8, b += 8)
	{
		uint64		w;

		memcpy(&w, b, 8);
		h = sift_fmix64(h ^ w);
	}
	if (len > 0)
	{
		uint64		w = 0;

		memcpy(&w, b, len);
		h = sift_fmix64(h ^ w);
	}
	return h;
}

/* The hash of a key of several columns: the hash of the first, then each next folded in */
static inline uint64
sift_point_hash_combine(uint64 h, uint64 x)
{
	return sift_fmix64(h ^ (x + SIFT_FIB64 + (h << 6) + (h >> 2)));
}

/* The bucket of a hash in a table of 1 << log2_buckets */
static inline uint32
sift_point_bucket(uint64 hash, uint32 log2_buckets)
{
	return (uint32) (hash >> (64 - log2_buckets));
}

typedef struct SiftPointHdr
{
	uint32		magic;
	uint16		version;
	uint16		ncols;			/* keys */
} SiftPointHdr;

typedef struct SiftPointCol
{
	uint16		leaves[SIFT_POINT_MAX_COMP];	/* the columns of the key (the others SIFT_POINT_NO_LEAF) */
	uint8		key_kind;		/* SiftPointKeyKind */
	uint8		nleaves;		/* 1..SIFT_POINT_MAX_COMP */
	uint16		flags;			/* SIFT_PCOL_* */
	uint32		log2_buckets;
	uint32		nentries;
	uint32		off_buckets;	/* in the section: nbuckets + 1 uint32 */
	uint32		off_entries;	/* in the section: nentries uint32 */
	uint32		pad;
} SiftPointCol;

StaticAssertDecl(sizeof(SiftPointHdr) == 8, "the point directory header is 8 bytes");
StaticAssertDecl(sizeof(SiftPointCol) == 32, "a key of the point directory is 32 bytes");
StaticAssertDecl(offsetof(SiftPointCol, key_kind) == 8 && offsetof(SiftPointCol, log2_buckets) == 12 && offsetof(SiftPointCol, off_entries) == 24, "the layout of a key of the point directory");

/* The section of the rows of a part of part_bytes of granules, or false when none of its keys qualifies and fits in SIFT_POINT_MAX_PCT of them */
extern bool sift_point_dir_build(const SiftRows *rows, const int *gstart, int ng, uint64 part_bytes, StringInfo out);

/* The header of the section of an opened part (copied into cx), or NULL when it has none, or one of another version */
typedef struct SiftPointDir
{
	SiftSecDirEntry sec;
	SiftPointHdr hdr;
	SiftPointCol cols[SIFT_POINT_MAX_COLS];
} SiftPointDir;

extern SiftPointDir *sift_point_dir_open(Relation rel, const SiftPart *part, MemoryContext cx);

/* The rows a scan for the values of one predicate has to look at: refs ascending and unique, empty when no row can hold a value */
typedef struct SiftPointCands
{
	uint32		n;
	uint32	   *ref;
	const SiftColPred *pred;	/* the predicate they are for: of a key of several columns, that of the first, and the others are tested on the rows it leaves */
	int			pred_idx;		/* and its place among the predicates of the scan */
	bool		str;			/* its column holds strings, not integers */
} SiftPointCands;

/* False when no key of the directory is one the predicates of the scan name by an equality or a short list of values on each of its columns; else the rows of the part that may satisfy it, from the pages of the section it needs */
extern bool sift_point_dir_cands(Relation rel, const SiftPart *part, const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds, MemoryContext cx, SiftPointCands *pc);

/* The rows of granule g among the candidates, ascending, into rows (cap of them); -1 when there are more than cap */
extern int	sift_point_cands_rows(const SiftPointCands *pc, uint32 g, uint16 *rows, int cap);

/* Of the rows of a granule the directory named, those whose value the predicate accepts, read from the chunk; rows (n of them) is compacted, and the count is returned */
extern int	sift_point_cands_verify(const SiftPointCands *pc, const SiftGranuleView *v, uint16 *rows, int n);

/* The first granule at or after g that has a candidate, or UINT32_MAX */
extern uint32 sift_point_cands_next(const SiftPointCands *pc, uint32 g);

#endif							/* SIFT_F2_POINT_DIR_H */
