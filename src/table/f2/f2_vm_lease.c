/*
 * table/f2/f2_vm_lease.c - the vblocks a backend may still put rows in (table/f2/f2_vm.h): the run it reserved and the vblock it fills now, published in the registry of shared memory
 * (core/shm_manifest.h) where VACUUM reads the floor below which a vblock is closed. A backend keeps one lease for each table it inserts into and lets them all go when it exits.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "core/cachebudget.h"
#include "core/f2/f2_vm_xlog.h"
#include "core/shm_manifest.h"
#include "table/f2/f2_vm.h"

bool
sift_tbl_vm_active(Relation rel)
{
	return sift_vm_rmgr_active() && rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP && !RecoveryInProgress();
}

typedef struct VmLease
{
	RelFileLocator locator;
	Oid			relid;
	int			slot;
	uint64		hi;
} VmLease;

static VmLease *leases;
static int	nleases;
static int	capleases;

static VmLease *
lease_find(const RelFileLocator *locator)
{
	for (int i = 0; i < nleases; i++)
		if (RelFileLocatorEquals(leases[i].locator, *locator))
			return &leases[i];
	return NULL;
}

static void
lease_drop(VmLease *l)
{
	sift_shm_tid_lease_release(l->slot);
	*l = leases[--nleases];
}

static void
leases_release_all(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	while (nleases > 0)
		lease_drop(&leases[0]);
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

void
sift_tbl_vm_lease_open(Relation rel)
{
	int			slot;

	if (!sift_tbl_vm_active(rel) || lease_find(&rel->rd_locator) != NULL)
		return;
	if (leases == NULL)
	{
		leases = MemoryContextAlloc(sift_cache_create_context("sift cache vm leases"), sizeof(VmLease) * (capleases = 16));
		before_shmem_exit(leases_release_all, 0);
	}
	leases_prune();
	if ((slot = sift_shm_tid_lease_acquire(&rel->rd_locator)) < 0)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("could not reserve a vblock lease for sift_table \"%s\"", RelationGetRelationName(rel)),
						errdetail("The shared registry has no free lease (%d at most) or could not be mapped.", SIFT_SHM_TID_LEASES)));
	if (nleases == capleases)
		leases = repalloc(leases, sizeof(VmLease) * (capleases = Min(capleases * 2, SIFT_SHM_TID_LEASES)));
	leases[nleases++] = (VmLease) {.locator = rel->rd_locator,.relid = RelationGetRelid(rel),.slot = slot};
}

void
sift_tbl_vm_lease_set(Relation rel, uint64 lo, uint64 hi)
{
	VmLease    *l = lease_find(&rel->rd_locator);

	if (l == NULL)
		return;
	l->hi = hi;
	sift_shm_tid_lease_set(l->slot, lo, hi);
}

void
sift_tbl_vm_lease_roll(Relation rel, uint64 open_vblock)
{
	VmLease    *l = lease_find(&rel->rd_locator);

	if (l == NULL)
		return;
	if (open_vblock >= l->hi)
		lease_drop(l);
	else
		sift_shm_tid_lease_set(l->slot, open_vblock, l->hi);
}
