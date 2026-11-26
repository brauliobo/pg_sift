/*
 * core/shm/manifest_int.h - what manifest.c (the readers, the area, the leases) and manifest_pub.c (the writers of the slots) share of the registry of the manifests in shared memory.
 */
#ifndef SIFT_SHM_MANIFEST_INT_H
#define SIFT_SHM_MANIFEST_INT_H

#include "core/shm/manifest.h"

/* a slot that was used and holds nothing now: the probe of a lookup goes past it (an empty slot ends it) */
#define SIFT_SHM_SLOT_DEAD	(1U << 3)

/* a slot whose index is committing a version that the registry does not hold yet: readers find nothing, and a meta of an older version is not published over it */
#define SIFT_SHM_SLOT_HELD	(1U << 4)

typedef struct Table
{
	int			tranche;
	LWLock		claim_lock;
	LWLock		area_lock;
	dsa_handle	area_handle;	/* area_lock */
	uint32		area_users;		/* area_lock */
	pg_atomic_uint64 clock;
	pg_atomic_uint32 snaps_live;
	pg_atomic_uint64 snap_bytes;
	pg_atomic_uint32 unleased;	/* the runs of vblocks that backends fill with no lease slot */
	pg_atomic_uint64 global_epoch pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* bumped when a snapshot is retired */
	LWLock		retire_lock;
	dsa_pointer retired_head;	/* the snapshots retired and not freed yet, newest first (retire_lock) */
	pg_atomic_uint64 epoch_mask[SIFT_SHM_EPOCH_SLOTS / 64];	/* the backends that have pinned in an epoch slot and not exited: all that oldest_epoch reads */
	SiftEpochSlot epoch[SIFT_SHM_EPOCH_SLOTS];
	SiftShmIndexSlot slot[SIFT_SHM_MANIFEST_SLOTS];
	SiftShmTidLease lease[SIFT_SHM_TID_LEASES];
} Table;

extern Table *sift_shm_registry(void);
extern SiftShmIndexSlot *sift_shm_slot_at(Table *t, const RelFileLocator *loc, int probe);
extern dsa_area *sift_shm_area_attach(Table *t, bool create);
extern dsa_pointer sift_shm_slot_swap_snap(SiftShmIndexSlot *s, dsa_pointer dp);
extern void sift_shm_snap_retire(Table *t, dsa_area *a, dsa_pointer dp);
extern dsa_pointer sift_shm_snap_build(Table *t, dsa_area *a, const RelFileLocator *locator, const SiftShmSnapSpec *spec);
extern void sift_shm_snap_free(dsa_area *a, SiftShmManifestSnap *snap);
extern void sift_shm_meta_write(SiftShmIndexSlot *s, const SiftShmIndexMeta *meta);

#endif
