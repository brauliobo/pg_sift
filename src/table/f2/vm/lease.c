/*
 * table/f2/vm/lease.c - the vblocks a backend may still put rows in (table/f2/vm/vm.h): the run it reserved and the vblock it fills now, published in the registry of shared memory
 * (core/shm/manifest.h) where VACUUM reads the floor below which a vblock is closed. A backend keeps one lease for each table it inserts into until it exits, but only a transaction that is
 * running holds one: at its end (commit, abort, prepare) the leases of the backend are idle, and a backend that finds the registry full, or VACUUM of the table, takes an idle lease away. The backend
 * that inserts again wakes its lease with its first row (sift_tbl_vm_lease_wake) and, where the lease was taken, drops the run it covered: the table takes a new run at its next row (no lock of the table is taken
 * for it, the pool of its addresses is in the memory of this backend). Where the registry has no slot even then, the backend gives back its least recently used lease, and where it has none to give back, or the registry
 * cannot be had, the insert goes on without a lease for the rest of the transaction: VACUUM sets no bit without a registry, and none while a backend fills a run that has no slot (the registry counts them).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/f2/store/store.h"
#include "core/cachebudget.h"
#include "core/f2/vm_xlog.h"
#include "core/shm/manifest.h"
#include "table/f2/tbl.h"
#include "table/f2/vm/vm.h"

bool
sift_tbl_vm_active(Relation rel)
{
	return sift_vm_rmgr_active() && rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP && !RecoveryInProgress();
}

typedef struct VmLease
{
	RelFileLocator locator;
	Oid			relid;
	int			slot;			/* -1: the registry had none to give */
	uint64		seq;			/* the claim of the slot the lease holds */
	bool		idle;			/* the transaction that used it has ended (the slot may be taken away) */
	bool		counted;		/* the registry counts this run of vblocks as one with no slot */
	uint64		hi;
	uint64		used;
} VmLease;

static VmLease *leases;
static int	nleases;
static int	capleases;
static uint64 clock_used;
static uint64 xact_epoch = 1;
static uint64 woken_epoch;
static RelFileLocator woken;

int			sift_tbl_vm_idle_leases;

static VmLease *
lease_find(const RelFileLocator *locator)
{
	for (int i = 0; i < nleases; i++)
		if (RelFileLocatorEquals(leases[i].locator, *locator))
			return &leases[i];
	return NULL;
}

/* The slot of the lease, and the count of the runs that have none kept as it says */
static void
lease_slot(VmLease *l, int slot, uint64 seq)
{
	if (slot < 0 && !l->counted)
		l->counted = sift_shm_tid_unleased(1);
	else if (slot >= 0 && l->counted)
	{
		(void) sift_shm_tid_unleased(-1);
		l->counted = false;
	}
	l->slot = slot;
	l->seq = seq;
}

static void
lease_drop(VmLease *l)
{
	if (l->slot >= 0)
		sift_shm_tid_lease_release(l->slot, l->seq);
	if (l->counted)
		(void) sift_shm_tid_unleased(-1);
	sift_tbl_vm_idle_leases -= l->idle;
	*l = leases[--nleases];
}

static void
leases_release_all(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	while (nleases > 0)
		lease_drop(&leases[0]);
}

/* The run of the lease is given up with it: the table takes a new one at its next row */
static void
lease_drop_run(VmLease *l)
{
	SiftF2Store *store = sift_f2_store_cached(l->relid);

	if (store != NULL)
		sift_tbl_f2_tid_reset(store);
	lease_drop(l);
}

/* The end of a transaction: its leases are idle, and the runs that have no slot are given up */
static void
leases_end_xact(XactEvent event, void *arg pg_attribute_unused())
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	xact_epoch++;
	for (int i = nleases - 1; i >= 0 && nleases > sift_tbl_vm_idle_leases; i--)
	{
		VmLease    *l = &leases[i];

		if (l->slot < 0)
			lease_drop_run(l);
		else if (!l->idle)
		{
			sift_shm_tid_lease_idle(l->slot);
			l->idle = true;
			sift_tbl_vm_idle_leases++;
		}
	}
}

void
sift_tbl_vm_lease_wake(Relation rel, SiftF2Store *store)
{
	VmLease    *l;

	if (woken_epoch == xact_epoch && RelFileLocatorEquals(woken, rel->rd_locator))
		return;
	woken = rel->rd_locator;
	woken_epoch = xact_epoch;
	if ((l = lease_find(&rel->rd_locator)) == NULL || !l->idle)
		return;
	l->idle = false;
	sift_tbl_vm_idle_leases--;
	if (!sift_shm_tid_lease_wake(l->slot, l->seq))
	{
		sift_tbl_f2_tid_reset(store);
		lease_drop(l);
	}
}

/* A lease whose file is gone (TRUNCATE, a rewrite, DROP) holds its slot for nothing: the leases of this backend that name no file are let go when it takes a new one */
static void
leases_prune(void)
{
	for (int i = nleases - 1; i >= 0; i--)
	{
		HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(leases[i].relid));
		bool		gone = !HeapTupleIsValid(tup);

		if (!gone)
		{
			gone = ((Form_pg_class) GETSTRUCT(tup))->relfilenode != leases[i].locator.relNumber;
			ReleaseSysCache(tup);
		}
		if (gone)
			lease_drop(&leases[i]);
	}
}

/* The least recently used lease goes back with the run it protected: the table takes a new run (and a new lease) with its next row, so that no run is left without one */
static bool
lease_evict(bool resident)
{
	VmLease    *victim = NULL;

	for (int i = 0; i < nleases; i++)
		if ((!resident || leases[i].slot >= 0) && (victim == NULL || leases[i].used < victim->used))
			victim = &leases[i];
	if (victim == NULL)
		return false;
	lease_drop_run(victim);
	return true;
}

static VmLease *
lease_add(Relation rel)
{
	if (leases == NULL)
	{
		leases = MemoryContextAlloc(sift_cache_create_context("sift cache vm leases"), sizeof(VmLease) * (capleases = 16));
		before_shmem_exit(leases_release_all, 0);
		RegisterXactCallback(leases_end_xact, NULL);
	}
	if (nleases == capleases && capleases < SIFT_SHM_TID_LEASES)
		leases = repalloc(leases, sizeof(VmLease) * (capleases = Min(capleases * 2, SIFT_SHM_TID_LEASES)));
	else if (nleases == capleases)
		(void) lease_evict(false);
	leases[nleases] = (VmLease) {.locator = rel->rd_locator,.relid = RelationGetRelid(rel),.slot = -1};
	return &leases[nleases++];
}

void
sift_tbl_vm_lease_open(Relation rel)
{
	VmLease    *l;
	uint64		seq;
	int			slot;

	if (!sift_tbl_vm_active(rel) || ((l = lease_find(&rel->rd_locator)) != NULL && l->slot >= 0))
		return;
	leases_prune();
	while ((slot = sift_shm_tid_lease_acquire(&rel->rd_locator, &seq)) < 0 && lease_evict(true))
		;
	if ((l = lease_find(&rel->rd_locator)) == NULL)
		l = lease_add(rel);
	lease_slot(l, slot, seq);
	l->used = ++clock_used;
}

void
sift_tbl_vm_lease_set(Relation rel, uint64 lo, uint64 hi)
{
	VmLease    *l = lease_find(&rel->rd_locator);

	if (l == NULL)
		return;
	l->hi = hi;
	l->used = ++clock_used;
	if (l->slot >= 0)
		sift_shm_tid_lease_set(l->slot, lo);
}

void
sift_tbl_vm_lease_release(Relation rel)
{
	VmLease    *l = lease_find(&rel->rd_locator);

	if (l != NULL)
		lease_drop(l);
}

void
sift_tbl_vm_lease_roll(Relation rel, uint64 open_vblock)
{
	VmLease    *l = lease_find(&rel->rd_locator);

	if (l == NULL)
		return;
	if (open_vblock >= l->hi)
		lease_drop(l);
	else if (l->slot >= 0)
		sift_shm_tid_lease_set(l->slot, open_vblock);
}
