/*
 * col/f2/f2_point_dir.h - SEC_POINT (0x0209), an optional section of a part of level 1 and up of an unkeyed index: for each of at most three integer columns whose values are nearly all
 * distinct, a hash table from a value to the rows that hold it (granule << 13 | row in it), so that a scan for one value or a few reads three pages of the section and the granules
 * of the rows it names, and not every granule of the part. A reader that does not know the section ignores it, and a part that holds it is not joined as it is (col/granule/part_join.c).
 *
 *   SiftPointHdr, then one SiftPointCol for each column, then for each column the bucket table (nbuckets + 1 entry numbers) and its entries (the rows, ascending in a bucket)
 */
#ifndef SIFT_F2_POINT_DIR_H
#define SIFT_F2_POINT_DIR_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/part_format.h"
#include "col/granule/granule.h"

#define SIFT_POINT_MAGIC		0x44544E50	/* "PNTD" */
#define SIFT_POINT_VERSION		1
#define SIFT_POINT_MAX_COLS		3
#define SIFT_POINT_MIN_ROWS		16384
#define SIFT_POINT_SAMPLE		4096
#define SIFT_POINT_LOAD			8		/* entries of a bucket, on average */
#define SIFT_POINT_MAX_VALUES	64
#define SIFT_POINT_ROW_BITS		13
#define SIFT_POINT_MAX_PCT		25		/* the section is at most this share of the bytes of the granules of its part: a narrow part is not made twice as large for lookups it serves quickly anyway */

StaticAssertDecl(SIFT_GRANULE_ROWS <= (1 << SIFT_POINT_ROW_BITS), "a row of a granule is 13 bits");

/* The bucket of a value in a table of 1 << log2_buckets */
static inline uint32
sift_point_bucket(int64 v, uint32 log2_buckets)
{
	return (uint32) (((uint64) v * UINT64CONST(0x9E3779B97F4A7C15)) >> (64 - log2_buckets));
}

typedef struct SiftPointHdr
{
	uint32		magic;
	uint16		version;
	uint16		ncols;
} SiftPointHdr;

typedef struct SiftPointCol
{
	uint32		leaf;
	uint32		log2_buckets;
	uint32		nentries;
	uint32		off_buckets;	/* in the section: nbuckets + 1 uint32 */
	uint32		off_entries;	/* in the section: nentries uint32 */
	uint32		pad;
} SiftPointCol;

StaticAssertDecl(sizeof(SiftPointHdr) == 8 && sizeof(SiftPointCol) == 24, "the point directory header is 8 bytes and a column 24");

/* The section of the rows of a part of part_bytes of granules, or false when none of its columns qualifies and fits in SIFT_POINT_MAX_PCT of them */
extern bool sift_point_dir_build(const SiftRows *rows, const int *gstart, int ng, uint64 part_bytes, StringInfo out);

/* The header of the section of an opened part (copied into cx), or NULL when it has none */
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
	const SiftColPred *pred;	/* the predicate they are for */
	int			pred_idx;		/* and its place among the predicates of the scan */
} SiftPointCands;

/* False when no predicate of the scan is an equality or a short list of values on a column the directory covers; else the rows of the part that may satisfy it, from the pages of the section it needs */
extern bool sift_point_dir_cands(Relation rel, const SiftPart *part, const SiftPointDir *pd, const SiftColSchema *schema, const SiftColPred *preds, int npreds, MemoryContext cx, SiftPointCands *pc);

/* The rows of granule g among the candidates, ascending, into rows (cap of them); -1 when there are more than cap */
extern int	sift_point_cands_rows(const SiftPointCands *pc, uint32 g, uint16 *rows, int cap);

/* Of the rows of a granule the directory named, those whose value the predicate accepts, read from the chunk; rows (n of them) is compacted, and the count is returned */
extern int	sift_point_cands_verify(const SiftPointCands *pc, const SiftGranuleView *v, uint16 *rows, int n);

/* The first granule at or after g that has a candidate, or UINT32_MAX */
extern uint32 sift_point_cands_next(const SiftPointCands *pc, uint32 g);

#endif							/* SIFT_F2_POINT_DIR_H */
