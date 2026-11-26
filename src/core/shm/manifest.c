/*
 * core/shm/manifest.c - the registry of the manifests of the indexes in shared memory (core/shm/manifest.h).
 *
 * Writers (hold, publish and invalidate in manifest_pub.c, the eviction of a slot, the teardown of the area) write a slot under its build_lock, so a slot has one writer at a time: it moves the seqlock of line 0 from even to odd,
 * writes the meta, and back to even. A writer of an index the registry holds takes only that lock; one that must find or give up a slot takes claim_lock first. A reader takes no lock: it copies the meta between two reads of the seqlock and tries again if they differ or the first is odd. The snapshot of a slot is pinned
 * inside an epoch (SiftEpochSlot): a snapshot nobody holds is retired and freed once no reader is in an epoch up to the one it was retired in, so a reader never pins a snapshot that is
 * freed under it, and takes no lock. A backend beyond the epoch slots pins under the build_lock of the slot (shared; the writer that swaps the snapshot holds it exclusive).
 *
 * Lock order: claim_lock, then area_lock, then a build_lock or the retire_lock; the snapshots a writer takes out of a slot are retired after its locks are let go. The area is attached before the first snapshot is touched; a backend that has attached counts itself in area_users and the last one
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
#include "core/shm/manifest_int.h"
#include "core/shm/budget.h"

#define REGISTRY_NAME	"sift_shm_manifest"
#define TRANCHE_NAME	"sift_shm_manifest"

/* the header of a list of a partition in the area: the bytes it was charged */
typedef struct ListHdr
{
	uint64		bytes;
	uint64		pad;
} ListHdr;

StaticAssertDecl(sizeof(ListHdr) % MAXIMUM_ALIGNOF == 0, "the list of a partition after its header is aligned for the entries a reader casts it to");

static Table *table;
static dsa_area *area;
static MemoryContext area_cx;
static bool exit_registered;

static void epoch_leave(int code, Datum arg);
static void hits_flush(void);

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
	pg_atomic_init_u64(&t->list_copies, 0);
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

Table *
sift_shm_registry(void)
{
	if (table == NULL)
	{
		/* MEM[MEM-SHM-03]: DSM registry sift_shm_manifest (1024 epochs + 2048 slots + 2048 vblock leases, fixed) */
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
	hits_flush();
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

SiftShmIndexSlot *
sift_shm_slot_at(Table *t, const RelFileLocator *loc, int probe)
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
dsa_area *
sift_shm_area_attach(Table *t, bool create)
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


/* The snapshot of a slot, swapped out under the build_lock of the slot: the dsa_pointer the slot had */
dsa_pointer
sift_shm_slot_swap_snap(SiftShmIndexSlot *s, dsa_pointer dp)
{
	dsa_pointer old;

	LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
	old = pg_atomic_exchange_u64(&s->current_snap_dp, dp);
	LWLockRelease(&s->build_lock);
	return old;
}

#define TICK_EVERY		64

/* The clock moves once in TICK_EVERY publications of a backend, and the slots are marked with it as it reads: LRU does not need more, and no publication writes the line of the clock */
uint64
sift_shm_tick(Table *t)
{
	static uint32 publications;

	if ((++publications % TICK_EVERY) == 0)
		pg_atomic_fetch_add_u64(&t->clock, 1);
	return pg_atomic_read_u64(&t->clock);
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
reclaim_free(dsa_area *a, const dsa_pointer *batch, int n)
{
	for (int i = 0; i < n; i++)
		sift_shm_snap_free(a, dsa_get_address(a, batch[i]));
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
		reclaim_free(a, batch, n);
	} while (n == RECLAIM_BATCH);
}

/* The snapshot is put on the list and the ones that nobody can read are taken off it in one hold of the retire_lock */
void
sift_shm_snap_retire(Table *t, dsa_area *a, dsa_pointer dp)
{
	SiftShmManifestSnap *snap = dsa_get_address(a, dp);
	dsa_pointer batch[RECLAIM_BATCH];
	int			n;

	pg_atomic_write_u32(&snap->retired, 1);
	pg_memory_barrier();
	LWLockAcquire(&t->retire_lock, LW_EXCLUSIVE);
	snap->retire_epoch = pg_atomic_fetch_add_u64(&t->global_epoch, 1);
	snap->retire_next = t->retired_head;
	t->retired_head = dp;
	pg_atomic_sub_fetch_u32(&snap->refcount, 1);
	n = reclaim_unlink(t, a, false, batch);
	LWLockRelease(&t->retire_lock);
	reclaim_free(a, batch, n);
	if (n == RECLAIM_BATCH)
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

void
sift_shm_meta_write(SiftShmIndexSlot *s, const SiftShmIndexMeta *meta)
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
			dsa_pointer dp = sift_shm_slot_swap_snap(s, InvalidDsaPointer);

			if (dp != InvalidDsaPointer)
			{
				SiftShmIndexMeta meta = s->meta;

				meta.state_flags &= ~SIFT_SHM_SLOT_HAS_SNAP;
				LWLockAcquire(&s->build_lock, LW_EXCLUSIVE);
				sift_shm_meta_write(s, &meta);
				LWLockRelease(&s->build_lock);
				sift_shm_snap_retire(t, area, dp);
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
dsa_pointer
sift_shm_snap_build(Table *t, dsa_area *a, const RelFileLocator *locator, const SiftShmSnapSpec *spec)
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
void
sift_shm_snap_free(dsa_area *a, SiftShmManifestSnap *snap)
{
	Table	   *t = table;
	pg_atomic_uint64 *lists = dsa_get_address(a, snap->part_lists_dp);

	for (uint32 p = 0; lists != NULL && p < snap->npartitions; p++)
	{
		dsa_pointer lp = pg_atomic_read_u64(&lists[p]);

		if (lp != InvalidDsaPointer && lp != SIFT_LIST_INSTALLING)
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
		SiftShmIndexSlot *s = sift_shm_slot_at(t, loc, i);
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

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
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

/* The pins that found a snapshot are counted by the backend and added to the slot in runs of HITS_EVERY: a pin writes no line that the readers of the slot share */
#define HITS_EVERY		64

static struct
{
	SiftShmIndexSlot *slot;
	uint32		n;
} pending_hits;

static void
hits_flush(void)
{
	if (pending_hits.n > 0)
		pg_atomic_fetch_add_u64(&pending_hits.slot->snap_hits, pending_hits.n);
	pending_hits.n = 0;
}

static void
hits_note(SiftShmIndexSlot *s)
{
	if (pending_hits.slot != s)
	{
		hits_flush();
		pending_hits.slot = s;
	}
	if (++pending_hits.n == HITS_EVERY)
		hits_flush();
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

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return NULL;
	s = slot_lookup(t, looked_up, &meta);
	if (s == NULL)
		return NULL;
	if (!(meta.state_flags & SIFT_SHM_SLOT_HAS_SNAP) || (a = sift_shm_area_attach(t, false)) == NULL)
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
	hits_note(s);
	return snap;
}

bool
sift_shm_manifest_pin_stat(const RelFileLocator *locator, uint64 *hits, uint64 *misses)
{
	Table	   *t;
	SiftShmIndexMeta meta;
	SiftShmIndexSlot *s;

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL || (s = slot_lookup(t, locator, &meta)) == NULL)
		return false;
	*hits = pg_atomic_read_u64(&s->snap_hits) + (pending_hits.slot == s ? pending_hits.n : 0);
	*misses = pg_atomic_read_u64(&s->snap_misses);
	return true;
}

const SiftShmManifestSnap *
sift_shm_snap_pin(const RelFileLocator *locator)
{
	return pin_found_for(locator, locator);
}

static bool
run_on_snapshot(Table *t, SiftShmIndexSlot *s, dsa_area *a, const RelFileLocator *locator, uint64 version, SiftShmSnapFn fn, void *arg)
{
	pg_atomic_uint64 *mine = &t->epoch[MyProcNumber].epoch;
	bool		ran = false;

	epoch_register(t);
	pg_atomic_write_u64(mine, pg_atomic_read_u64(&t->global_epoch) | SIFT_EPOCH_ACTIVE_BIT);
	pg_memory_barrier();
	PG_TRY();
	{
		dsa_pointer dp = pg_atomic_read_u64(&s->current_snap_dp);
		const SiftShmManifestSnap *snap = dp == InvalidDsaPointer ? NULL : dsa_get_address(a, dp);

		if (snap != NULL && snap->manifest_version == version && RelFileLocatorEquals(snap->locator, *locator))
		{
			fn(snap, arg);
			ran = true;
		}
	}
	PG_FINALLY();
	{
		pg_write_barrier();
		pg_atomic_write_u64(mine, 0);
	}
	PG_END_TRY();
	return ran;
}

static bool
run_on_pinned(SiftShmIndexSlot *s, dsa_area *a, const RelFileLocator *locator, uint64 version, SiftShmSnapFn fn, void *arg)
{
	SiftShmManifestSnap *snap = pin_locked(s, a);
	bool		ran = snap != NULL && snap->manifest_version == version && RelFileLocatorEquals(snap->locator, *locator);

	if (ran)
		fn(snap, arg);
	if (snap != NULL)
		snap_decref(a, snap->self_dp);
	return ran;
}

bool
sift_shm_snap_with_epoch(const RelFileLocator *locator, uint64 version, SiftShmSnapFn fn, void *arg)
{
	Table	   *t;
	SiftShmIndexMeta meta;
	SiftShmIndexSlot *s;
	dsa_area   *a;
	bool		ran;

	if (!sift_shm_manifest_enabled() || (t = sift_shm_registry()) == NULL)
		return false;
	s = slot_lookup(t, locator, &meta);
	if (s == NULL)
		return false;
	if (!(meta.state_flags & SIFT_SHM_SLOT_HAS_SNAP) || (a = sift_shm_area_attach(t, false)) == NULL)
	{
		(void) pin_missed(s);
		return false;
	}
	ran = (uint32) MyProcNumber < SIFT_SHM_EPOCH_SLOTS ? run_on_snapshot(t, s, a, locator, version, fn, arg) : run_on_pinned(s, a, locator, version, fn, arg);
	if (ran)
		hits_note(s);
	else
		(void) pin_missed(s);
	return ran;
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
	Table	   *t = sift_shm_registry();

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

	if (lp == InvalidDsaPointer || lp == SIFT_LIST_INSTALLING)
		return NULL;
	pg_read_barrier();
	return list_payload(lp);
}

/* The list in a block of the area of its own, published in the slot once it is whole; NULL where the area has no room */
static dsa_pointer
list_block(const void *data, Size bytes, Size total)
{
	dsa_pointer lp;

#ifdef SIFT_TESTING
	if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "part_list_install") != NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift test: the list of a partition cannot be put in the area (part_list_install)")));	/* check-errors: allow */
#endif
	lp = dsa_allocate_extended(area, total, DSA_ALLOC_NO_OOM);
	if (lp != InvalidDsaPointer)
	{
		((ListHdr *) dsa_get_address(area, lp))->bytes = total;
		memcpy(list_payload(lp), data, bytes);
	}
	return lp;
}

/* The slot is SIFT_LIST_INSTALLING while a backend makes the list: one that fails, or finds no room, gives the slot and the bytes it took back, so that the next reader makes the list */
static bool
list_give_up(pg_atomic_uint64 *slot, Size reserved)
{
	if (reserved > 0)
		sift_shm_release_shared(SIFT_SHM_MANIFEST, reserved);
	pg_atomic_write_u64(slot, InvalidDsaPointer);
	return false;
}

bool
sift_shm_snap_part_list_install(const SiftShmManifestSnap *snapshot, uint32 p, const void *data, Size bytes)
{
	SiftShmManifestSnap *snap = (SiftShmManifestSnap *) snapshot;
	pg_atomic_uint64 *slot = part_list_slot(snap, p);
	Size		total = sizeof(ListHdr) + bytes;
	dsa_pointer none = InvalidDsaPointer,
				mine;

	if (pg_atomic_read_u64(slot) != InvalidDsaPointer || !pg_atomic_compare_exchange_u64(slot, &none, SIFT_LIST_INSTALLING))
		return true;
	if (!sift_shm_reserve_shared(SIFT_SHM_MANIFEST, total))
		return list_give_up(slot, 0);
	PG_TRY();
	{
		mine = list_block(data, bytes, total);
	}
	PG_CATCH();
	{
		(void) list_give_up(slot, total);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (mine == InvalidDsaPointer)
		return list_give_up(slot, total);
	pg_atomic_fetch_add_u64(&table->list_copies, 1);
	pg_write_barrier();
	pg_atomic_write_u64(slot, mine);
	pg_atomic_fetch_add_u64(&snap->snap_bytes, total);
	pg_atomic_fetch_add_u64(&table->snap_bytes, total);
	return true;
}

void
sift_shm_manifest_stats(SiftShmManifestStats *stats)
{
	Table	   *t = sift_shm_registry();
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
	stats->list_copies = pg_atomic_read_u64(&t->list_copies);
	for (int i = 0; i < SIFT_SHM_EPOCH_SLOTS; i++)
		stats->epochs_active += pg_atomic_read_u64(&t->epoch[i].epoch) != 0;
	LWLockAcquire(&t->area_lock, LW_SHARED);
	stats->area_users = t->area_users;
	LWLockRelease(&t->area_lock);
	stats->area_bytes = area == NULL ? 0 : dsa_get_total_size(area);
}
