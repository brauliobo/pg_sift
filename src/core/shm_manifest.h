/*
 * core/shm_manifest.h - the one registry of the manifests of the indexes in shared memory.
 *
 * A table of 512 slots of two cache lines, in a table of the dynamic shared memory registry (col/dsmreg.h: no shared_preload_libraries is needed). Line 0 of a slot is the
 * meta of the index in a seqlock: a reader copies it without a lock and without reading Block 0 (sift_shm_manifest_peek). Line 1 anchors an immutable snapshot
 * (the partition entries, the lazily loaded partition lists, the scope entries) in a shared dsa_area. The snapshot is counted by its readers and the registry, and freed by the last one
 * that lets go; its bytes are charged to the manifest pool of the ledger (core/shmbudget.h), so a snapshot that does not fit is not made and the caller reads the manifest itself.
 *
 * The area is made by the first backend that needs a snapshot and is gone with the last: a backend that has attached counts itself in the registry, and the last one to leave
 * frees every snapshot before it detaches. Where sift.shared_cache_kb is 0 or the server is in recovery the registry is not used: peek finds nothing, publish does nothing.
 */
#ifndef SIFT_SHM_MANIFEST_H
#define SIFT_SHM_MANIFEST_H

#include "postgres.h"

#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"
#include "utils/dsa.h"

#define SIFT_SHM_MANIFEST_SLOTS		512
#define SIFT_SHM_MANIFEST_PROBES	8

#define SIFT_SHM_SLOT_EMPTY			0U
#define SIFT_SHM_SLOT_VALID			(1U << 0)
#define SIFT_SHM_SLOT_QUARANTINED	(1U << 1)
#define SIFT_SHM_SLOT_HAS_SNAP		(1U << 2)

/*
 * Reclamation by epochs (quiescent states): a reader that pins a snapshot without the lock of its slot announces, in a cache line of its own, the epoch it entered, for the few
 * instructions between reading the snapshot's pointer and counting itself in its refcount. A snapshot nobody holds is not freed at once but retired with the epoch it left, and freed
 * when no reader announces an epoch up to it and none holds it: a reader that read the pointer before the swap cannot find the memory gone. A reader that counts itself in a snapshot
 * that is retired gives its count back and reads the pointer again.
 */
#define SIFT_SHM_EPOCH_SLOTS		256
#define SIFT_EPOCH_ACTIVE_BIT		(UINT64CONST(1) << 63)

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftEpochSlot
{
	pg_atomic_uint64 epoch;		/* 0: not in a pin; else the epoch entered | SIFT_EPOCH_ACTIVE_BIT */
	char		pad[PG_CACHE_LINE_SIZE - sizeof(pg_atomic_uint64)];
} SiftEpochSlot;

StaticAssertDecl(sizeof(SiftEpochSlot) == PG_CACHE_LINE_SIZE, "the epoch of a backend occupies one cache line");

/* What a reader copies out of line 0 of a slot */
typedef struct SiftShmIndexMeta
{
	RelFileLocator locator;
	uint32		state_flags;	/* SIFT_SHM_SLOT_* */
	uint64		manifest_version;
	uint64		scope_dir_gen;
	BlockNumber partdir_root_blk;
	BlockNumber scope_dir_blk;
	uint16		partdir_level;
	uint16		nscopes;
	uint32		npartitions;
	uint64		total_rows;
	uint64		total_dead;
	uint64		total_bytes;
	TransactionId oldest_active_xid;
	uint32		meta_flags;
	uint64		compat_flags;
	uint64		ro_compat_flags;
	uint16		inline_off;
	uint16		inline_len;
} SiftShmIndexMeta;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftShmIndexSlot
{
	/* line 0: the seqlock (even: stable, odd: a writer is in it) and the meta */
	pg_atomic_uint64 seq;
	SiftShmIndexMeta meta;
	char		pad_line0[PG_CACHE_LINE_SIZE - sizeof(pg_atomic_uint64) - sizeof(SiftShmIndexMeta)];

	/* line 1: the snapshot of the manifest in the dsa_area (InvalidDsaPointer: none), the use clock, the lock that the readers beyond the epoch slots take to pin the snapshot and the writer takes to swap it */
	pg_atomic_uint64 current_snap_dp;
	pg_atomic_uint64 last_used_tick;
	pg_atomic_uint64 snap_hits;	/* the pins of the snapshot that found it, and those that did not, since the slot was taken by its index (sift.status shm_hit_pct) */
	pg_atomic_uint64 snap_misses;
	LWLock		build_lock;
	char		pad_line1[PG_CACHE_LINE_SIZE - 4 * sizeof(pg_atomic_uint64) - sizeof(LWLock)];
} SiftShmIndexSlot;

StaticAssertDecl(offsetof(SiftShmIndexSlot, current_snap_dp) == PG_CACHE_LINE_SIZE, "line 0 of a slot of the manifest registry is one cache line");
StaticAssertDecl(sizeof(SiftShmIndexSlot) == 2 * PG_CACHE_LINE_SIZE, "a slot of the manifest registry is two cache lines");

/* The immutable snapshot of a manifest in the dsa_area */
typedef struct SiftShmManifestSnap
{
	pg_atomic_uint32 refcount;	/* the registry and every reader that pinned it */
	pg_atomic_uint32 retired;	/* 1 once the registry let go of it: a reader that counts itself in it gives the count back */
	RelFileLocator locator;		/* the index it is the snapshot of: a slot that was given to another index while a reader looked it up hands that reader this one's */
	uint32		flags;
	uint64		retire_epoch;	/* the epoch it was retired in (retire_lock) */
	dsa_pointer retire_next;	/* the next snapshot retired and not freed yet (retire_lock) */
	uint64		manifest_version;
	uint64		scope_dir_gen;
	uint32		npartitions;
	uint32		nscopes;
	dsa_pointer part_entries_dp;	/* npartitions entries of part_entry_size bytes */
	dsa_pointer part_lists_dp;	/* pg_atomic_uint64[npartitions]: the dsa_pointer of the list of a partition, InvalidDsaPointer until a query reads it */
	dsa_pointer scopes_dp;		/* nscopes entries of scope_entry_size bytes */
	uint32		part_entry_size;
	uint32		scope_entry_size;
	dsa_pointer self_dp;
	uint64		base_bytes;		/* the bytes charged when the snapshot was made (the lists are charged one by one) */
	pg_atomic_uint64 snap_bytes;	/* the bytes charged to the manifest pool for this snapshot, its lists included */
} SiftShmManifestSnap;

/* What a commit publishes besides the meta: the entries are copied into the snapshot */
typedef struct SiftShmSnapSpec
{
	uint64		manifest_version;
	uint64		scope_dir_gen;
	uint32		npartitions;
	uint32		nscopes;
	const void *part_entries;
	uint32		part_entry_size;
	const void *scopes;
	uint32		scope_entry_size;
} SiftShmSnapSpec;

typedef struct SiftShmManifestStats
{
	uint32		slots_valid;
	uint32		snaps_live;
	uint64		snap_bytes;		/* charged to the manifest pool for the live snapshots */
	uint32		area_users;		/* backends attached to the dsa_area */
	uint32		epochs_active;	/* backends inside a pin */
	uint64		area_bytes;		/* what the dsa_area holds from the system, as this backend sees it (0: not attached) */
	uint64		evictions;
} SiftShmManifestStats;

/* Whether the registry is used: sift.shared_cache_kb is above 0 and the server is not in recovery */
extern bool sift_shm_manifest_enabled(void);

/* Copies the meta of the index into *out without a lock; false: the registry does not know it (or holds it quarantined) */
extern bool sift_shm_manifest_peek(const RelFileLocator *locator, SiftShmIndexMeta *out);

/*
 * Publishes the meta of an index (the commit of a manifest) and, where snap is not NULL, the snapshot that goes with it: the snapshot replaces the one the slot had, which is freed
 * when its last reader lets go. False where snap was given and the ledger refused its bytes: the meta is published without a snapshot and readers load the manifest themselves.
 */
extern bool sift_shm_manifest_publish(const SiftShmIndexMeta *meta, const SiftShmSnapSpec *snap);

/* The index is no longer what the registry says (DROP, REINDEX, a damaged manifest): readers find nothing, the snapshot is released; quarantine keeps the slot as one that is not to be trusted */
extern void sift_shm_manifest_invalidate(const RelFileLocator *locator, bool quarantine);

/* The pins of the snapshot of the index that found it and that did not, since the registry took the index; false: the registry has no slot for it */
extern bool sift_shm_manifest_pin_stat(const RelFileLocator *locator, uint64 *hits, uint64 *misses);

/* Pins the snapshot of the index until sift_shm_snap_unpin (or the end of the transaction, which warns of a pin left); NULL: there is none (or the slot went to another index meanwhile) */
extern const SiftShmManifestSnap *sift_shm_snap_pin(const RelFileLocator *locator);
extern void sift_shm_snap_unpin(const SiftShmManifestSnap *snap);

/* The partition entries, the scope entries and the lists of a pinned snapshot, as addresses of this backend */
extern const void *sift_shm_snap_part_entries(const SiftShmManifestSnap *snap);
extern const void *sift_shm_snap_scopes(const SiftShmManifestSnap *snap);

/*
 * The list of partition p of a pinned snapshot: NULL until sift_shm_snap_part_list_install makes it. Install makes a zeroed list of bytes (charged to the pool and counted in the snapshot) and
 * returns the one that is there if another backend was first.
 */
extern void *sift_shm_snap_part_list(const SiftShmManifestSnap *snap, uint32 p);
extern void *sift_shm_snap_part_list_install(const SiftShmManifestSnap *snap, uint32 p, Size bytes);

extern void sift_shm_manifest_stats(SiftShmManifestStats *stats);

#ifdef SIFT_TESTING
/* A reader that found the slot of `looked_up` and pins it for `expected`: the slot went to another index between the two */
extern const SiftShmManifestSnap *sift_shm_snap_pin_found_for(const RelFileLocator *looked_up, const RelFileLocator *expected);
/* This backend announces an epoch as one inside a pin does; only its exit (or a pin) takes it back */
extern void sift_shm_manifest_announce_epoch(void);
#endif

#endif
