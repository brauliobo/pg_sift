/*
 * core/shm_manifest.c - the registry of the manifests of the indexes in shared memory (core/shm_manifest.h).
 *
 * Writers (publish, invalidate, the eviction of a slot, the teardown of the area) take claim_lock, so a slot has one writer at a time: it moves the seqlock of line 0 from even to odd,
 * writes the meta, and back to even. A reader takes no lock: it copies the meta between two reads of the seqlock and tries again if they differ or the first is odd. The snapshot of a slot is pinned
 * inside an epoch (SiftEpochSlot): a snapshot nobody holds is retired and freed once no reader is in an epoch up to the one it was retired in, so a reader never pins a snapshot that is
 * freed under it, and takes no lock. A backend beyond the epoch slots pins under the build_lock of the slot (shared; the writer that swaps the snapshot holds it exclusive).
 *
 * Lock order: claim_lock, then area_lock, then a build_lock or the retire_lock. The area is attached before the first snapshot is touched; a backend that has attached counts itself in area_users and the last one
 * to leave frees every snapshot (they are in the area that goes with it) and gives their bytes back to the ledger.
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "col/dsmreg.h"
#include "core/cachebudget.h"
#include "core/shm_manifest.h"
#include "core/shmbudget.h"

#define REGISTRY_NAME	"sift_shm_manifest"
#define TRANCHE_NAME	"sift_shm_manifest"

/* a slot that was used and holds nothing now: the probe of a lookup goes past it (an empty slot ends it) */
#define SIFT_SHM_SLOT_DEAD	(1U << 3)

/* the header of a list of a partition in the area: the bytes it was charged */
typedef struct ListHdr
{
	uint64		bytes;
	uint64		pad;
} ListHdr;

StaticAssertDecl(sizeof(ListHdr) % MAXIMUM_ALIGNOF == 0, "the list of a partition after its header is aligned for the entries a reader casts it to");

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

static Table *table;
static dsa_area *area;
static MemoryContext area_cx;
static bool exit_registered;

static void epoch_leave(int code, Datum arg);

static void snap_pin_release(Datum res);
static char *snap_pin_debug(Datum res);

static const ResourceOwnerDesc snap_pin_desc = {
	.name = "sift manifest snapshot pin",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FIRST,
	.ReleaseResource = snap_pin_release,
	.DebugPrint = snap_pin_debug
};

static Table *
table_base(void *raw)
{
	return (Table *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
table_init(SIFT_DSM_INIT_ARGS)
{
	Table	   *t = table_base(ptr);

	memset(t, 0, sizeof(Table));
	t->tranche = sift_tranche_new(TRANCHE_NAME);
	LWLockInitialize(&t->claim_lock, t->tranche);
	LWLockInitialize(&t->area_lock, t->tranche);
	LWLockInitialize(&t->retire_lock, t->tranche);
	pg_atomic_init_u64(&t->global_epoch, 1);
	t->retired_head = InvalidDsaPointer;
	for (int i = 0; i < SIFT_SHM_EPOCH_SLOTS; i++)
		pg_atomic_init_u64(&t->epoch[i].epoch, 0);
	for (int i = 0; i < SIFT_SHM_EPOCH_SLOTS / 64; i++)
		pg_atomic_init_u64(&t->epoch_mask[i], 0);
	t->area_handle = DSA_HANDLE_INVALID;
	pg_atomic_init_u64(&t->clock, 1);
	pg_atomic_init_u32(&t->snaps_live, 0);
	pg_atomic_init_u64(&t->snap_bytes, 0);
	pg_atomic_init_u32(&t->unleased, 0);
	for (int i = 0; i < SIFT_SHM_TID_LEASES; i++)
	{
		pg_atomic_init_u32(&t->lease[i].owner, 0);
		pg_atomic_init_u64(&t->lease[i].lo, 0);
		pg_atomic_init_u64(&t->lease[i].seq, 0);
	}
	for (int i = 0; i < SIFT_SHM_MANIFEST_SLOTS; i++)
	{
		SiftShmIndexSlot *s = &t->slot[i];

		pg_atomic_init_u64(&s->seq, 0);
		pg_atomic_init_u64(&s->current_snap_dp, InvalidDsaPointer);
		pg_atomic_init_u64(&s->last_used_tick, 0);
		pg_atomic_init_u64(&s->snap_hits, 0);
		pg_atomic_init_u64(&s->snap_misses, 0);
		LWLockInitialize(&s->build_lock, t->tranche);
	}
}

bool
sift_shm_manifest_enabled(void)
{
	return sift_shared_cache_kb > 0 && !RecoveryInProgress();
}

static Table *
registry(void)
{
	if (table == NULL)
	{
		/* MEM[MEM-SHM-03]: DSM registry sift_shm_manifest (1024 epochs + 512 slots + 2048 vblock leases, fixed) */
		void	   *raw = sift_dsm_try(REGISTRY_NAME, sizeof(Table) + PG_CACHE_LINE_SIZE, table_init);

		if (raw == NULL)
			return NULL;
		table = table_base(raw);
		sift_tranche_register(table->tranche, TRANCHE_NAME);
		before_shmem_exit(epoch_leave, 0);
	}
	return table;
}

/* A backend that exits inside a pin (FATAL runs no PG_FINALLY) must not leave its epoch announced: no snapshot of the cluster would be freed again */
static void
epoch_leave(int code, Datum arg)
{
	if ((uint32) MyProcNumber < SIFT_SHM_EPOCH_SLOTS)
	{
		pg_atomic_write_u64(&table->epoch[MyProcNumber].epoch, 0);
		pg_atomic_fetch_and_u64(&table->epoch_mask[MyProcNumber / 64], ~(UINT64CONST(1) << (MyProcNumber % 64)));
	}
}

/* The first pin of a backend puts its epoch slot where the reclaimer looks, once: the pin of every query would otherwise write a line that all backends share */
static void
epoch_register(Table *t)
{
	static bool registered;

	if (registered)
		return;
	pg_atomic_fetch_or_u64(&t->epoch_mask[MyProcNumber / 64], UINT64CONST(1) << (MyProcNumber % 64));
	registered = true;
}

static SiftShmIndexSlot *
slot_at(Table *t, const RelFileLocator *loc, int probe)
{
	uint32		h = hash_bytes((const unsigned char *) loc, sizeof(RelFileLocator));

	return &t->slot[(h + probe) & (SIFT_SHM_MANIFEST_SLOTS - 1)];
}

/* ---------- the area ---------- */

static void area_leave(int code, Datum arg);

/*
 * The area, attached by this backend: made where there is none and create is true; NULL where the system has no memory for it, or where there is none and create is false.
 * Takes area_lock, so a caller that holds claim_lock may call it.
 */
static dsa_area *
area_attach(Table *t, bool create)
{
	MemoryContext old;
	dsa_area   *a;
	uint32		holdoff;

	if (area != NULL)
		return area;
	if (area_cx == NULL)
		/* MEM[MEM-IDLE-13]: cachebudget manifest dsa_area handle ctx (~42 KB per attached backend) */
		area_cx = sift_cache_create_context("sift cache manifest area");
	old = MemoryContextSwitchTo(area_cx);
	LWLockAcquire(&t->area_lock, LW_EXCLUSIVE);
	holdoff = InterruptHoldoffCount;
	PG_TRY();
	{
#ifdef SIFT_TESTING
		if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "manifest_area") != NULL)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift test: the area of the manifests cannot be attached (manifest_area)")));	/* check-errors: allow */
#endif
		if (t->area_handle != DSA_HANDLE_INVALID)
			a = dsa_attach(t->area_handle);
		else if (create)
		{
			/* MEM[MEM-CORE-10]: DSM-backed dsa_create/dsa_attach for shared manifest snapshots */
			a = dsa_create(t->tranche);
			t->area_handle = dsa_get_handle(a);
		}
		else
			a = NULL;
	}
	PG_CATCH();
	{
		ErrorData  *e;
		bool		shortage;

		InterruptHoldoffCount = holdoff;
		LWLockRelease(&t->area_lock);
		MemoryContextSwitchTo(old);
		e = CopyErrorData();
		shortage = ERRCODE_TO_CATEGORY(e->sqlerrcode) == ERRCODE_INSUFFICIENT_RESOURCES;
		FreeErrorData(e);
		if (!shortage)
			PG_RE_THROW();
		FlushErrorState();
		return NULL;
	}
	PG_END_TRY();
	if (a != NULL)
	{
		dsa_pin_mapping(a);
		t->area_users++;
	}
	LWLockRelease(&t->area_lock);
	MemoryContextSwitchTo(old);
	if (a != NULL && !exit_registered)
	{
		before_shmem_exit(area_leave, 0);
		exit_registered = true;
	}
	area = a;
	return a;
}

static void snap_free(dsa_area *a, SiftShmManifestSnap *snap);

/* The snapshot of a slot, swapped out under the build_lock of the slot: the dsa_pointer the slot had */
static dsa_pointer
slot_swap_snap(SiftShmIndexSlot *s, dsa_pointer dp)
{
	dsa_pointer old;

	LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
	old = pg_atomic_exchange_u64(&s->current_snap_dp, dp);
	LWLockRelease(&s->build_lock);
	return old;
}

/* The oldest epoch a reader is in (UINT64_MAX: none is) */
static uint64
oldest_epoch(Table *t)
{
	uint64		oldest = UINT64_MAX;

	for (int w = 0; w < SIFT_SHM_EPOCH_SLOTS / 64; w++)
		for (uint64 bits = pg_atomic_read_u64(&t->epoch_mask[w]); bits != 0; bits &= bits - 1)
		{
			uint64		e = pg_atomic_read_u64(&t->epoch[w * 64 + pg_rightmost_one_pos64(bits)].epoch);

			if (e != 0)
				oldest = Min(oldest, e & ~SIFT_EPOCH_ACTIVE_BIT);
		}
	return oldest;
}

#define RECLAIM_BATCH	64

/*
 * Takes off the retired list up to RECLAIM_BATCH snapshots that no reader can be reading (none is in an epoch up to the one they were retired in) or holds, all of them where nobody is
 * attached any more (retire_lock held). Their memory is given back by reclaim_now after the lock is let go: nothing else reaches a snapshot that is off the list
 */
static int
reclaim_unlink(Table *t, dsa_area *a, bool everything, dsa_pointer *batch)
{
	uint64		oldest = everything ? UINT64_MAX : oldest_epoch(t);
	dsa_pointer *link = &t->retired_head;
	int			n = 0;

	while (*link != InvalidDsaPointer && n < RECLAIM_BATCH)
	{
		SiftShmManifestSnap *snap = dsa_get_address(a, *link);

		if (everything || (snap->retire_epoch < oldest && pg_atomic_read_u32(&snap->refcount) == 0))
		{
			batch[n++] = *link;
			*link = snap->retire_next;
		}
		else
			link = &snap->retire_next;
	}
	return n;
}

static void
reclaim_now(Table *t, dsa_area *a, bool everything)
{
	dsa_pointer batch[RECLAIM_BATCH];
	int			n;

	do
	{
		LWLockAcquire(&t->retire_lock, LW_EXCLUSIVE);
		n = reclaim_unlink(t, a, everything, batch);
		LWLockRelease(&t->retire_lock);
		for (int i = 0; i < n; i++)
			snap_free(a, dsa_get_address(a, batch[i]));
	} while (n == RECLAIM_BATCH);
}

static void
snap_retire(Table *t, dsa_area *a, dsa_pointer dp)
{
	SiftShmManifestSnap *snap = dsa_get_address(a, dp);

	pg_atomic_write_u32(&snap->retired, 1);
	pg_memory_barrier();
	LWLockAcquire(&t->retire_lock, LW_EXCLUSIVE);
	snap->retire_epoch = pg_atomic_fetch_add_u64(&t->global_epoch, 1);
	snap->retire_next = t->retired_head;
	t->retired_head = dp;
	pg_atomic_sub_fetch_u32(&snap->refcount, 1);
	LWLockRelease(&t->retire_lock);
	reclaim_now(t, a, false);
}

/* A reader lets go of its count: the last one of a retired snapshot has it freed */
static void
snap_decref(dsa_area *a, dsa_pointer dp)
{
	SiftShmManifestSnap *snap = dsa_get_address(a, dp);

	if (pg_atomic_sub_fetch_u32(&snap->refcount, 1) == 0 && pg_atomic_read_u32(&snap->retired))
		reclaim_now(table, a, false);
}

static void
meta_write(SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
{
	uint64		seq = pg_atomic_read_u64(&s->seq);

	Assert((seq & 1) == 0);
	pg_atomic_write_u64(&s->seq, seq + 1);
	pg_write_barrier();
	memcpy(&s->meta, meta, sizeof(SiftShmIndexMeta));
	pg_write_barrier();
	pg_atomic_write_u64(&s->seq, seq + 2);
}

/*
 * The last backend of the area frees what it holds: the snapshots go with the area, and the meta they were the snapshots of stays. The pins of an open transaction are released with it,
 * which the exit of a backend would do only after this callback (the callbacks run last in, first out, and the abort is registered first)
 */
static void
area_leave(int code, Datum arg)
{
	Table	   *t = table;

	AbortOutOfAnyTransaction();
	if (area == NULL)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	LWLockAcquire(&t->area_lock, LW_EXCLUSIVE);
	if (--t->area_users == 0)
	{
		for (int i = 0; i < SIFT_SHM_MANIFEST_SLOTS; i++)
		{
			SiftShmIndexSlot *s = &t->slot[i];
			dsa_pointer dp = slot_swap_snap(s, InvalidDsaPointer);

			if (dp != InvalidDsaPointer)
			{
				SiftShmIndexMeta meta = s->meta;

				meta.state_flags &= ~SIFT_SHM_SLOT_HAS_SNAP;
				meta_write(s, &meta);
				snap_retire(t, area, dp);
			}
		}
		reclaim_now(t, area, true);
		t->area_handle = DSA_HANDLE_INVALID;
	}
	dsa_detach(area);
	area = NULL;
	MemoryContextDelete(area_cx);
	area_cx = NULL;
	LWLockRelease(&t->area_lock);
	LWLockRelease(&t->claim_lock);
}

/* ---------- the snapshots ---------- */

/*
 * The snapshot of spec, one reader (the registry); InvalidDsaPointer where the ledger or the system has no room for it. The header, the partition entries, the scope entries and the
 * table of lists are one allocation, so that making or freeing a snapshot takes the lock of the area once, whatever the partitions it has; the lists a query reads later are allocations of
 * their own
 */
/* MEM[MEM-CORE-11]: DSA snapshot sub-alloc helper (coalesce into MEM-SHM-04) */
static dsa_pointer
snap_build(Table *t, dsa_area *a, const RelFileLocator *locator, const SiftShmSnapSpec *spec)
{
	Size		entries = (Size) spec->npartitions * spec->part_entry_size;
	Size		scopes = (Size) spec->nscopes * spec->scope_entry_size;
	Size		lists = (Size) spec->npartitions * sizeof(pg_atomic_uint64);
	Size		entries_at = MAXALIGN(sizeof(SiftShmManifestSnap));
	Size		scopes_at = entries_at + MAXALIGN(entries);
	Size		lists_at = scopes_at + MAXALIGN(scopes);
	Size		total = lists_at + MAXALIGN(lists);
	dsa_pointer self;
	SiftShmManifestSnap *snap;

	if (!sift_shm_reserve_shared(SIFT_SHM_MANIFEST, total))
		return InvalidDsaPointer;
	/* MEM[MEM-SHM-04]: DSA manifest snapshot single contiguous alloc (charged to SIFT_SHM_MANIFEST) */
	self = dsa_allocate_extended(a, total, DSA_ALLOC_ZERO | DSA_ALLOC_NO_OOM);
	if (self == InvalidDsaPointer)
	{
		sift_shm_release_shared(SIFT_SHM_MANIFEST, total);
		return InvalidDsaPointer;
	}
	snap = dsa_get_address(a, self);
	pg_atomic_init_u32(&snap->refcount, 1);
	pg_atomic_init_u32(&snap->retired, 0);
	snap->locator = *locator;
	snap->manifest_version = spec->manifest_version;
	snap->scope_dir_gen = spec->scope_dir_gen;
	snap->npartitions = spec->npartitions;
	snap->nscopes = spec->nscopes;
	snap->part_entry_size = spec->part_entry_size;
	snap->scope_entry_size = spec->scope_entry_size;
	snap->part_entries_dp = entries > 0 ? self + entries_at : InvalidDsaPointer;
	snap->part_lists_dp = lists > 0 ? self + lists_at : InvalidDsaPointer;
	snap->scopes_dp = scopes > 0 ? self + scopes_at : InvalidDsaPointer;
	snap->self_dp = self;
	snap->base_bytes = total;
	pg_atomic_init_u64(&snap->snap_bytes, total);
	if (entries > 0)
		memcpy((char *) snap + entries_at, spec->part_entries, entries);
	if (scopes > 0)
		memcpy((char *) snap + scopes_at, spec->scopes, scopes);
	for (uint32 p = 0; p < spec->npartitions; p++)
		pg_atomic_init_u64(&((pg_atomic_uint64 *) ((char *) snap + lists_at))[p], InvalidDsaPointer);
	pg_atomic_fetch_add_u32(&t->snaps_live, 1);
	pg_atomic_fetch_add_u64(&t->snap_bytes, total);
	return self;
}

/* One free for the snapshot; the lists that queries installed are each an allocation, given back before it. No lock of the registry is held by the callers that matter (reclaim_now) */
static void
snap_free(dsa_area *a, SiftShmManifestSnap *snap)
{
	Table	   *t = table;
	pg_atomic_uint64 *lists = dsa_get_address(a, snap->part_lists_dp);

	for (uint32 p = 0; lists != NULL && p < snap->npartitions; p++)
	{
		dsa_pointer lp = pg_atomic_read_u64(&lists[p]);

		if (lp != InvalidDsaPointer)
		{
			sift_shm_release_shared(SIFT_SHM_MANIFEST, ((ListHdr *) dsa_get_address(a, lp))->bytes);
			dsa_free(a, lp);
		}
	}
	sift_shm_release_shared(SIFT_SHM_MANIFEST, snap->base_bytes);
	pg_atomic_fetch_sub_u64(&t->snap_bytes, pg_atomic_read_u64(&snap->snap_bytes));
	pg_atomic_fetch_sub_u32(&t->snaps_live, 1);
	dsa_free(a, snap->self_dp);
}

/* ---------- readers ---------- */

/* The slot of the locator, or NULL, found without a lock: the seqlock makes the copy of its meta whole */
static SiftShmIndexSlot *
slot_lookup(Table *t, const RelFileLocator *loc, SiftShmIndexMeta *meta)
{
	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = slot_at(t, loc, i);
		uint64		seq;

		for (;;)
		{
			seq = pg_atomic_read_u64(&s->seq);
			if (seq & 1)
			{
				pg_spin_delay();
				continue;
			}
			pg_read_barrier();
			memcpy(meta, &s->meta, sizeof(SiftShmIndexMeta));
			pg_read_barrier();
			if (pg_atomic_read_u64(&s->seq) == seq)
				break;
		}
		if (meta->state_flags == SIFT_SHM_SLOT_EMPTY)
			return NULL;
		if (RelFileLocatorEquals(meta->locator, *loc) && (meta->state_flags & SIFT_SHM_SLOT_VALID))
		{
			if (pg_atomic_read_u64(&s->last_used_tick) != pg_atomic_read_u64(&t->clock))
				pg_atomic_write_u64(&s->last_used_tick, pg_atomic_read_u64(&t->clock));
			return s;
		}
	}
	return NULL;
}

bool
sift_shm_manifest_peek(const RelFileLocator *locator, SiftShmIndexMeta *out)
{
	Table	   *t;

	if (!sift_shm_manifest_enabled() || (t = registry()) == NULL)
		return false;
	return slot_lookup(t, locator, out) != NULL;
}

/* The snapshot of the slot, counted in its refcount, under the build_lock of the slot; NULL where it has none */
static SiftShmManifestSnap *
pin_locked(SiftShmIndexSlot *s, dsa_area *a)
{
	dsa_pointer dp;
	SiftShmManifestSnap *snap = NULL;

	LWLockAcquire(&s->build_lock, LW_SHARED);
	dp = pg_atomic_read_u64(&s->current_snap_dp);
	if (dp != InvalidDsaPointer)
	{
		snap = dsa_get_address(a, dp);
		pg_atomic_fetch_add_u32(&snap->refcount, 1);
	}
	LWLockRelease(&s->build_lock);
	return snap;
}

/*
 * The same without a lock: the backend announces the epoch it enters, reads the pointer and counts itself in the refcount, and leaves the epoch. The full barrier after the announcement
 * makes either the writer that retires the snapshot see the announcement or this backend see the pointer that replaced it; a snapshot that was retired in between has no holder left (its
 * count was 0), the count is given back and the pointer read again.
 */
static SiftShmManifestSnap *
pin_in_epoch(Table *t, SiftShmIndexSlot *s, dsa_area *a)
{
	pg_atomic_uint64 *mine = &t->epoch[MyProcNumber].epoch;
	SiftShmManifestSnap *snap;
	bool		retired;

	epoch_register(t);
	do
	{
		snap = NULL;
		retired = false;
		pg_atomic_write_u64(mine, pg_atomic_read_u64(&t->global_epoch) | SIFT_EPOCH_ACTIVE_BIT);
		pg_memory_barrier();
		PG_TRY();
		{
			dsa_pointer dp = pg_atomic_read_u64(&s->current_snap_dp);

			if (dp != InvalidDsaPointer)
			{
				snap = dsa_get_address(a, dp);
				pg_atomic_fetch_add_u32(&snap->refcount, 1);
				if (pg_atomic_read_u32(&snap->retired))
					retired = true;
			}
		}
		PG_FINALLY();
		{
			pg_write_barrier();
			pg_atomic_write_u64(mine, 0);
		}
		PG_END_TRY();
		if (retired)
			snap_decref(a, snap->self_dp);
	} while (retired);
	return snap;
}

static const SiftShmManifestSnap *
pin_missed(SiftShmIndexSlot *s)
{
	pg_atomic_fetch_add_u64(&s->snap_misses, 1);
	return NULL;
}

/* The snapshot of the slot that holds `looked_up`, pinned for the reader of `expected`: the slot may have been given to another index since the lookup, and then its snapshot is not the reader's */
static const SiftShmManifestSnap *
pin_found_for(const RelFileLocator *looked_up, const RelFileLocator *expected)
{
	Table	   *t;
	SiftShmIndexMeta meta;
	SiftShmIndexSlot *s;
	dsa_area   *a;
	SiftShmManifestSnap *snap;

	if (!sift_shm_manifest_enabled() || (t = registry()) == NULL)
		return NULL;
	s = slot_lookup(t, looked_up, &meta);
	if (s == NULL)
		return NULL;
	if (!(meta.state_flags & SIFT_SHM_SLOT_HAS_SNAP) || (a = area_attach(t, false)) == NULL)
		return pin_missed(s);
	ResourceOwnerEnlarge(CurrentResourceOwner);
	snap = (uint32) MyProcNumber < SIFT_SHM_EPOCH_SLOTS ? pin_in_epoch(t, s, a) : pin_locked(s, a);
	if (snap == NULL)
		return pin_missed(s);
	if (!RelFileLocatorEquals(snap->locator, *expected))
	{
		snap_decref(a, snap->self_dp);
		return NULL;
	}
	ResourceOwnerRemember(CurrentResourceOwner, (Datum) snap->self_dp, &snap_pin_desc);
	pg_atomic_fetch_add_u64(&s->snap_hits, 1);
	return snap;
}

bool
sift_shm_manifest_pin_stat(const RelFileLocator *locator, uint64 *hits, uint64 *misses)
{
	Table	   *t;
	SiftShmIndexMeta meta;
	SiftShmIndexSlot *s;

	if (!sift_shm_manifest_enabled() || (t = registry()) == NULL || (s = slot_lookup(t, locator, &meta)) == NULL)
		return false;
	*hits = pg_atomic_read_u64(&s->snap_hits);
	*misses = pg_atomic_read_u64(&s->snap_misses);
	return true;
}

const SiftShmManifestSnap *
sift_shm_snap_pin(const RelFileLocator *locator)
{
	return pin_found_for(locator, locator);
}

#ifdef SIFT_TESTING
const SiftShmManifestSnap *
sift_shm_snap_pin_found_for(const RelFileLocator *looked_up, const RelFileLocator *expected)
{
	return pin_found_for(looked_up, expected);
}

void
sift_shm_manifest_announce_epoch(void)
{
	Table	   *t = registry();

	if (t != NULL && (uint32) MyProcNumber < SIFT_SHM_EPOCH_SLOTS)
	{
		epoch_register(t);
		pg_atomic_write_u64(&t->epoch[MyProcNumber].epoch, pg_atomic_read_u64(&t->global_epoch) | SIFT_EPOCH_ACTIVE_BIT);
	}
}
#endif

void
sift_shm_snap_unpin(const SiftShmManifestSnap *snap)
{
	ResourceOwnerForget(CurrentResourceOwner, (Datum) snap->self_dp, &snap_pin_desc);
	snap_decref(area, snap->self_dp);
}

static void
snap_pin_release(Datum res)
{
	snap_decref(area, (dsa_pointer) res);
}

static char *
snap_pin_debug(Datum res)
{
	return psprintf("manifest snapshot " UINT64_FORMAT, (uint64) res);
}

const void *
sift_shm_snap_part_entries(const SiftShmManifestSnap *snap)
{
	return snap->part_entries_dp == InvalidDsaPointer ? NULL : dsa_get_address(area, snap->part_entries_dp);
}

const void *
sift_shm_snap_scopes(const SiftShmManifestSnap *snap)
{
	return snap->scopes_dp == InvalidDsaPointer ? NULL : dsa_get_address(area, snap->scopes_dp);
}

static pg_atomic_uint64 *
part_list_slot(const SiftShmManifestSnap *snap, uint32 p)
{
	Assert(p < snap->npartitions);
	return &((pg_atomic_uint64 *) dsa_get_address(area, snap->part_lists_dp))[p];
}

static void *
list_payload(dsa_pointer lp)
{
	return (char *) dsa_get_address(area, lp) + sizeof(ListHdr);
}

const void *
sift_shm_snap_part_list(const SiftShmManifestSnap *snap, uint32 p)
{
	dsa_pointer lp = pg_atomic_read_u64(part_list_slot(snap, p));

	if (lp == InvalidDsaPointer)
		return NULL;
	pg_read_barrier();
	return list_payload(lp);
}

bool
sift_shm_snap_part_list_install(const SiftShmManifestSnap *snapshot, uint32 p, const void *data, Size bytes)
{
	SiftShmManifestSnap *snap = (SiftShmManifestSnap *) snapshot;
	pg_atomic_uint64 *slot = part_list_slot(snap, p);
	Size		total = sizeof(ListHdr) + bytes;
	dsa_pointer lp = pg_atomic_read_u64(slot),
				mine;

	if (lp != InvalidDsaPointer || !sift_shm_reserve_shared(SIFT_SHM_MANIFEST, total))
		return false;
	mine = dsa_allocate_extended(area, total, DSA_ALLOC_NO_OOM);
	if (mine == InvalidDsaPointer)
	{
		sift_shm_release_shared(SIFT_SHM_MANIFEST, total);
		return false;
	}
	((ListHdr *) dsa_get_address(area, mine))->bytes = total;
	memcpy(list_payload(mine), data, bytes);
	if (pg_atomic_compare_exchange_u64(slot, &lp, mine))
	{
		pg_atomic_fetch_add_u64(&snap->snap_bytes, total);
		pg_atomic_fetch_add_u64(&table->snap_bytes, total);
		return true;
	}
	dsa_free(area, mine);
	sift_shm_release_shared(SIFT_SHM_MANIFEST, total);
	return false;
}

/* ---------- writers ---------- */

/* What a slot costs to give up: a dead one nothing, a live one its cache; a quarantined one is not given up, it keeps the index from being trusted */
static int
slot_value(SiftShmIndexSlot *s)
{
	return (s->meta.state_flags & SIFT_SHM_SLOT_DEAD) ? 0 : 1;
}

/* The slot of the locator in the probe window, or NULL (claim_lock held) */
static SiftShmIndexSlot *
slot_find(Table *t, const RelFileLocator *loc)
{
	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = slot_at(t, loc, i);

		if (s->meta.state_flags == SIFT_SHM_SLOT_EMPTY)
			break;
		if (RelFileLocatorEquals(s->meta.locator, *loc))
			return s;
	}
	return NULL;
}

/* The slot of the locator for a writer (claim_lock held): the one it has, an empty one, or the one a full probe window gives up (a dead one first, then the least recently used); NULL when the window holds nothing but quarantined indexes */
static SiftShmIndexSlot *
slot_claim(Table *t, const RelFileLocator *loc)
{
	SiftShmIndexSlot *victim = NULL;

	for (int i = 0; i < SIFT_SHM_MANIFEST_PROBES; i++)
	{
		SiftShmIndexSlot *s = slot_at(t, loc, i);

		if (s->meta.state_flags == SIFT_SHM_SLOT_EMPTY || RelFileLocatorEquals(s->meta.locator, *loc))
			return s;
		if (s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED)
			continue;
		if (victim == NULL || slot_value(s) < slot_value(victim) ||
			(slot_value(s) == slot_value(victim) && pg_atomic_read_u64(&s->last_used_tick) < pg_atomic_read_u64(&victim->last_used_tick)))
			victim = s;
	}
	if (victim != NULL)
		sift_shm_note_eviction(SIFT_SHM_MANIFEST, 1);
	return victim;
}

/* The snapshot a slot holds goes: the area is attached first, so that the snapshot can be freed (claim_lock held) */
static void
slot_drop_snap(Table *t, SiftShmIndexSlot *s)
{
	dsa_area   *a;

	if (pg_atomic_read_u64(&s->current_snap_dp) == InvalidDsaPointer)
		return;
#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "manifest_drop_snap") != NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift test: the snapshot of the slot cannot be dropped (manifest_drop_snap)")));	/* check-errors: allow */
#endif
	a = area_attach(t, false);
	if (a == NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: the area of the manifest snapshots cannot be attached"),
						errhint("Shared memory is short; the snapshots are held until a session that can attach the area drops them.")));
	snap_retire(t, a, slot_swap_snap(s, InvalidDsaPointer));
}

bool
sift_shm_manifest_publish(const SiftShmIndexMeta *meta, const SiftShmSnapSpec *spec)
{
	Table	   *t;
	dsa_area   *a = NULL;
	dsa_pointer dp = InvalidDsaPointer;
	SiftShmIndexSlot *s;
	SiftShmIndexMeta m = *meta;

	if (!sift_shm_manifest_enabled() || (t = registry()) == NULL)
		return false;
	if (spec != NULL && (a = area_attach(t, true)) != NULL)
		dp = snap_build(t, a, &meta->locator, spec);
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = slot_claim(t, &meta->locator);
	if (s == NULL || ((s->meta.state_flags & SIFT_SHM_SLOT_QUARANTINED) && RelFileLocatorEquals(s->meta.locator, meta->locator)) ||
		((s->meta.state_flags & SIFT_SHM_SLOT_VALID) && RelFileLocatorEquals(s->meta.locator, meta->locator) && s->meta.manifest_version > meta->manifest_version))
	{
		LWLockRelease(&t->claim_lock);
		if (dp != InvalidDsaPointer)
			snap_free(a, dsa_get_address(a, dp));
		return false;
	}
	if (!RelFileLocatorEquals(s->meta.locator, meta->locator))
	{
		pg_atomic_write_u64(&s->snap_hits, 0);
		pg_atomic_write_u64(&s->snap_misses, 0);
	}
	PG_TRY();
	{
		slot_drop_snap(t, s);
	}
	PG_CATCH();
	{
		if (dp != InvalidDsaPointer)
			snap_free(a, dsa_get_address(a, dp));
		PG_RE_THROW();
	}
	PG_END_TRY();
	m.state_flags = SIFT_SHM_SLOT_VALID | (dp != InvalidDsaPointer ? SIFT_SHM_SLOT_HAS_SNAP : 0);
	meta_write(s, &m);
	pg_atomic_write_u64(&s->last_used_tick, pg_atomic_fetch_add_u64(&t->clock, 1));
	if (dp != InvalidDsaPointer)
		slot_swap_snap(s, dp);
	LWLockRelease(&t->claim_lock);
	return spec == NULL || dp != InvalidDsaPointer;
}

void
sift_shm_manifest_invalidate(const RelFileLocator *locator, bool quarantine)
{
	Table	   *t;
	SiftShmIndexSlot *s;

	if (!sift_shm_manifest_enabled() || (t = registry()) == NULL)
		return;
	LWLockAcquire(&t->claim_lock, LW_EXCLUSIVE);
	s = quarantine ? slot_claim(t, locator) : slot_find(t, locator);
	if (s != NULL)
	{
		bool		mine = RelFileLocatorEquals(s->meta.locator, *locator);
		SiftShmIndexMeta m = mine ? s->meta : (SiftShmIndexMeta) {.locator = *locator};

		if (!mine)
		{
			pg_atomic_write_u64(&s->snap_hits, 0);
			pg_atomic_write_u64(&s->snap_misses, 0);
		}
		m.state_flags = quarantine ? SIFT_SHM_SLOT_QUARANTINED : SIFT_SHM_SLOT_DEAD;
		meta_write(s, &m);
		slot_drop_snap(t, s);
	}
	LWLockRelease(&t->claim_lock);
}

void
sift_shm_manifest_stats(SiftShmManifestStats *stats)
{
	Table	   *t = registry();
	SiftShmPoolStat pool;

	memset(stats, 0, sizeof(*stats));
	if (t == NULL)
		return;
	for (int i = 0; i < SIFT_SHM_MANIFEST_SLOTS; i++)
		if (t->slot[i].meta.state_flags & SIFT_SHM_SLOT_VALID)
			stats->slots_valid++;
	stats->snaps_live = pg_atomic_read_u32(&t->snaps_live);
	stats->snap_bytes = pg_atomic_read_u64(&t->snap_bytes);
	sift_shm_pool_stat(SIFT_SHM_MANIFEST, &pool);
	stats->evictions = pool.evictions;
	for (int i = 0; i < SIFT_SHM_EPOCH_SLOTS; i++)
		stats->epochs_active += pg_atomic_read_u64(&t->epoch[i].epoch) != 0;
	LWLockAcquire(&t->area_lock, LW_SHARED);
	stats->area_users = t->area_users;
	LWLockRelease(&t->area_lock);
	stats->area_bytes = area == NULL ? 0 : dsa_get_total_size(area);
}

/* ---------- the vblock leases ---------- */

int
sift_shm_tid_lease_acquire(const RelFileLocator *locator)
{
	Table	   *t = registry();

	for (int i = 0; t != NULL && i < SIFT_SHM_TID_LEASES; i++)
	{
		SiftShmTidLease *l = &t->lease[i];
		uint32		free_slot = 0;

		if (!pg_atomic_compare_exchange_u32(&l->owner, &free_slot, SIFT_TID_LEASE_CLAIMING))
			continue;
		(void) pg_atomic_fetch_add_u64(&l->seq, 1);
		l->locator = *locator;
		pg_atomic_write_u64(&l->lo, 0);
		pg_write_barrier();
		pg_atomic_write_u32(&l->owner, (uint32) MyProcNumber + 1);
		return i;
	}
	return -1;
}

void
sift_shm_tid_lease_set(int slot, uint64 lo)
{
	pg_atomic_write_u64(&table->lease[slot].lo, lo);
}

void
sift_shm_tid_lease_release(int slot)
{
	pg_atomic_write_u32(&table->lease[slot].owner, 0);
}

bool
sift_shm_tid_unleased(int delta)
{
	Table	   *t = registry();

	if (t != NULL)
		pg_atomic_fetch_add_u32(&t->unleased, (uint32) delta);
	return t != NULL;
}

bool
sift_shm_tid_lease_floor(const RelFileLocator *locator, uint64 next_vblock, uint64 *floor)
{
	Table	   *t = registry();

	if (t == NULL)
		return false;
	*floor = pg_atomic_read_u32(&t->unleased) > 0 ? 0 : next_vblock;
	for (int i = 0; i < SIFT_SHM_TID_LEASES; i++)
	{
		SiftShmTidLease *l = &t->lease[i];
		uint64		seq = pg_atomic_read_u64(&l->seq),
					lo;
		uint32		owner = pg_atomic_read_u32(&l->owner);
		bool		mine;

		pg_read_barrier();
		if (owner == 0)
			continue;
		mine = RelFileLocatorEquals(l->locator, *locator);
		lo = pg_atomic_read_u64(&l->lo);
		pg_read_barrier();
		if (owner == SIFT_TID_LEASE_CLAIMING || pg_atomic_read_u64(&l->seq) != seq)
			*floor = 0;
		else if (mine)
			*floor = Min(*floor, lo);
	}
	return true;
}
