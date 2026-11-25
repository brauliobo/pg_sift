/*
 * agg/v6/join/asof.h - the ASOF join of SiftAggV6: PostgreSQL's idiom
 *
 *   FROM fact l LEFT | INNER JOIN LATERAL (SELECT .. FROM dim r WHERE r.k = l.k AND r.ts <= l.ts ORDER BY r.ts DESC LIMIT 1) x ON true
 *
 * and its mirror (r.ts >= l.ts ORDER BY r.ts ASC LIMIT 1), with < and > as the strict forms, executed as one merge of the two sides instead of one index probe for each fact row. dim is a table whose parts
 * are sorted by (k, ts) (the key_order of a sift_table of format major 2, or a SIFT_PROJ_SORT projection), so its batches come in that order and the fact rows are taken in that order too: the merge reads
 * each side once, O(N + M) with no sort and a state of one dim row. Fact rows that are not in order (the fact table has another key_order) go to sift_asof_match_arrays, one binary search of an in-memory
 * dim for each.
 *
 * The answer of PostgreSQL, row for row: a NULL k or ts on either side never matches (a fact row with one is a row of no match: its LEFT row has NULLs, the INNER join has none); a dim row with a NULL k
 * or ts is never read. Among the dim rows of the same k whose ts ties, the one a backward index scan of (k, ts) returns first is the last in the order of the parts for <=, <, and the first for >=, >.
 * Types: k and ts are int8, int4, date, timestamp and timestamptz, which are one int64 whose order is the type's; a float8 ts is sift_asof_i64_of_f64 (NaN above everything, -0 equal to 0).
 */
#ifndef SIFT_AGG_V6_ASOF_H
#define SIFT_AGG_V6_ASOF_H

#include "core/host.h"

typedef enum SiftAsofDir
{
	SIFT_ASOF_BACKWARD,			/* r.ts <= l.ts ORDER BY r.ts DESC LIMIT 1 */
	SIFT_ASOF_FORWARD			/* r.ts >= l.ts ORDER BY r.ts ASC LIMIT 1 */
} SiftAsofDir;

typedef struct SiftAsofSpec
{
	SiftAsofDir dir;
	bool		strict;			/* r.ts < l.ts, or r.ts > l.ts */
} SiftAsofSpec;

/* A batch of dim rows in the order of its parts; valid bitmaps NULL: every row is */
typedef struct SiftAsofBatch
{
	uint32		n;
	const int64 *key;
	const int64 *ts;
	const uint64 *key_valid;
	const uint64 *ts_valid;
	const uint64 *tag;			/* what the caller fetches the dim row by (its row id) */
} SiftAsofBatch;

/* The next batch (valid until the following call); false at the end of dim */
typedef bool (*SiftAsofNext) (void *ctx, SiftAsofBatch *out);

typedef struct SiftAsofHit
{
	bool		found;
	uint64		tag;
	int64		ts;
} SiftAsofHit;

typedef struct SiftAsofStats
{
	uint64		fact_rows;
	uint64		dim_rows;		/* read from dim: at most all of them, each once */
	uint64		dim_null_rows;
	uint64		matched;
} SiftAsofStats;

typedef struct SiftAsofStream SiftAsofStream;

extern SiftAsofStream *sift_asof_open(MemoryContext cx, SiftAsofSpec spec, SiftAsofNext next, void *ctx);
/* The match of a fact row; the rows with a non-NULL k and ts must come in nondecreasing (k, ts) order (a violation is an error) */
extern SiftAsofHit sift_asof_probe(SiftAsofStream *s, int64 key, bool key_null, int64 ts, bool ts_null);
extern const SiftAsofStats *sift_asof_stats(const SiftAsofStream *s);
extern void sift_asof_close(SiftAsofStream *s);

/* The same for fact rows in any order against dim in memory (n rows sorted by (key, ts), non-NULL): the match of each of nfact rows into hits[] */
typedef struct SiftAsofArrays
{
	uint32		n;
	const int64 *key;
	const int64 *ts;
	const uint64 *tag;
} SiftAsofArrays;

extern void sift_asof_match_arrays(SiftAsofSpec spec, const SiftAsofArrays *dim, uint32 nfact, const int64 *key, const int64 *ts, const uint64 *key_valid, const uint64 *ts_valid, SiftAsofHit *hits);

/* A float8 ts or k as the int64 whose order is the float8 order of PostgreSQL */
extern int64 sift_asof_i64_of_f64(double v);

#endif							/* SIFT_AGG_V6_ASOF_H */
