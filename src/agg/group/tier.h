/*
 * agg/group/tier.h - the groups of a count, sum, min and max of an int8 value by an int8 key, in the structure that costs the least for the groups there
 * are, and in the next when the groups outgrow it, with no error and no restart: the tiers of SiftGroupTier.
 *
 *   DICT_DIRECT, DIRECT_INT  a flat array of the accumulators of keys in [base, base + span), span at most SIFT_TIER_DIRECT_MAX, that grows with the highest key:
 *                            no hash, one cache line for two groups (a dictionary code is a key whose base is 0)
 *   SALTED_HT                the table of agg/group/ht.h
 *   RADIX_SPILL              the table, small, gathers the groups of the rows; when it is full they go to the files of agg/group/spill.h by the hash kept in each slot
 * A key outside the span, or an array that would be more than SIFT_SPILL_SOFT_PERCENT of the budget, sends the groups to the table, each by one upsert of the hash of
 * its key; a table that would take more than SIFT_SPILL_SOFT_PERCENT of the budget when it grows sends its groups to the files. Nothing in the memory is lost on
 * the way: the same groups come out of every tier.
 */
#ifndef SIFT_AGG_GROUP_TIER_H
#define SIFT_AGG_GROUP_TIER_H

#include "agg/group/spill.h"

#define SIFT_TIER_DIRECT_MAX	65536

typedef enum SiftGroupTier
{
	SIFT_GRP_UNGROUPED = 0,		/* no key: one accumulator */
	SIFT_GRP_DICT_DIRECT = 1,
	SIFT_GRP_DIRECT_INT = 2,
	SIFT_GRP_SALTED_HT = 3,
	SIFT_GRP_RADIX_SPILL = 4
} SiftGroupTier;

typedef struct SiftTierGroups SiftTierGroups;

/* The groups of keys that are expected in [base, base + span) (span 0: no expectation), in a tier from start, within budget bytes */
extern SiftTierGroups *sift_tier_new(SiftGroupTier start, int64 base, uint32 span, uint64 budget);
extern void sift_tier_add(SiftTierGroups *g, int64 key, int64 val);
/* Every group once, in no order: the groups of the files are merged one partition at a time */
typedef void (*SiftTierEmit) (void *arg, int64 key, const int64 agg[4]);
extern void sift_tier_emit(SiftTierGroups *g, SiftTierEmit emit, void *arg);
/* The tier now, the tiers it has been in (a bit mask by tier), and the bytes of the files */
extern SiftGroupTier sift_tier_of(const SiftTierGroups *g);
extern uint32 sift_tier_visited(const SiftTierGroups *g);
extern uint64 sift_tier_spilled_bytes(const SiftTierGroups *g);
/* The deepest level of partitions the files needed, once the groups are emitted */
extern uint32 sift_tier_spill_depth(const SiftTierGroups *g);
extern void sift_tier_free(SiftTierGroups *g);

#endif							/* SIFT_AGG_GROUP_TIER_H */
