/*
 * col/f2/ttl.h - the retention of a sift_table (sift.set_ttl): which parts are wholly older than the retention and can leave the manifest in one commit.
 *
 * The drop is the one of ClickHouse's ttl_only_drop_parts: a part goes when the newest row it has is older than the cutoff. A settled part that holds rows on both sides of the cutoff is first
 * compacted into one part of each side (sift_partition_cut_online), so that its older rows leave too; one that is not settled yet stays whole, its older rows visible, until a later step.
 * The drop reads, deletes and logs no row: it is the removal of descriptors from a part list in a commit of the manifest and the release of the blocks of the parts it names. The column
 * of the TTL is NOT NULL (sift.set_ttl checks it): the time bounds of a part are those of its non-null values, so a row without a time would be dropped with its part.
 */
#ifndef SIFT_F2_TTL_H
#define SIFT_F2_TTL_H

#include "core/host.h"

#include "core/f2/manifest/manifest.h"
#include "core/f2/tier.h"

typedef enum SiftTtlVerdict
{
	SIFT_TTL_KEEP,				/* every row is inside the retention */
	SIFT_TTL_PARTIAL,			/* some rows are outside it and some inside: kept whole */
	SIFT_TTL_EXPIRED,			/* every row is outside it */
	SIFT_TTL_NO_BOUNDS			/* the part has no bounds for the slot (an empty range): kept */
} SiftTtlVerdict;

/* slot: the time column of the manifest (0 or 1) the TTL column is; cutoff: the least time inside the retention, in the unit of the column */
extern SiftTtlVerdict sift_ttl_classify(const SiftPartDesc *d, int slot, int64 cutoff);

/* A part whose rows are settled can leave in a commit: its inserters are frozen or known committed, and none is newer than the horizon (the oldest transaction id that may still be running) */
extern bool sift_ttl_droppable(const SiftPartDesc *d, FullTransactionId horizon);

typedef struct SiftTtlPlan
{
	uint64	   *drop_gens;		/* wholly expired and settled: leave the part list in the commit */
	int			ndrop;
	uint64	   *mark_gens;		/* wholly expired, not yet settled: get SIFT_PART_EXT_EXPIRED until a later step drops them */
	int			nmark;
	uint64	   *unmark_gens;	/* flagged expired and no longer wholly expired (the retention grew): lose the flag */
	int			nunmark;
	int			npartial;
	int			nkeep;
} SiftTtlPlan;

/* The parts that hold rows on both sides of the cutoff and are settled, which a compaction cuts at it: their generations in gens (room for n); how many */
extern int	sift_ttl_cut_gens(const SiftPartDesc *parts, int n, int slot, int64 cutoff, FullTransactionId horizon, uint64 *gens);

extern SiftTtlPlan *sift_ttl_plan(const SiftPartDesc *parts, int n, int slot, int64 cutoff, FullTransactionId horizon);
extern void sift_ttl_plan_free(SiftTtlPlan *plan);

/* The generations of the refs whose objects are due for deletion at now (sift_tier_ref_expire); the count (gens has room for n) */
extern int	sift_ttl_due_deletes(const SiftTierPartRef *refs, int n, TimestampTz now, uint64 *gens);

#endif							/* SIFT_F2_TTL_H */
