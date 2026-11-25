/*
 * col/granule/cfilt/kern.h - the test of a predicate on a column of integers as it is stored, with no column materialized: the rows that pass come out as a bit for each value of the chunk
 * (the dense bits of the non-null values; sift_cf_scatter lays them over the rows). What the stored form gives for free is used before any value is looked at.
 *
 *   frame of reference    base + unsigned offsets of `bits` bits (SIFT_CF_FOR; the widths 8, 16, 32 and 64 are arrays and run as vector compares, the others unpack 64 offsets at a time):
 *                         the range is moved to offset space once, so a value costs one subtraction and one unsigned compare; a range that holds no offset or all of them ends it at once
 *   delta                 the first value and the differences, zigzag coded or, when the column is non-decreasing, plain; a non-decreasing column stops at the first value above the range
 *   run length            a test of each run, never of each row, and the runs that pass are filled as words
 *   dictionary            the test of each entry, then the codes looked up in the set of those that pass (or, when those are a contiguous run of codes, compared as a range of the codes)
 *   boolean               the bits themselves
 *
 *   the codec's own layout of the frame of reference and the delta (the vertical 32 x 32 bit-packing, `sift_cf_for_vert`, `sift_cf_delta_vert`): the rows of 32 values are unpacked a block of 1024 at a time
 *                         into 32 bit offsets that the vector compares read; a delta chunk keeps the value each block starts from, so a block the range misses or holds whole is not unpacked
 *
 * A chunk the reader cannot open (a run list that does not add up to the rows, a width above 64) is SIFT_CF_UNSUPPORTED and the decoder takes it. The output words past the last value are 0.
 */
#ifndef SIFT_CFILT_KERN_H
#define SIFT_CFILT_KERN_H

#include "core/host.h"

typedef enum SiftCfResult
{
	SIFT_CF_FALSE,				/* no value passes: out is not written */
	SIFT_CF_TRUE,				/* every value passes: out is not written */
	SIFT_CF_BITS,				/* out holds them */
	SIFT_CF_UNSUPPORTED
} SiftCfResult;

typedef enum SiftCfKind
{
	SIFT_CFT_NONE,				/* no value passes */
	SIFT_CFT_ALL,				/* every non-null value (IS NOT NULL, the part of a NULL test that is on the values) */
	SIFT_CFT_RANGE,				/* lo <= v <= hi */
	SIFT_CFT_NRANGE,			/* not in [lo, hi] */
	SIFT_CFT_SET,				/* v is one of the nset values of set, ascending */
	SIFT_CFT_NSET,
	SIFT_CFT_MEMBER				/* v is a code of 0 .. nmember - 1 whose bit is set in member (the codes of a dictionary that a predicate chose) */
} SiftCfKind;

typedef struct SiftCfTest
{
	SiftCfKind	kind;
	int64		lo;
	int64		hi;
	const int64 *set;
	uint32		nset;
	const uint64 *member;
	uint32		nmember;
} SiftCfTest;

extern bool sift_cf_test_value(const SiftCfTest *t, int64 v);

/* The least and the greatest value of a chunk, when the writer kept them: the test is answered from them where they allow */
typedef struct SiftCfBounds
{
	bool		known;
	int64		vmin;
	int64		vmax;
} SiftCfBounds;

typedef struct SiftCfFor
{
	int64		base;
	uint8		bits;			/* 0 .. 64: 0 is a column of one value */
	uint32		n;
	const uint8 *packed;		/* the offsets, little endian, bits each, ceil(n * bits / 8) bytes */
	SiftCfBounds range;
} SiftCfFor;

typedef struct SiftCfDelta
{
	int64		first;
	uint8		bits;			/* of each of the n - 1 differences */
	bool		monotone;		/* the differences are not negative and are stored as they are; else zigzag coded */
	uint32		n;
	const uint8 *packed;
	SiftCfBounds range;
} SiftCfDelta;

typedef struct SiftCfRle
{
	uint32		nruns;
	uint32		n;				/* the values: the lengths add up to it */
	const int64 *vals;
	const uint32 *lens;
} SiftCfRle;

typedef struct SiftCfDict
{
	uint32		dict_n;
	uint8		bits;			/* of a code: 0 .. 32 */
	uint32		n;
	const uint8 *packed;		/* the codes */
	const uint64 *match;		/* a bit for each code of 0 .. dict_n - 1 that passes: ceil(dict_n / 64) words */
} SiftCfDict;

/* The frame of reference of the codec: base + offsets of `bits` bits (1 .. 32) in the vertical layout, rows of 32 values, 32 rows (1024 values) a block of 32 * bits words */
typedef struct SiftCfForVert
{
	int64		base;
	uint8		bits;
	uint32		n;
	const uint8 *words;			/* ceil(n / 32) rows as the codec lays them: a full block of 32 rows takes 32 * bits words, the last block 32 * ceil(rows * bits / 32) */
	SiftCfBounds range;
} SiftCfForVert;

/* The delta of the codec: the value each block of 1024 starts from (first, then check[b - 1] for block b, 8 bytes each), the differences minus `step` in the vertical layout */
typedef struct SiftCfDeltaVert
{
	int64		first;
	int64		step;			/* the least difference, added to each */
	uint8		bits;			/* 0 .. 32 */
	uint32		n;
	const uint8 *check;			/* (n - 1) / 1024 values */
	const uint8 *words;
	SiftCfBounds range;
} SiftCfDeltaVert;

/* The codes of a dictionary in the vertical layout of the codec (bits 1 .. 32) */
typedef struct SiftCfDictVert
{
	uint32		dict_n;
	uint8		bits;
	uint32		n;
	const uint8 *words;
	const uint64 *match;
} SiftCfDictVert;

extern SiftCfResult sift_cf_for(const SiftCfFor *c, const SiftCfTest *t, uint64 *out);
extern SiftCfResult sift_cf_for_vert(const SiftCfForVert *c, const SiftCfTest *t, uint64 *out);
extern SiftCfResult sift_cf_delta_vert(const SiftCfDeltaVert *c, const SiftCfTest *t, uint64 *out);
extern SiftCfResult sift_cf_dict_vert(const SiftCfDictVert *c, uint64 *out);
extern SiftCfResult sift_cf_delta(const SiftCfDelta *c, const SiftCfTest *t, uint64 *out);
extern SiftCfResult sift_cf_rle(const SiftCfRle *c, const SiftCfTest *t, uint64 *out);
extern SiftCfResult sift_cf_dict(const SiftCfDict *c, uint64 *out);

/* The entries of a dictionary of integers that pass the test, as the bits of match (ceil(n / 64) words); how many pass */
extern uint32 sift_cf_dict_match(const int64 *dict, uint32 n, const SiftCfTest *t, uint64 *match);

/* The same for a dictionary whose entries ascend (that of the codec): the entries a range or a set takes are found by binary search, not tested one by one */
extern uint32 sift_cf_dict_match_sorted(const int64 *dict, uint32 n, const SiftCfTest *t, uint64 *match);

/* The n bits of a boolean chunk tested by what each value does (pass0, pass1) */
extern SiftCfResult sift_cf_bool(const uint8 *bits, uint32 n, bool pass0, bool pass1, uint64 *out);

/* The dense bits of the non-null values laid over nrows rows: row r takes the bit of the value it holds, 0 when it has none; validity is a bit for each row; out has ceil(nrows / 64) words */
extern void sift_cf_scatter(const uint64 *dense, const uint8 *validity, uint32 nrows, uint64 *out);

#endif							/* SIFT_CFILT_KERN_H */
