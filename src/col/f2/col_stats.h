/*
 * col/f2/col_stats.h - SEC_COL_STATS (0x020E): what a part knows of each of its columns before the first granule is read, so that a scan can rank its predicates from granule 0 instead of
 * after the granules that teach the adaptive filter (agg/filter_adapt.h): the bytes the column takes encoded, the distinct values it holds, the encoding most of it is in, the rows that are NULL.
 *
 * The section is a 16-byte header, ncols records of 16 bytes and, when SIFT_COLSTATS_HAS_NULLS is set, ncols counts of NULL rows (uint64). A column's distinct values are counted by a sketch
 * of 1,024 registers whose error is about 3% and which merges, so that the writer of a part joined from others adds theirs.
 */
#ifndef SIFT_COL_STATS_H
#define SIFT_COL_STATS_H

#include "core/host.h"

#define SIFT_COL_STATS_MAGIC	0x41545343U		/* "CSTA" */
#define SIFT_COL_STATS_VERSION	1
#define SIFT_COLSTATS_HAS_NULLS	1U		/* the NULL counts follow the records */

typedef struct SiftColStatsHdr
{
	uint32		magic;
	uint16		version;
	uint16		ncols;
	uint32		ngranules;
	uint32		flags;
} SiftColStatsHdr;

StaticAssertDecl(sizeof(SiftColStatsHdr) == 16, "SiftColStatsHdr must be 16 bytes");

#define SIFT_COLSTAT_ALL_NULL	(UINT16_C(1) << 0)	/* no row has a value */
#define SIFT_COLSTAT_NDV_EXACT	(UINT16_C(1) << 1)	/* est_ndv is a count, not an estimate (the column is held as a dictionary of that many entries) */

typedef struct SiftPartColStat
{
	uint64		total_bytes;	/* the bytes of the chunks of the column over the granules */
	uint32		est_ndv;
	uint16		dominant_enc;	/* the encoding that took most of total_bytes */
	uint16		flags;			/* SIFT_COLSTAT_* */
} SiftPartColStat;

StaticAssertDecl(sizeof(SiftPartColStat) == 16, "SiftPartColStat must be 16 bytes");

/* A section that passed sift_col_stats_open: pointers into its bytes */
typedef struct SiftColStats
{
	const SiftColStatsHdr *hdr;
	const uint8 *recs;
	const uint8 *nulls;			/* NULL: the counts are not there */
} SiftColStats;

extern uint64 sift_col_stats_size(uint16 ncols, bool with_nulls);
extern const char *sift_col_stats_open(const void *blob, uint64 len, uint16 ncols, uint32 ngranules, SiftColStats *cs);

/* The record and the NULL count of column c (0 when the section has none) */
extern SiftPartColStat sift_col_stats_get(const SiftColStats *cs, int c);
extern uint64 sift_col_stats_nulls(const SiftColStats *cs, int c);

/*
 * The statistics of parts joined as they are: the bytes and the NULLs added, the distinct count of the part that has the most (the sketches are not kept, so it is a floor, not an estimate), the
 * encoding of the part that has the most bytes; NULL when the sections are not of one schema.
 */
extern void *sift_col_stats_merge(const void *const *blobs, const uint64 *lens, int n, uint64 *len);

/* ---- the distinct count: a sketch of 1,024 one byte registers ---- */

#define SIFT_NDV_REGISTERS		1024

typedef struct SiftNdvSketch
{
	uint8		reg[SIFT_NDV_REGISTERS];
} SiftNdvSketch;

extern void sift_ndv_add_hash(SiftNdvSketch *s, uint64 h);
extern void sift_ndv_add_i64(SiftNdvSketch *s, int64 v);
extern void sift_ndv_add_bytes(SiftNdvSketch *s, const void *p, uint32 len);
extern void sift_ndv_merge(SiftNdvSketch *into, const SiftNdvSketch *from);
extern uint64 sift_ndv_estimate(const SiftNdvSketch *s);

/* ---- the writer ---- */

typedef struct SiftColStatsBuilder SiftColStatsBuilder;

extern SiftColStatsBuilder *sift_col_stats_begin(uint16 ncols);

/* The chunk of column c of one granule: its rows, the NULL ones, the bytes it takes, the encoding it is in */
extern void sift_col_stats_granule(SiftColStatsBuilder *b, int c, uint32 nrows, uint32 nnull, uint32 chunk_bytes, uint16 enc);

/* The values of column c: each distinct one counts once; a column held as a dictionary reports its entries (exact: no value is repeated) */
extern SiftNdvSketch *sift_col_stats_ndv(SiftColStatsBuilder *b, int c);
extern void sift_col_stats_set_ndv_exact(SiftColStatsBuilder *b, int c, uint32 ndv);

extern void *sift_col_stats_finish(SiftColStatsBuilder *b, uint32 ngranules, uint64 *len);

/* ---- what a scan expects of a predicate from the statistics ---- */

typedef enum SiftPredShape
{
	SIFT_PS_EQ,					/* nvals equalities of one value each */
	SIFT_PS_RANGE,				/* a range on an ordered column: a third of the values unless the zones say otherwise */
	SIFT_PS_ISNULL,
	SIFT_PS_NOTNULL,
	SIFT_PS_SUBSTRING,
	SIFT_PS_OTHER
} SiftPredShape;

/* The share of its rows a predicate of the shape expected to pass, clamped to [0.001, 0.99], and the encoded bytes a row of the column takes (what reading it costs) */
extern double sift_col_stats_pass_prior(const SiftPartColStat *st, uint64 nrows, uint64 nnull, SiftPredShape shape, int nvals);
extern double sift_col_stats_bytes_per_row(const SiftPartColStat *st, uint64 nrows);

#endif							/* SIFT_COL_STATS_H */
