/*
 * agg/distinct/u64.h - the exact set of the 64-bit values of an ungrouped COUNT(DISTINCT integer column): one open-addressing table of 8 byte values (0 is an empty entry, and the value 0 a flag),
 * where the generic set of v6_distinct.h pays a record, a hash and a partition for each value. The set is held
 * to a budget: sift_du64_add stops at the value whose growth would pass it, and the caller moves the values to the generic set (sift_du64_each), which spills.
 */
#ifndef SIFT_AGG_V6_DISTINCT_U64_H
#define SIFT_AGG_V6_DISTINCT_U64_H

#include "postgres.h"

/*
 * The part of the sets that calls no server function: the hash, and the deduplication of values and of pairs in a table the caller made (the sets above make theirs of the memory of the server, the
 * threads of v6_par_radix_dist.c of an arena). A table has a power of 2 of entries and room for what it is given: the caller grows it before it is full.
 */
typedef struct SiftDuPair
{
	uint64		val;
	uint32		gid1;			/* the group + 1: 0 is an empty entry */
	uint32		h32;			/* the low half of the hash of the pair */
} SiftDuPair;

static inline uint64
sift_du64_mix(uint64 x)
{
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	return x ^ (x >> 32);
}

static inline uint64
sift_du64_pair_hash(uint64 v, uint32 gid)
{
	return sift_du64_mix(v ^ ((uint64) gid + 1) * UINT64CONST(0x9e3779b97f4a7c15));
}

/* The values of vals[0 .. n-1] that are not 0 and are new to tab (0 is an empty entry: the caller keeps the value 0 itself); returns how many were new */
extern uint32 sift_du64_dedup_slice(uint64 *tab, uint64 cap, const uint64 *vals, uint32 n);
/* The entries of src (scap of them) into the empty table dst of dcap entries, which is twice as large or more: no entry is compared, they are all different (a pair is placed by the hash of its value and group) */
extern void sift_du64_rehash(uint64 *dst, uint64 dcap, const uint64 *src, uint64 scap);
extern void sift_du64g_rehash(SiftDuPair *dst, uint64 dcap, const SiftDuPair *src, uint64 scap);
/* The pairs p[0 .. n-1] that are new to tab, moved to the front of p in their order; returns how many */
extern uint32 sift_du64g_dedup_bucket(SiftDuPair *tab, uint64 cap, SiftDuPair *p, uint32 n);

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
 *
 * A set whose table outgrows the cache keeps its pairs in 64 buckets by the top bits of their hash instead: a pair is appended to its bucket, and a bucket is deduplicated in a table that fits
 * the cache (the work is linear in the pairs, and no pair is a miss), so that the pairs of a scan of millions of rows cost a store, not a probe of a table of tens of megabytes.
 */
typedef struct SiftDu64G SiftDu64G;

typedef void (*SiftDu64GGroup) (void *arg, uint32 gid, uint64 count);
typedef void (*SiftDu64GPair) (void *arg, uint32 gid, uint64 value);

extern SiftDu64G *sift_du64g_create(MemoryContext cx, uint64 budget_bytes);
/* The same, with the number of table entries at which the pairs go to the buckets of the radix mode (below) in place of a larger table */
extern SiftDu64G *sift_du64g_create_ex(MemoryContext cx, uint64 budget_bytes, uint64 radix_cap);
extern bool sift_du64g_add(SiftDu64G *d, uint32 gid, uint64 value);
/*
 * The n pairs (gids[i], vals[i]), the entries prefetched ahead. Returns n, or the index of the first pair that is new and does not fit the budget: the pairs before it are in the set, and the
 * caller moves it to the generic set (sift_du64g_pairs) and adds the rest there.
 */
extern uint32 sift_du64g_add_batch(SiftDu64G *d, const uint32 *gids, const uint64 *vals, uint32 n);
/* each group that has a pair, with the number of its distinct values (the buckets of the radix mode are deduplicated first) */
extern void sift_du64g_groups(SiftDu64G *d, SiftDu64GGroup fn, void *arg);
extern void sift_du64g_pairs(SiftDu64G *d, SiftDu64GPair fn, void *arg);
extern uint64 sift_du64g_bytes(const SiftDu64G *d);
extern void sift_du64g_free(SiftDu64G *d);

#endif							/* SIFT_AGG_V6_DISTINCT_U64_H */
