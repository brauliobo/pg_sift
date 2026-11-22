/*
 * col/granule_filter.h - the predicates of a scan run on the chunks of a granule as they are stored: the rows that pass come out as a bit for each row of the granule, and no value is decoded, no
 * string built. The codes of a dictionary, the runs of a run length chunk, the packed offsets of a frame of reference or a delta, the digits of a decimal float, the bits of a boolean and the coded
 * bytes of a string are tested in place. Rows with no value come out 0 (a value test is not true of a null), and the bits past the last row too.
 *
 * Only a predicate that the scan decides exactly on the stored values is compiled (a leaf of one lane that holds what its type does), and only a chunk without exceptions is filtered; every
 * other pair stays with the decoder, which gives the same answer for the rows it is asked about.
 */
#ifndef SIFT_GRANULE_FILTER_H
#define SIFT_GRANULE_FILTER_H

#include "col/codec.h"
#include "col/col.h"
#include "col/granule/granule_int.h"

typedef enum SiftCfiltResult
{
	SIFT_CFILT_ALL_FALSE,		/* no row passes: out_mask is not written */
	SIFT_CFILT_ALL_TRUE,		/* every row of the granule passes: out_mask is not written */
	SIFT_CFILT_BITS,			/* out_mask holds the rows */
	SIFT_CFILT_UNSUPPORTED		/* the chunk or the predicate is one the decoder must take */
} SiftCfiltResult;

/* The rows of the dictionary of a part (or of a granule) that satisfy a predicate: a bit for each code, and the bit past the last (a clamp for a code that is not one) is 0 */
typedef struct SiftDictPredCache
{
	uint32		dict_n;
	uint32		n_matches;		/* 0 with n_maybe 0: no row of the part can pass */
	uint32		n_maybe;		/* an extended predicate (SIFT_COLOP_IS_EXT): the codes the stored bytes cannot decide, bits of maybe_bits; the matches of bits are exact */
	uint32		min_match;
	uint32		max_match;
	bool		is_contiguous;	/* the matches are min_match .. max_match, as they are for a comparison and a prefix of the sorted dictionary */
	bool		is_all;
	uint32		nwords64;
	uint64	   *bits;
	uint64	   *maybe_bits;		/* NULL unless the predicate is an extended one */
} SiftDictPredCache;

/* The cache of a string predicate over a dictionary of n strings in byte order (off: n + 1 offsets into bytes) */
extern SiftDictPredCache *sift_dict_pred_prepare(const SiftColPred *p, uint32 n, const uint32 *off, const char *bytes);

/* A needle of a string predicate as the bytes the table of symbols of a part codes it to (a prefix: the codes of the part of it that its continuations cannot change, and the bytes of it they stand for) */
typedef struct SiftFsstNeedle
{
	uint8	   *enc;
	uint32		elen;
	uint32		raw;
} SiftFsstNeedle;

/* The needles of a predicate (an equality, an IN list or a prefix) over the table of symbols of a part, made once for the part: the chunks of a part share the table */
typedef struct SiftFsstPredCache
{
	const struct SiftFsst *fsst;
	int			n;
	SiftFsstNeedle *needles;
} SiftFsstPredCache;

extern SiftFsstPredCache *sift_fsst_pred_prepare(const struct SiftFsst *fsst, const SiftColPred *p);

typedef struct SiftCfiltPred
{
	const SiftColDef *def;
	const SiftColPred *pred;
	bool		nulltest;		/* IS NULL, IS NOT NULL */
	bool		numeric;		/* ct: the predicate as a test of the keys of the leaf */
	SiftCTest	ct;
	bool		str;
	const SiftDictPredCache *part;	/* of the dictionary of the part of the leaf, when the scan has made it */
	const SiftFsstPredCache *fsst_part;	/* of the table of symbols of the part of the leaf, when the scan has made it */
} SiftCfiltPred;

/* Whether the predicate can run on a chunk as it is stored; false: the scan decodes the leaf for it */
extern bool sift_cfilt_compile(SiftCfiltPred *cp, const SiftColDef *def, const SiftColPred *p);

/* The rows of the granule that pass; out_mask: SIFT_GRANULE_ROWS / 64 words */
#define SIFT_CFILT_WORDS		(SIFT_GRANULE_ROWS / 64)
extern SiftCfiltResult sift_granule_filter_compressed(const SiftGranuleView *v, const SiftCfiltPred *cp, uint64 *out_mask);

#endif							/* SIFT_GRANULE_FILTER_H */
