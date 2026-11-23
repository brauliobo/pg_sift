/*
 * agg/v6/v6_distinct_u64.h - the exact set of the 64-bit values of an ungrouped COUNT(DISTINCT integer column): one open-addressing table of 8 byte values and a 1 byte tag each (0 empty, else 0x80
 * and 7 bits of the hash, which tells two values apart before the values are compared), where the generic set of v6_distinct.h pays a record, a hash and a partition for each value. The set is held
 * to a budget: sift_du64_add stops at the value whose growth would pass it, and the caller moves the values to the generic set (sift_du64_each), which spills.
 */
#ifndef SIFT_AGG_V6_DISTINCT_U64_H
#define SIFT_AGG_V6_DISTINCT_U64_H

#include "postgres.h"

typedef struct SiftDistinctU64 SiftDistinctU64;

extern SiftDistinctU64 *sift_du64_create(MemoryContext cx, uint64 budget_bytes);
/* The n values; the table grows only for a value that is new. Returns n, or the index of the first value that is new and does not fit the budget: those before it are in the set, and the caller moves it to the generic set (sift_du64_each) with the rest */
extern uint32 sift_du64_add(SiftDistinctU64 *d, const uint64 *vals, uint32 n);
extern uint64 sift_du64_count(const SiftDistinctU64 *d);
extern uint64 sift_du64_bytes(const SiftDistinctU64 *d);
extern void sift_du64_each(const SiftDistinctU64 *d, void (*fn) (void *arg, uint64 v), void *arg);
extern void sift_du64_free(SiftDistinctU64 *d);

/*
 * The grouped set: pairs (group, value) of a GROUP BY with COUNT(DISTINCT integer column). A group is the dense number its caller already has (the ext_idx of its slot in the group table), so a pair
 * is one entry of 16 bytes (the value and the group) in one open-addressing table and the count of its group goes up when a pair is new: no second table finds the group. The set says no, and adds
 * nothing, when what it would need passes the budget: the caller moves the pairs to the generic set (sift_du64g_pairs) and goes on there.
 */
typedef struct SiftDu64G SiftDu64G;

typedef void (*SiftDu64GGroup) (void *arg, uint32 gid, uint64 count);
typedef void (*SiftDu64GPair) (void *arg, uint32 gid, uint64 value);

extern SiftDu64G *sift_du64g_create(MemoryContext cx, uint64 budget_bytes);
extern bool sift_du64g_add(SiftDu64G *d, uint32 gid, uint64 value);
/*
 * The n pairs (gids[i], vals[i]), the entries prefetched ahead. Returns n, or the index of the first pair that is new and does not fit the budget: the pairs before it are in the set, and the
 * caller moves it to the generic set (sift_du64g_pairs) and adds the rest there.
 */
extern uint32 sift_du64g_add_batch(SiftDu64G *d, const uint32 *gids, const uint64 *vals, uint32 n);
/* each group that has a pair, with the number of its distinct values */
extern void sift_du64g_groups(const SiftDu64G *d, SiftDu64GGroup fn, void *arg);
extern void sift_du64g_pairs(const SiftDu64G *d, SiftDu64GPair fn, void *arg);
extern uint64 sift_du64g_bytes(const SiftDu64G *d);
extern void sift_du64g_free(SiftDu64G *d);

#endif							/* SIFT_AGG_V6_DISTINCT_U64_H */
