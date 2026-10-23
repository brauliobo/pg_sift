/*
 * agg/stats.h - sift.stats(): the counters of this backend (docs/USAGE.md "Session statistics").
 *
 * Plain memory of the process, no shared memory and no shared_preload_libraries: a scan folds its own counters (the ones EXPLAIN ANALYZE shows)
 * into these once, when it ends, so no loop of a scan pays for them.
 */
#ifndef SIFT_AGG_STATS_H
#define SIFT_AGG_STATS_H

#include "postgres.h"

/* X(ID, name, sentence): the rows of sift.stats() in this order, and the table of docs/USAGE.md */
#define SIFT_STATS(X) \
	X(SCANS, "scans", "Custom Scan (SiftAgg) scans that ended (the arms of one fused pass are one)") \
	X(WORKERS, "workers_launched", "parallel workers those scans launched") \
	X(UNITS, "units", "units the scans read or answered (not the pruned ones)") \
	X(UNITS_PRUNED_ZONE, "units_pruned_zone", "units skipped before they were read: their zone maps hold no match") \
	X(UNITS_PRUNED_SET, "units_pruned_set", "units skipped before they were read: only their summary of key_columns holds no match") \
	X(UNITS_PRUNED_SEARCH, "units_pruned_search", "units skipped before they were read: none of their rows is in the search selection") \
	X(UNITS_PREAGG, "units_preagg", "of the units, those answered from their pre-aggregates") \
	X(ROWS_DECODED, "rows_decoded", "rows of the units that were scanned") \
	X(ROWS_VETTED, "rows_vetted", "rows checked against the heap for visibility") \
	X(ROWS_RECHECKED, "rows_rechecked", "rows of dirty units read again from the heap") \
	X(UNITS_RECHECKED, "units_rechecked", "units that had such rows") \
	X(UNITS_NO_ROLLUP, "units_no_rollup", "exact units scanned for lack of their rollup") \
	X(UNITS_ROLLUP_COST, "units_rollup_cost", "exact units scanned because their rollup cost more than their elements") \
	X(LOOKUP_HITS, "lookup_cache_hits", "lookup tables taken from the cache of an earlier query") \
	X(LOOKUP_MISSES, "lookup_cache_misses", "lookup tables the cache could have held and that were loaded")

typedef enum SiftStat
{
#define SIFT_STAT_ID(id, name, text) SIFT_STAT_##id,
	SIFT_STATS(SIFT_STAT_ID)
#undef SIFT_STAT_ID
	SIFT_STAT_COUNT
} SiftStat;

extern int64 sift_stat_v[SIFT_STAT_COUNT];

static inline void
sift_stat_add(SiftStat s, int64 n)
{
	sift_stat_v[s] += n;
}

/* a scan of the relation (index, else table) could not use its pre-aggregates for the reason (a static string), and `units` of it were scanned for it */
extern void sift_stats_preagg_miss(Oid rel, const char *slug, bool fixable, int64 units);

/* a query kept the normal plan for the refusal named by slug (a static string; agg/refuse.h) */
extern void sift_stats_refusal(const char *slug, const char *limit);

#endif
