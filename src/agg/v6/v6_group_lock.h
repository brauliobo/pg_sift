/*
 * agg/v6/v6_group_lock.h - GROUP BY .. LIMIT K with no ORDER BY, HAVING or DISTINCT (SiftV6Spec.lock_k): any K groups with their exact aggregates answer it. The group table takes the first K groups
 * (SiftSaltedHT.lock_at), and from then on a row of another group is dropped before its aggregates are read; when the first key is an integer column the rows are first cut to those whose first key
 * is one of the K groups', by one vector compare of the batch.
 */
#ifndef SIFT_AGG_V6_GROUP_LOCK_H
#define SIFT_AGG_V6_GROUP_LOCK_H

#include "postgres.h"

#include "agg/group/ht.h"
#include "agg/v6/v6_group_fast.h"

#define SIFT_LOCK_SET_MAX	255

typedef struct SiftLockSet
{
	bool		built;
	bool		usable;			/* the first key of the K groups is a non-NULL integer in at most SIFT_LOCK_SET_MAX values */
	uint8		n;
	int64		v[SIFT_LOCK_SET_MAX];
} SiftLockSet;

/* The first keys of the groups of ht (as made for the key forms: packed, wide, or one integer) */
extern void sift_lock_set_build(SiftLockSet *ls, const SiftSaltedHT *ht, bool packed, const SiftPackSpec *ps, bool int_key0);
/* out = the rows of mask whose first key (the vector k0 of the batch) is in the set; returns how many */
extern uint16 sift_lock_set_rows(const SiftLockSet *ls, const SiftVec *k0, const uint64 *mask, uint16 n_raw, uint64 *out);
/* The rows sel[0 .. n-1] whose slot is not NULL, with their slots, to the front; returns their number */
extern uint16 sift_lock_compact(uint16 *sel, SiftAggSlot64 **slots, uint16 n);

#endif							/* SIFT_AGG_V6_GROUP_LOCK_H */
