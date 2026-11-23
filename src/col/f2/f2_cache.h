/*
 * col/f2/f2_cache.h - the partition directory and the part lists of an index of format major 2, parsed once per backend and kept under the budget of sift.backend_cache_kb.
 *
 * A snapshot is valid while Block 0 still holds the (manifest_version, scope_dir_gen) it was read at: every commit of the manifest bumps the version, so one buffer read tells a hit from a
 * miss. A miss reloads the directory (one to three pages) and the part list of only the partitions whose manifest_gen moved.
 */
#ifndef SIFT_F2_CACHE_H
#define SIFT_F2_CACHE_H

#include "postgres.h"

#include "storage/relfilelocator.h"
#include "utils/rel.h"

#include "core/f2/manifest.h"

typedef struct SiftPlanPartList
{
	uint64		part_id;
	uint64		manifest_gen;	/* the manifest_gen of the partition entry the parts were read at */
	int			nparts;
	SiftPartDesc *parts;
} SiftPlanPartList;

typedef struct SiftPlanSnapshot
{
	RelFileLocator locator;		/* first: the hash key */
	uint64		manifest_version;
	uint64		scope_dir_gen;
	uint64		total_rows;
	uint64		total_dead;
	uint64		total_bytes;
	int			npartitions;
	SiftPartitionEntry *partitions;	/* in key order */
	SiftPlanPartList *partlists;	/* [npartitions], the list of partitions[i] */
	int			nscopes;
	SiftManifestScopeRef *scopes;	/* ascending by scope_ord */
	struct SiftCardStatsView *card_view;	/* the statistics folded for the planner (f2_v6_card.c), one allocation in cx; NULL until built, and again when the directory is read anew */
	bool		card_built;		/* a build was tried for this version of the manifest (it may have left no view) */
	bool		slabs_in_memory;	/* a copy made for a statement of a standby: its partitions list their micro-slabs as parts (col/f2/f2_slab_view.h), so nothing is kept of it */
	MemoryContext cx;
	Size		size;
	int			pins;
	uint64		used;
	dlist_node	node;
} SiftPlanSnapshot;

/* The snapshot of the index as of Block 0 now, valid until mcxt is reset: shared with the backend's later statements while the cache has room for it, private to mcxt else */
extern const SiftPlanSnapshot *sift_plan_cache_get(Relation rel, MemoryContext mcxt);

/* The statistics of the snapshot's version of the manifest, kept with it: a view allocated in snap->cx (one block) is accounted to the snapshot; NULL stands for an index that has none to give */
extern bool sift_plan_card_built(const SiftPlanSnapshot *snap);
extern const struct SiftCardStatsView *sift_plan_card_get(const SiftPlanSnapshot *snap);
extern void sift_plan_card_put(const SiftPlanSnapshot *snap, struct SiftCardStatsView *view);

/* Drops every snapshot nobody pins */
extern void sift_plan_cache_evict_all(void);

/* What a TRUNCATE of a table made in the transaction leaves: the file stays and its manifest versions start again, so a snapshot of it must not be taken for one of the new rows */
extern void sift_plan_cache_forget(const RelFileLocator *locator);

/* The entry of the partition in the snapshot; an ERROR when it has none */
extern const SiftPartitionEntry *sift_plan_entry(const SiftPlanSnapshot *snap, uint64 part_id);

#ifdef SIFT_TESTING
typedef struct SiftPlanCacheStats
{
	int64		hits;
	int64		refreshes;
	int64		builds;
	int64		private_builds;
	int64		partlists_read;
	int64		shm_loads;		/* directories and lists copied out of the snapshot the registry in shared memory holds */
	int64		shm_publishes;	/* snapshots this backend read from the pages and gave the registry */
} SiftPlanCacheStats;

extern SiftPlanCacheStats sift_plan_cache_stats;
#endif

#endif
