/*
 * table/f2/vm/lease.c - the vblocks a backend may still put rows in (table/f2/vm/vm.h): the run it reserved and the vblock it fills now, published in the registry of shared memory
 * (core/shm/manifest.h) where VACUUM reads the floor below which a vblock is closed. A backend keeps one lease for each table it inserts into until it exits, but only a transaction that is
 * running holds one: at its end (commit, abort, prepare) the leases of the backend are idle, and a backend that finds the registry full, or VACUUM of the table, takes an idle lease away. A lease whose run
 * the backend can no longer fill (the run is used up, or the store that held it left the cache) is given back instead. The backend
 * that inserts again wakes its lease with its first row (sift_tbl_vm_lease_wake) and, where the lease was taken, drops the run it covered: the table takes a new run at its next row (no lock of the table is taken
 * for it, the pool of its addresses is in the memory of this backend). Where the registry has no slot even then, the backend gives back its least recently used lease, and where it has none to give back, or the registry
 * cannot be had, the insert goes on without a lease for the rest of the transaction: VACUUM sets no bit without a registry, and none of the tables of its bucket while a backend fills a run that has no slot (the registry counts them).
 * The leases are found by their file; a change of a relation (RELOID) marks its leases, and the next lease taken looks whether their files are gone. DISCARD ALL gives back the leases no transaction uses.
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
#include "lib/ilist.h"
#include "utils/hsearch.h"
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
	RelFileLocator locator;		/* the key */
	Oid			relid;
	uint32		relhash;		/* the RELOID hash value of relid */
	int			slot;			/* -1: the registry had none to give */
	uint64		seq;			/* the claim of the slot the lease holds */
	bool		idle;			/* the transaction that used it has ended (the slot may be taken away) */
	bool		counted;		/* the registry counts this run of vblocks as one with no slot */
	bool		stale;			/* its relation changed since: its file may be gone */
	uint64		hi;
	uint64		used;
	dlist_node	active;			/* in `active` while it is not idle */
} VmLease;

static MemoryContext leases_cx;
static HTAB *leases;
static dlist_head active = DLIST_STATIC_INIT(active);
static int	nstale;
static uint64 clock_used;
static uint64 xact_epoch = 1;
static uint64 woken_epoch;
static RelFileLocator woken;

int			sift_tbl_vm_idle_leases;

static int
nleases(void)
{
	return leases == NULL ? 0 : (int) hash_get_num_entries(leases);
}

static VmLease *
lease_find(const RelFileLocator *locator)
{
	return leases == NULL ? NULL : hash_search(leases, locator, HASH_FIND, NULL);
}

/* The slot of the lease, and the count of the runs that have none kept as it says */
static void
lease_slot(VmLease *l, int slot, uint64 seq)
{
	if (slot < 0 && !l->counted)
		l->counted = sift_shm_tid_unleased(&l->locator, 1);
	else if (slot >= 0 && l->counted)
	{
		(void) sift_shm_tid_unleased(&l->locator, -1);
		l->counted = false;
	}
	l->slot = slot;
	l->seq = seq;
}

static void
lease_set_idle(VmLease *l, bool idle)
{
	if (l->idle == idle)
		return;
	l->idle = idle;
	sift_tbl_vm_idle_leases += idle ? 1 : -1;
	if (idle)
		dlist_delete(&l->active);
	else
		dlist_push_tail(&active, &l->active);
}

static void
lease_drop(VmLease *l)
{
	if (l->slot >= 0)
		sift_shm_tid_lease_release(l->slot, l->seq);
	if (l->counted)
		(void) sift_shm_tid_unleased(&l->locator, -1);
	if (l->idle)
		sift_tbl_vm_idle_leases--;
	else
		dlist_delete(&l->active);
	nstale -= l->stale;
	(void) hash_search(leases, &l->locator, HASH_REMOVE, NULL);
}

static void
leases_release_all(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	HASH_SEQ_STATUS it;
	VmLease    *l;

	if (leases == NULL)
		return;
	hash_seq_init(&it, leases);
	while ((l = hash_seq_search(&it)) != NULL)
		lease_drop(l);
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

/* Whether the backend may still put a row in the run of the lease: the store that holds the run is in the cache and the run is not used up */
static bool
lease_run_left(const VmLease *l)
{
	SiftF2Store *store = sift_f2_store_cached(l->relid);

	return store != NULL && sift_tbl_extra(store)->pool.next_vblock < sift_tbl_extra(store)->pool.end_vblock;
}

/* The end of a transaction: the leases it used are idle, and the runs that have no slot, or that the backend cannot fill any more, are given up */
static void
leases_end_xact(XactEvent event, void *arg pg_attribute_unused())
{
	dlist_mutable_iter it;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	xact_epoch++;
	dlist_foreach_modify(it, &active)
	{
		VmLease    *l = dlist_container(VmLease, active, it.cur);

		if (l->slot < 0)
			lease_drop_run(l);
		else if (!lease_run_left(l))
			lease_drop(l);
		else
		{
			sift_shm_tid_lease_idle(l->slot);
			lease_set_idle(l, true);
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
	lease_set_idle(l, false);
	if (!sift_shm_tid_lease_wake(l->slot, l->seq))
	{
		sift_tbl_f2_tid_reset(store);
		lease_drop(l);
	}
}

/* A change of a relation marks its leases (no catalog is read in an invalidation): the next lease taken looks whether their files are gone */
static void
leases_invalidate(uint32 hashvalue)
{
	HASH_SEQ_STATUS it;
	VmLease    *l;

	if (leases == NULL)
		return;
	hash_seq_init(&it, leases);
	while ((l = hash_seq_search(&it)) != NULL)
		if (!l->stale && sift_f2_inval_hits(l->relhash, hashvalue))
		{
			l->stale = true;
			nstale++;
		}
}

/* A lease whose file is gone (TRUNCATE, a rewrite, DROP) holds its slot for nothing: the marked leases of this backend that name no file are let go when it takes a new one */
static void
leases_prune(void)
{
	HASH_SEQ_STATUS it;
	VmLease    *l;

	if (nstale == 0)
		return;
	hash_seq_init(&it, leases);
	while ((l = hash_seq_search(&it)) != NULL)
	{
		HeapTuple	tup;
		bool		gone;

		if (!l->stale)
			continue;
		l->stale = false;
		nstale--;
		tup = SearchSysCache1(RELOID, ObjectIdGetDatum(l->relid));
		gone = !HeapTupleIsValid(tup);
		if (!gone)
		{
			gone = ((Form_pg_class) GETSTRUCT(tup))->relfilenode != l->locator.relNumber;
			ReleaseSysCache(tup);
		}
		if (gone)
			lease_drop(l);
	}
}

/* The least recently used lease goes back with the run it protected: the table takes a new run (and a new lease) with its next row, so that no run is left without one */
static bool
lease_evict(bool resident)
{
	HASH_SEQ_STATUS it;
	VmLease    *l,
			   *victim = NULL;

	if (leases == NULL)
		return false;
	hash_seq_init(&it, leases);
	while ((l = hash_seq_search(&it)) != NULL)
		if ((!resident || l->slot >= 0) && (victim == NULL || l->used < victim->used))
			victim = l;
	if (victim == NULL)
		return false;
	lease_drop_run(victim);
	return true;
}

/* The leases no transaction uses are given back, and with none left so is their memory */
static void
leases_evict_all(void)
{
	HASH_SEQ_STATUS it;
	VmLease    *l;

	if (leases == NULL)
		return;
	hash_seq_init(&it, leases);
	while ((l = hash_seq_search(&it)) != NULL)
		if (l->idle)
			lease_drop_run(l);
	if (nleases() > 0)
		return;
	MemoryContextDelete(leases_cx);
	leases_cx = NULL;
	leases = NULL;
}

static uint64
leases_oldest(void)
{
	return UINT64_MAX;
}

static bool
leases_evict_one(void)
{
	return false;
}

static const SiftCacheKind leases_kind = {"table_f2_vm_leases", leases_oldest, leases_evict_one, leases_evict_all};

static void
leases_init(void)
{
	static bool registered;
	HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(VmLease)};

	leases_cx = sift_cache_create_context("sift cache vm leases");
	ctl.hcxt = leases_cx;
	leases = hash_create("sift cache vm leases", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	if (registered)
		return;
	before_shmem_exit(leases_release_all, 0);
	RegisterXactCallback(leases_end_xact, NULL);
	sift_f2_invalidator_register(leases_invalidate);
	sift_cache_register(&leases_kind);
	registered = true;
}

static VmLease *
lease_add(Relation rel)
{
	VmLease    *l;

	if (leases == NULL)
		leases_init();
	if (nleases() == SIFT_SHM_TID_LEASES)
		(void) lease_evict(false);
	l = hash_search(leases, &rel->rd_locator, HASH_ENTER, NULL);
	*l = (VmLease) {.locator = rel->rd_locator,.relid = RelationGetRelid(rel),.relhash = sift_f2_rel_hash(rel),.slot = -1};
	dlist_push_tail(&active, &l->active);
	return l;
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
