/*
 * col/skip_index.h - the skipping indexes of a part of format major 2 (SEC_SKIP_INDEX): for a column and a granule, a summary of the values that tells a predicate the granule holds no
 * row that passes it, so that none of its chunks is read. The summaries are written by the build of a part of the cold tier and by a compaction, whatever the columns hold: no option asks
 * for them. A column that a zone does not rule out gets, for each granule, the smallest summary that is exact:
 *
 *   INT_BITSET64   integers that span less than 64 values: the base and a bit for each value of the span (16 bytes)
 *   EXACT_SET      at most SIFT_SKIP_EXACT_MAX distinct integers, or the 64 bit hashes of that many strings (8 bytes each)
 *   DICT_BITSET    strings coded by the dictionary of the part (at most SIFT_SKIP_DICT_MAX of them): a bit for each code that occurs in the granule
 *
 * and, within a bound on the bytes of the whole index (sift_aux_skip_budget, col/f2/aux_budget.h: a two-hundredth of the data that compresses, no index for a part of fewer than SIFT_AUX_SKIP_MIN_ROWS rows; no filter for a granule of fewer than SIFT_SKIP_FILTER_MIN_ROWS rows, which a scan reads whole
 * as fast, nor for one whose filter has SIFT_SKIP_SATURATED_PCT of its bits set, which rules nothing out), for the columns that have more distinct values than that:
 *
 *   BLOOM_SB       a split-block Bloom filter (core/sbbf.h) of the integers, strings and uuids, for equality and IN
 *   TRIGRAM_SB     the same of the three byte windows of the strings with their ASCII letters folded to lower case, for LIKE and ILIKE
 *
 * The section is laid out by column, so that a scan reads the entries of the columns it filters: the header, a descriptor for each (column, kind), then for each descriptor the offsets of the
 * payloads of its granules, a flag for each granule and the payloads. A granule an entry holds nothing for (another kind was exact for it, or it was too large to keep) says so in its flag.
 * The payloads of a granule do not depend on its place in the part, so that a compaction that copies the granule copies them (sift_skip_join).
 */
#ifndef SIFT_SKIP_INDEX_H
#define SIFT_SKIP_INDEX_H

#include "col/f2/aux_budget.h"
#include "core/host.h"

#include "col/am.h"
#include "col/granule/granule.h"
#include "col/f2/part/format.h"

#define SIFT_SKIP_MAGIC			0x534B5032U		/* "SKP2" */
#define SIFT_SKIP_VERSION		1

typedef enum SiftSkipKind
{
	SIFT_SKIP_DICT_BITSET = 1,
	SIFT_SKIP_INT_BITSET64 = 2,
	SIFT_SKIP_EXACT_SET = 3,
	SIFT_SKIP_BLOOM_SB = 4,
	SIFT_SKIP_TRIGRAM_SB = 5
} SiftSkipKind;

#define SIFT_SKIP_NKINDS		5
#define SIFT_SKIP_EXACT_MAX		16
#define SIFT_SKIP_DICT_MAX		512
#define SIFT_SKIP_SPAN			64
#define SIFT_SKIP_FILTER_MIN_ROWS	2048
#define SIFT_SKIP_SATURATED_PCT	85
#define SIFT_TRIGRAM_MAX_BLOCKS	128
#define SIFT_TRIGRAM_SATURATE	3276

#define SIFT_SKIP_GRAN_PRESENT	0
#define SIFT_SKIP_GRAN_EMPTY	1	/* every row of the granule is null in the column: no value can match */
#define SIFT_SKIP_GRAN_NONE		2	/* nothing is kept for the granule in this entry */

#define SIFT_SKIP_EXACT_HASHES	1	/* param of EXACT_SET: 64 bit hashes of strings, not the integers themselves */

typedef struct SiftSkipIndexHeader
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		nentries;
	uint32		ngranules;
	uint32		reserved[4];
} SiftSkipIndexHeader;

StaticAssertDecl(sizeof(SiftSkipIndexHeader) == 32, "SiftSkipIndexHeader must be 32 bytes");

typedef struct SiftSkipColDesc
{
	uint16		col;
	uint8		kind;
	uint8		flags;
	uint16		param;			/* DICT_BITSET: words of 64 bits; EXACT_SET: SIFT_SKIP_EXACT_HASHES or 0; the filters: bits for each key */
	uint16		pad;
	uint32		meta_off;		/* from the start of the section: ngranules + 1 payload offsets, then a flag for each granule */
	uint32		data_off;
} SiftSkipColDesc;

StaticAssertDecl(sizeof(SiftSkipColDesc) == 16, "SiftSkipColDesc must be 16 bytes");

typedef struct SiftSkipIntBitset64
{
	int64		base;
	uint64		bits;
} SiftSkipIntBitset64;

/* Writes the section of the rows of a part into out; false when no column has a summary (nothing is written). granule_bytes: the bytes of the granules, which bound the filters */
extern bool sift_skip_build(const SiftRows *rows, const int *gstart, int ng, const SiftDictEnc *const *dicts, const SiftColPromo *promos, const SiftAuxBudget *aux, StringInfo out);

/* The section of the parts joined as they are (their granules in the order given): the payloads copied, an entry that a part lacks flagged for its granules; false when no part has the section */
extern bool sift_skip_join(Relation rel, SiftPart *const *src, int n, StringInfo out);

typedef enum SiftSkipVerdict
{
	SIFT_SKIP_PRUNE,			/* no row of the granule passes the predicate */
	SIFT_SKIP_MAY_MATCH,
	SIFT_SKIP_ALL_MATCH			/* every row of the granule passes it (it holds no null): the predicate is not tested */
} SiftSkipVerdict;

/* The skipping indexes of an opened part, read as a scan needs them */
typedef struct SiftSkipReader SiftSkipReader;

/* NULL when the part has no skipping index */
extern SiftSkipReader *sift_skip_open(Relation rel, const SiftPart *part);

/* A predicate of a leaf made executable against the entries of the part: NULL when no entry of the leaf can tell it anything. dict: the dictionary of the leaf of the part, or NULL */
typedef struct SiftSkipQuery SiftSkipQuery;
extern SiftSkipQuery *sift_skip_query(const SiftSkipReader *r, const SiftColDef *def, const SiftColPred *p, const SiftPartDict *dict);

/* What the entries say of granule g. no_nulls: the zone of the granule says it holds no null */
extern SiftSkipVerdict sift_skip_probe(Relation rel, const SiftPart *part, SiftSkipReader *r, const SiftSkipQuery *q, uint32 g, bool no_nulls);

#endif							/* SIFT_SKIP_INDEX_H */
