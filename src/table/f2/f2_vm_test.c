/*
 * table/f2/f2_vm_test.c - the test functions of the vblock counter of the metapage, the records of the visibility map and the vblock leases of a sift_table, compiled only with SIFT_TESTING.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "fmgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/xid8.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_store.h"
#include "core/f2/f2_vm_xlog.h"
#include "core/shm_manifest.h"
#include "table/f2/f2_tbl.h"
#include "table/f2/f2_vm.h"

PG_FUNCTION_INFO_V1(sift_test_tbl_set_next_vblock);
PG_FUNCTION_INFO_V1(sift_test_vm_horizon_xid);
PG_FUNCTION_INFO_V1(sift_test_vm_redo_set);
PG_FUNCTION_INFO_V1(sift_test_vm_lease_fill);
PG_FUNCTION_INFO_V1(sift_test_vm_lease_floor);

/* sift._test_tbl_set_next_vblock(rel regclass, next int8): the vblocks reserved so far, as the metapage says, and the pool of addresses of this backend gone */
Datum
sift_test_tbl_set_next_vblock(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessExclusiveLock);
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->next_vblock = PG_GETARG_INT64(1);
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
	sift_tbl_f2_tid_reset(sift_f2_store_open(rel));
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* sift._test_vm_horizon_xid(fx xid8, next xid8): the transaction id a record of the map names for a transaction of the table, 0 for none */
Datum
sift_test_vm_horizon_xid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_vm_horizon_xid(PG_GETARG_FULLTRANSACTIONID(0), PG_GETARG_FULLTRANSACTIONID(1)));
}

/* sift._test_vm_redo_set(record bytea): redo of a VM_SET record that holds only these bytes of data and no block */
Datum
sift_test_vm_redo_set(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	DecodedXLogRecord record = {.max_block_id = -1,.main_data = (char *) VARDATA_ANY(data),.main_data_len = VARSIZE_ANY_EXHDR(data)};
	XLogReaderState reader = {.record = &record};

	record.header.xl_info = SIFT_XLOG_VM_SET;
	sift_vm_redo_test(&reader);
	PG_RETURN_VOID();
}

static int	filled[SIFT_SHM_TID_LEASES];
static int	nfilled;

static void
lease_unfill(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	while (nfilled > 0)
		sift_shm_tid_lease_release(filled[--nfilled]);
}

/* sift._test_vm_lease_fill(n int): takes up to n slots of the registry of the leases for tables that are none, until this backend exits; the slots it got */
Datum
sift_test_vm_lease_fill(PG_FUNCTION_ARGS)
{
	static bool registered;
	int			n = PG_GETARG_INT32(0);
	int			slot;

	if (!registered)
	{
		before_shmem_exit(lease_unfill, 0);
		registered = true;
	}
	while (nfilled < n && (slot = sift_shm_tid_lease_acquire(&(RelFileLocator) {.spcOid = 1,.dbOid = 2,.relNumber = 4000000 + nfilled})) >= 0)
		filled[nfilled++] = slot;
	PG_RETURN_INT32(nfilled);
}

/* sift._test_vm_lease_floor(rel regclass, next int8): the vblock below which VACUUM may set bits of the table, with the leases as they stand; -1 without a registry */
Datum
sift_test_vm_lease_floor(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	uint64		floor = 0;
	bool		have = sift_shm_tid_lease_floor(&rel->rd_locator, PG_GETARG_INT64(1), &floor);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT64(have ? (int64) floor : -1);
}

#endif							/* SIFT_TESTING */
