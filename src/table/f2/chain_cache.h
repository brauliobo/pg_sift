/*
 * table/f2/chain_cache.h - the pending deletes and the column delta chain of a partition, kept by row for the writes that ask about one row after another.
 *
 * A write to a row asks what the partition's pending deletes and its column delta chain say of that row. Reading and checking the pages for every row costs the length of the chain for
 * each row of a statement; the log of a partition is read once and indexed by row, and stays while the pages it was read from are the pages that are there: the page of the chain that
 * grows (the last of the pending deletes, the newest of the column delta chain) is compared by its log sequence number, and the manifest entry names the rest, which a change of the
 * chain replaces. The logs are one of the caches of core/cachebudget.h.
 *
 * A pointer a function returns holds until the next call into the cache.
 */
#ifndef SIFT_TBL_F2_CHAIN_CACHE_H
#define SIFT_TBL_F2_CHAIN_CACHE_H

#include "col/f2/del/pend.h"
#include "core/f2/manifest/manifest.h"
#include "table/f2/col_delta/col_delta.h"

typedef struct SiftPendDelLog SiftPendDelLog;
typedef struct SiftColDeltaLog SiftColDeltaLog;

/* The pending deletes of the partition of the entry as they are now, NULL when it has none */
extern const SiftPendDelLog *sift_pend_del_log(Relation rel, const SiftPartitionEntry *entry);

/* The newest record of the row at tid (its pair's second slot not counted) and, from a record, the one before it for the same row; -1 for none */
extern int	sift_pend_del_log_newest(const SiftPendDelLog *l, uint64 tid);
extern int	sift_pend_del_log_prev(const SiftPendDelLog *l, int i);
extern int	sift_pend_del_log_count(const SiftPendDelLog *l);
extern const SiftPendDelRec *sift_pend_del_log_rec(const SiftPendDelLog *l, int i);

/* The column delta chain of the partition of the entry, whole and checked, NULL when it has none */
extern const SiftColDeltaLog *sift_col_delta_log(Relation rel, const SiftPartitionEntry *entry);

/* Whether the log of the chain fits a quarter of the budget of the caches: a page of the chain costs the log three pages of memory, the page and the index of its records */
extern bool sift_col_delta_log_fits(const SiftColDeltaAnchor *anchor);

/* The newest record of the row at tid, and the one before it; -1 for none */
extern int	sift_col_delta_log_newest(const SiftColDeltaLog *l, uint64 tid);
extern int	sift_col_delta_log_prev(const SiftColDeltaLog *l, int i);
extern const uint8 *sift_col_delta_log_rec(const SiftColDeltaLog *l, int i);

/* The cache holds nothing of the relation any more (its files changed) */
extern void sift_chain_cache_forget_rel(Oid relid);

#ifdef SIFT_TESTING
/* The pages of column delta chains the cache has read since the backend started (sift._test_chain_cache_pages) */
extern uint64 sift_chain_cache_pages;
#endif

#endif							/* SIFT_TBL_F2_CHAIN_CACHE_H */
