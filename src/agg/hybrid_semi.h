/*
 * agg/hybrid_semi.h - SiftHybridAclSet: the keys of the row set a semi join (EXISTS (SELECT 1 FROM tenant_links ..)) lets through, drained once from the plan of the other side of the join
 * and handed to the columnar scan as an IN list, so that its key marks and zones skip what the set misses. The parent semi join stays in the plan: the set is a sound pre-filter, never
 * the answer, and a set that outgrows its cap is dropped (the scan then runs as it would without the join). Portable (core/host.h).
 */
#ifndef SIFT_HYBRID_SEMI_H
#define SIFT_HYBRID_SEMI_H

#include "core/host.h"

#define SIFT_HYB_ACL_MAX_KEYS		65536	/* the keys a set holds (SIFT_SEMI_MAX_SET of core/limits.h); more and it overflows */
#define SIFT_HYB_ACL_PUSH_MAX		4096	/* the keys the scan takes as an IN list; a larger set filters the rows the scan hands over */
#define SIFT_HYB_ACL_MEM_DIV		8		/* the set holds at most work_mem / 8 bytes */
#define SIFT_HYB_ACL_BLOOM_BITS		4096

typedef struct SiftHybridAclSet
{
	MemoryContext mcxt;
	bool		is_str;				/* uuid / text keys, else int2 / int4 / int8 */
	bool		overflowed;			/* a key past the cap was offered: the set is no pre-filter */
	bool		empty;				/* finished with no key: no row of the other side, the query returns nothing */
	bool		finished;
	uint32		nvals;				/* deduplicated after finish */
	uint32		cap_keys;
	uint32		arr_cap;			/* entries the key arrays hold room for */
	Size		cap_bytes;
	Size		bytes;
	int64	   *ival;
	const uint8 **sptr;
	uint32	   *slen;
	uint64		bloom[SIFT_HYB_ACL_BLOOM_BITS / 64];
} SiftHybridAclSet;

/* A set of at most max_keys keys (SIFT_HYB_ACL_MAX_KEYS at the most) and mem_bytes bytes */
extern SiftHybridAclSet *sift_acl_begin(MemoryContext parent, bool is_str, uint32 max_keys, Size mem_bytes);
extern void sift_acl_end(SiftHybridAclSet *set);

/* false once the set has overflowed: the caller stops draining */
extern bool sift_acl_add_int(SiftHybridAclSet *set, int64 v);
extern bool sift_acl_add_str(SiftHybridAclSet *set, const uint8 *p, uint32 len);

/* Sorts and deduplicates, builds the Bloom filter, sets empty */
extern void sift_acl_finish(SiftHybridAclSet *set);

extern bool sift_acl_has_int(const SiftHybridAclSet *set, int64 v);
extern bool sift_acl_has_str(const SiftHybridAclSet *set, const uint8 *p, uint32 len);

/* The scan can take the set as an IN list */
static inline bool
sift_acl_pushable(const SiftHybridAclSet *set)
{
	return set->finished && !set->overflowed && set->nvals > 0 && set->nvals <= SIFT_HYB_ACL_PUSH_MAX;
}

#endif							/* SIFT_HYBRID_SEMI_H */
