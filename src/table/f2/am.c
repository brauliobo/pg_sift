/*
 * table/f2/am.c - the relation of a sift_table as the table access method asks for it: its slots, its files, TRUNCATE, copies, its size, and the scans that build indexes.
 *
 * A row is a virtual tuple: the scan fills its values from a granule, so the slot is the plain virtual one. The layout of Format 2.0 (block 0, the empty partition directory, the
 * allocator) is written when the file is made, with the file's own locator; a table has no TOAST table, and its visibility map is indexed by the virtual block of a row (table/f2/vm/vm.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlogutils.h"
#include "catalog/index.h"
#include "catalog/pg_operator.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "core/cmp.h"
#include "core/memtrack.h"
#include "core/shm/manifest.h"
#include "core/compat.h"
#include "core/part_lock.h"
#include "col/f2/build/build.h"
#include "col/f2/cache.h"
#include "col/f2/l0/shard_dir.h"
#include "table/f2/col_delta/io.h"
#include "table/f2/gcache.h"
#include "table/f2/row/state.h"
#include "table/f2/slot.h"
#include "table/f2/tbl.h"
#include "table/f2/vm/vm.h"
#include "table/dispatch.h"

/* The rows of an index build between two reports of its progress: a report is a write to the shared counters, which a parallel build pays for every row of every worker */
#define SIFT_TBL_PROGRESS_ROWS	4096

const TupleTableSlotOps *
sift_tbl_f2_slot_callbacks(Relation rel pg_attribute_unused())
{
	return sift_f2_slot_ops();
}

/* ---- files ---- */

#ifdef SIFT_TESTING
bool		sift_test_headered_strings = false;
#define NEW_TABLE_COMPAT()	(sift_tbl_host_ops.base_compat_flags & ~(sift_test_headered_strings ? SIFT_COMPAT_NATIVE_STRINGS : 0))
#else
#define NEW_TABLE_COMPAT()	(sift_tbl_host_ops.base_compat_flags)
#endif

static void
init_pages(Relation rel)
{
	sift_col_meta_init(rel, SIFT_AM_TABLE, NEW_TABLE_COMPAT(), 0, 0);
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
		sift_col_meta_init_fork(rel, SIFT_AM_TABLE, NEW_TABLE_COMPAT(), 0, 0);
}

void
sift_tbl_f2_init_layout(Relation rel)
{
	init_pages(rel);
}

void
sift_tbl_f2_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (BLCKSZ != 8192)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_table needs BLCKSZ 8192"), errdetail("This server has BLCKSZ %d.", BLCKSZ),
						errhint("Use a PostgreSQL build with the default 8 kB block size.")));
	sift_tbl_f2_guard_publication(rel);
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	srel = RelationCreateStorage(*newrlocator, persistence, true);
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}
	smgrclose(srel);
	sift_stage_flush_with_deletes(rel);
	sift_stage_discard(rel);
	sift_fsm_cache_discard_rel(rel);
	sift_shm_manifest_invalidate(&rel->rd_locator, false);
	if (persistence != RELPERSISTENCE_TEMP)
	{
		Relation	fake = CreateFakeRelcacheEntry(*newrlocator);

		fake->rd_rel->relpersistence = persistence;
		fake->rd_createSubid = GetCurrentSubTransactionId();
		init_pages(fake);
		FreeFakeRelcacheEntry(fake);
	}
}

static void
forget_file(Relation rel)
{
	sift_f2_forget_file(rel);
	sift_f2_gcache_forget(&rel->rd_locator);
	sift_col_delta_foreign_forget(&rel->rd_locator);
}

void
sift_tbl_f2_relation_nontransactional_truncate(Relation rel)
{
	forget_file(rel);
	sift_tbl_rowlock_drop_rel(&rel->rd_locator);
	RelationTruncate(rel, 0);
	init_pages(rel);
	sift_tbl_f2_tid_reset(sift_f2_store_open(rel));
	sift_tbl_vm_lease_release(rel);
	CacheInvalidateRelcache(rel);
}

/* What the transaction staged is written to the old file, each row and delete with the xid of its (sub)transaction, and the copy takes it: a rollback to a savepoint gives back the old file as the
 * transaction left it, with the row locks that name it */
void
sift_tbl_f2_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel = smgropen(*newrlocator, rel->rd_backend);

	sift_stage_flush_with_deletes(rel);
	forget_file(rel);
	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);
	FlushRelationBuffers(rel);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM, rel->rd_rel->relpersistence);
	for (ForkNumber fork = MAIN_FORKNUM + 1; fork <= MAX_FORKNUM; fork++)
		if (smgrexists(RelationGetSmgr(rel), fork))
		{
			smgrcreate(dstrel, fork, false);
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT || (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED && fork == INIT_FORKNUM))
				log_smgrcreate(newrlocator, fork);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, fork, rel->rd_rel->relpersistence);
		}
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

uint64
sift_tbl_f2_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

bool
sift_tbl_f2_relation_needs_toast_table(Relation rel pg_attribute_unused())
{
	return false;
}

/* ---- the planner ---- */

void
sift_tbl_f2_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
	SiftShmIndexMeta peek;
	BlockNumber unused,
				also_unused;
	uint64		total = 0,
				dead = 0;
	uint64		rows = 0;

	/* IndexOnlyNext asks the map fork by the vblock of an address: the fraction is that of the vblocks VACUUM found all visible, and 0 where the server cannot log the map */
	sift_tbl_vm_fractions(rel, 0, &unused, &also_unused, allvisfrac);
	if (RelationGetNumberOfBlocks(rel) > 0)
	{
		if (sift_shm_manifest_peek(&rel->rd_locator, &peek))
		{
			total = peek.total_rows;
			dead = peek.total_dead;
		}
		else
		{
			SiftColMetaPageData meta = sift_col_meta_read(rel);

			total = meta.total_rows;
			dead = meta.total_dead;
		}
		rows = total - dead;
		/* a table whose parts count no row has its rows in the slabs of its partitions, which the rows ANALYZE counted stand for; one whose part rows are all dead is empty */
		if (total == 0 && rel->rd_rel->reltuples > 0)
			rows = (uint64) rel->rd_rel->reltuples;
	}
	(void) get_rel_data_width(rel, attr_widths);
	*tuples = (double) rows;
	*pages = rows == 0 ? 0 : Max(1, RelationGetNumberOfBlocks(rel));
}

/* ---- rewrites ---- */

/*
 * VACUUM FULL and CLUSTER keep every version of a row that a snapshot still open may read, as the heap does. The table is locked against every writer, so each writer of the old table has
 * committed or aborted: a version is gone when its deleter is older than the horizon (every snapshot sees the delete), and its inserter is kept unless it is older than the horizon (then it is
 * frozen); CLUSTER in a transaction block keeps the versions the transaction deleted or replaced, with it as their deleter. A column delta newer than the horizon is a version of its own: the
 * one before it ends where the delta begins, read with a snapshot that does not see the delta.
 */
typedef struct SiftTblDeadVersion
{
	uint8		key[SIFT_BOUND_BYTES];	/* routes the delete to the partition of the row */
	SiftPendDelRec rec;
} SiftTblDeadVersion;

typedef struct SiftTblRewrite
{
	Relation	old;
	TransactionId oldest;
	SiftTblF2Bulk bulk;
	TupleTableSlot *before;		/* a version that a delta replaced */
	TupleTableSlot *horizon;	/* the version every snapshot sees */
	TransactionId *hide;		/* the writers the snapshot of the version before does not see */
	int			nhide;
	int			caphide;
	SiftTblDeadVersion *dead;	/* the versions of the new table and who ended them, written to it when maxdead are held */
	int			ndead;
	int			capdead;
	int			maxdead;
	double		live;
	double		vacuumed;
	double		recently_dead;
} SiftTblRewrite;

/* Every snapshot sees what committed before the horizon */
static bool
seen_by_all(const SiftTblRewrite *r, TransactionId xid)
{
	return !TransactionIdIsNormal(xid) || TransactionIdPrecedes(xid, r->oldest);
}

static void
hide_writer(SiftTblRewrite *r, TransactionId xid)
{
	if (r->nhide == r->caphide)
	{
		r->caphide = Min(Max(r->caphide * 2, 8), (int) (MaxAllocSize / sizeof(TransactionId)));
		r->hide = r->hide != NULL ? repalloc(r->hide, sizeof(TransactionId) * r->caphide) : palloc(sizeof(TransactionId) * r->caphide);
	}
	r->hide[r->nhide++] = xid;
}

/* The snapshot that sees every transaction that committed but the writers hidden, and nothing of this transaction (command 0): the version of the row before the oldest of them */
static Snapshot
snapshot_before(const SiftTblRewrite *r)
{
	Snapshot	snap = palloc0(sizeof(SnapshotData) + sizeof(TransactionId) * r->nhide);

	snap->snapshot_type = SNAPSHOT_MVCC;
	snap->xmin = r->oldest;
	snap->xmax = ReadNextTransactionId();
	snap->xip = (TransactionId *) (snap + 1);
	memcpy(snap->xip, r->hide, sizeof(TransactionId) * r->nhide);
	snap->xcnt = (uint32) r->nhide;
	snap->curcid = FirstCommandId;
	snap->copied = true;
	return snap;
}

static int
dead_cmp(const void *a, const void *b)
{
	return memcmp(((const SiftTblDeadVersion *) a)->key, ((const SiftTblDeadVersion *) b)->key, SIFT_BOUND_BYTES);
}

/* The smallest key above key: the upper bound of a read that ends with it */
static void
key_after(const uint8 *key, uint8 *after)
{
	int			i = SIFT_BOUND_BYTES - 1;

	memcpy(after, key, SIFT_BOUND_BYTES);
	while (i >= 0 && ++after[i] == 0)
		i--;
	SIFT_INVARIANT(i >= 0, "no key follows the largest one");
}

/*
 * The deletes of the versions held, written to the partitions of the new table that hold them, under the lock of each: the rows are written first, so that the deletes find them. The
 * manifest is read for the keys of the deletes only, sorted, one run of them for each partition.
 */
static void
write_deletes(SiftTblRewrite *r)
{
	Relation	rel = r->bulk.rel;
	SiftPendDelRec *recs = palloc(sizeof(SiftPendDelRec) * r->ndead);
	uint8		hi[SIFT_BOUND_BYTES];
	SiftManifestRead mr;

	sift_stage_flush(rel);
	qsort(r->dead, r->ndead, sizeof(SiftTblDeadVersion), dead_cmp);
	key_after(r->dead[r->ndead - 1].key, hi);
	mr = sift_manifest_read_range(rel, r->dead[0].key, hi);
	for (int i = 0, j, m = 0; i < r->ndead; i = j)
	{
		SiftPartitionEntry e;
		bool		locked;

		CHECK_FOR_INTERRUPTS();
		while (m + 1 < mr.n && memcmp(mr.entries[m + 1].key_lo, r->dead[i].key, SIFT_BOUND_BYTES) <= 0)
			m++;
		for (j = i; j < r->ndead && (m + 1 >= mr.n || memcmp(r->dead[j].key, mr.entries[m + 1].key_lo, SIFT_BOUND_BYTES) < 0); j++)
			recs[j - i] = r->dead[j].rec;
		locked = sift_l0_lock_range(rel, mr.entries[m].part_id, mr.entries[m].key_lo, m + 1 < mr.n ? mr.entries[m + 1].key_lo : NULL, &e);
		SIFT_INVARIANT(locked, "partition " UINT64_FORMAT " of the new table \"%s\" changed during its rewrite", mr.entries[m].part_id, RelationGetRelationName(rel));
		sift_del_route(rel, e.part_id, e.key_lo, recs, j - i, &r->bulk.store->spec);
		sift_part_page_unlock(rel, e.part_id, ExclusiveLock);
	}
	r->recently_dead += r->ndead;
	r->ndead = 0;
	pfree(recs);
	pfree(mr.entries);
}

/* The versions a rewrite holds before it writes their deletes: a quarter of the maintenance budget, in one allocation */
static int
dead_versions_max(void)
{
	return (int) Max(Min(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 4), MaxAllocSize) / sizeof(SiftTblDeadVersion), 64);
}

/* The command of a write of this transaction, which its later snapshots tell apart; 0 for any other's */
static CommandId
own_cid(TransactionId xid, CommandId cid)
{
	return TransactionIdIsCurrentTransactionId(xid) ? cid : 0;
}

static void
put_version(SiftTblRewrite *r, TupleTableSlot *slot, SiftRowState ins, TransactionId xmax, CommandId cmax)
{
	ItemPointerData tid;
	uint8		key[SIFT_BOUND_BYTES];
	bool		frozen = seen_by_all(r, ins.xmin);

	sift_tbl_f2_bulk_add(&r->bulk, slot, frozen ? FrozenTransactionId : ins.xmin, frozen ? 0 : own_cid(ins.xmin, ins.cmin), &tid, key);
	if (!TransactionIdIsValid(xmax))
	{
		r->live++;
		return;
	}
	if (r->ndead == r->maxdead)
		write_deletes(r);
	if (r->ndead == r->capdead)
	{
		r->capdead = Min(Max(r->capdead * 2, 64), r->maxdead);
		r->dead = r->dead != NULL ? repalloc(r->dead, sizeof(SiftTblDeadVersion) * r->capdead) : palloc(sizeof(SiftTblDeadVersion) * r->capdead);
	}
	memcpy(r->dead[r->ndead].key, key, SIFT_BOUND_BYTES);
	r->dead[r->ndead++].rec = (SiftPendDelRec) {.tid = sift_tbl_tid_pack(&tid),.xmax = xmax,.cmax = own_cid(xmax, cmax)};
}

static bool
same_values(TupleTableSlot *a, TupleTableSlot *b)
{
	TupleDesc	desc = a->tts_tupleDescriptor;

	slot_getallattrs(a);
	slot_getallattrs(b);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (a->tts_isnull[i] != b->tts_isnull[i] ||
			(!a->tts_isnull[i] && !datum_image_eq(a->tts_values[i], b->tts_values[i], att->attbyval, att->attlen)))
			return false;
	}
	return true;
}

/*
 * The inserter of the version before a delta of this transaction, which hides none of its own writes from a reader of the row: the version every snapshot sees, as no other delta newer than
 * the horizon changed it. A row that such a delta changed too keeps versions this transaction cannot tell apart, and the rewrite is refused.
 */
static SiftRowState
before_own_delta(SiftTblRewrite *r, ItemPointer at, const SiftRowState *rs)
{
	SiftTblRewrite all = {.oldest = r->oldest};
	Snapshot	snap = snapshot_before(&all);
	bool		found;

	snap->xmax = r->oldest;
	found = table_tuple_fetch_row_version(r->old, at, snap, r->horizon);
	pfree(snap);
	if (!found || !same_values(r->before, r->horizon))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot rewrite \"%s\" in this transaction: it updated a row that another transaction changed after a snapshot that is still open", RelationGetRelationName(r->old)),
						errhint("Run CLUSTER outside the transaction that changed the rows, or once the older snapshots have ended.")));
	return *rs;
}

/* The row the scan read, as the newest delta that committed made it, and the versions before it that a snapshot may still read */
static void
rewrite_row(SiftTblRewrite *r, TupleTableSlot *slot)
{
	ItemPointerData at = slot->tts_tid;
	uint64		tid = sift_tbl_tid_pack(&at);
	SiftRowState rs = sift_row_state_read(r->old, tid);
	TransactionId xmax = TransactionIdIsValid(rs.xmax) && (TransactionIdIsCurrentTransactionId(rs.xmax) || TransactionIdDidCommit(rs.xmax)) ? rs.xmax : InvalidTransactionId;
	SiftRowState ins;

	CHECK_FOR_INTERRUPTS();
	if (!rs.found || (TransactionIdIsValid(xmax) && seen_by_all(r, xmax)))
	{
		r->vacuumed++;
		return;
	}
	ins = sift_row_sysattr_read(r->old, tid, SnapshotAny);
	put_version(r, slot, ins, xmax, rs.cmax);
	r->nhide = 0;
	if (TransactionIdIsValid(xmax))
		hide_writer(r, xmax);
	while (ins.xmin != rs.xmin && !seen_by_all(r, ins.xmin))
	{
		SiftRowState replaced = ins;
		Snapshot	snap;
		bool		found;

		hide_writer(r, replaced.xmin);
		snap = snapshot_before(r);
		found = table_tuple_fetch_row_version(r->old, &at, snap, r->before);
		SIFT_INVARIANT(found, "the version of (%u,%u) of \"%s\" before its delta of xid %u is gone",
					   ItemPointerGetBlockNumber(&at), ItemPointerGetOffsetNumber(&at), RelationGetRelationName(r->old), replaced.xmin);
		ins = TransactionIdIsCurrentTransactionId(replaced.xmin) ? before_own_delta(r, &at, &rs) : sift_row_sysattr_read(r->old, tid, snap);
		put_version(r, r->before, ins, replaced.xmin, replaced.cmin);
		pfree(snap);
	}
}

/* The versions of a Format 2 table, each row as its newest delta that committed made it, with the versions before it that a snapshot may still read */
static void
copy_versions(SiftTblRewrite *r)
{
	TableScanDesc scan = sift_table_beginscan(r->old, SnapshotAny, 0, NULL);
	TupleTableSlot *slot = table_slot_create(r->old, NULL);

	r->before = table_slot_create(r->old, NULL);
	r->horizon = table_slot_create(r->old, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		rewrite_row(r, slot);
	ExecDropSingleTupleTableSlot(r->before);
	ExecDropSingleTupleTableSlot(r->horizon);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/*
 * The versions of a table of any format that a snapshot may still read (SnapshotNonVacuumable), each with the inserter and the deleter its slot reads. The latest version a reader of this
 * transaction sees has no deleter: the last tombstone of a row may be a lock or an aborted delete.
 */
static void
copy_readable(SiftTblRewrite *r)
{
	SnapshotData readable;
	TableScanDesc scan;
	TupleTableSlot *slot = table_slot_create(r->old, NULL);
	bool		isnull;

	InitNonVacuumableSnapshot(readable, GlobalVisTestFor(r->old));
	scan = sift_table_beginscan(r->old, &readable, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		SiftRowState ins = {.xmin = DatumGetTransactionId(slot_getsysattr(slot, MinTransactionIdAttributeNumber, &isnull)),
		.cmin = DatumGetCommandId(slot_getsysattr(slot, MinCommandIdAttributeNumber, &isnull))};
		bool		deleted = !table_tuple_satisfies_snapshot(r->old, slot, SnapshotSelf);

		CHECK_FOR_INTERRUPTS();
		put_version(r, slot, ins, deleted ? DatumGetTransactionId(slot_getsysattr(slot, MaxTransactionIdAttributeNumber, &isnull)) : InvalidTransactionId,
					deleted ? DatumGetCommandId(slot_getsysattr(slot, MaxCommandIdAttributeNumber, &isnull)) : 0);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

/* The rows of OldTable that copy reads, written to NewTable in the layout of OldTable, with the deletes of the versions that ended */
static SiftTblRewrite
rewrite(Relation OldTable, Relation NewTable, TransactionId OldestXmin, void (*copy) (SiftTblRewrite *r))
{
	SiftTblRewrite r = {.old = OldTable,.oldest = OldestXmin,.maxdead = dead_versions_max()};

	sift_tbl_f2_adopt_layout(NewTable, sift_f2_store_open(NewTable), sift_f2_store_open(OldTable));
	sift_tbl_f2_bulk_begin(&r.bulk, NewTable);
	copy(&r);
	if (r.ndead > 0)
		write_deletes(&r);
	sift_tbl_f2_bulk_end(&r.bulk);
	if (r.dead != NULL)
		pfree(r.dead);
	if (r.hide != NULL)
		pfree(r.hide);
	return r;
}

/* sift.upgrade_table: the rows of a table of any format, as the rewrite of CLUSTER keeps them */
double
sift_tbl_f2_rewrite_rows(Relation OldTable, Relation NewTable, TransactionId OldestXmin)
{
	SiftTblRewrite r = rewrite(OldTable, NewTable, OldestXmin, copy_readable);

	return r.live + r.recently_dead;
}

/* A Format 1 table has no column deltas to walk: its versions are the ones its slots read */
void
sift_tbl_f2_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex pg_attribute_unused(), bool use_sort pg_attribute_unused(), TransactionId OldestXmin,
									  SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	SiftTblRewrite r = rewrite(OldTable, NewTable, OldestXmin, sift_tbl_ops(OldTable)->format_major == SIFT_FORMAT2_MAJOR ? copy_versions : copy_readable);

	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
	*num_tuples = r.live + r.recently_dead;
	*tups_vacuumed = r.vacuumed;
	*tups_recently_dead = r.recently_dead;
}

/* ---- index builds ---- */

double
sift_tbl_f2_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, bool allow_sync, bool anyvisible pg_attribute_unused(), bool progress,
								   BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	ExprState  *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);
	Snapshot	snapshot = InvalidSnapshot;
	bool		own_scan = scan == NULL;
	double		reltuples = 0;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	if (own_scan)
	{
		snapshot = RegisterSnapshot(index_info->ii_Concurrent ? GetTransactionSnapshot() : GetLatestSnapshot());
		SIFT_POINT("sift-tbl-index-build-snapshot");
		scan = table_beginscan_strat(table_rel, snapshot, 0, NULL, true, allow_sync);
		if (start_blockno != 0 || numblocks != InvalidBlockNumber)
		{
			ItemPointerData lo,
						hi;

			ItemPointerSet(&lo, start_blockno, FirstOffsetNumber);
			ItemPointerSet(&hi, numblocks == InvalidBlockNumber ? MaxBlockNumber : start_blockno + numblocks - 1, MaxOffsetNumber);
			sift_tbl_f2_scan_set_tidrange(scan, &lo, &hi);
		}
	}
	else if (scan->rs_snapshot->snapshot_type == SNAPSHOT_ANY)
		/* a parallel build that holds off the writers hands in SnapshotAny, which sees the versions a commit deleted: the index gets the rows the serial build reads. A worker cannot take the latest snapshot */
		sift_tbl_f2_scan_set_snapshot(scan, SnapshotSelf);
	/* a row deleted after the snapshot is not in this index, which a snapshot older than the build must not rely on (indcheckxmin) */
	index_info->ii_BrokenHotChain = true;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		reltuples++;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		callback(index_rel, &slot->tts_tid, values, isnull, true, callback_state);
		if (progress && ((uint64) reltuples & (SIFT_TBL_PROGRESS_ROWS - 1)) == 0)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) reltuples);
	}
	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) reltuples);
	table_endscan(scan);
	if (own_scan)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
	return reltuples;
}

/*
 * The visible rows of the table by windows of virtual blocks (a window holds at most the addresses a half of the maintenance budget holds, one allocation of them and the blocks
 * of the table, as every block holds a bounded number of rows), each sorted by address and merged with the addresses the index has, which come in the order of the sort of PostgreSQL: one pass of N log N with no temp file where a scan that probes the index for each row is N squared.
 * A row that does not satisfy the predicate of the index is not in the window: the index never held it.
 */
void
sift_tbl_f2_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, Snapshot snapshot, struct ValidateIndexState *state)
{
	const uint64 nvblocks = sift_tbl_f2_nvblocks(table_rel);
	const uint64 budget = Min(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2), MaxAllocSize) / sizeof(int64);
	const uint64 max_addrs = Max(Min(budget, nvblocks * SIFT_TBL_ROWS_PER_VBLOCK), SIFT_TBL_ROWS_PER_VBLOCK);
	const uint64 step = max_addrs / SIFT_TBL_ROWS_PER_VBLOCK;
	int64	   *window = palloc(max_addrs * sizeof(int64));
	TableScanDesc scan = sift_table_beginscan(table_rel, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState  *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);
	Datum		idx_val = 0;
	bool		idx_null,
				have_idx = tuplesort_getdatum(state->tuplesort, true, false, &idx_val, &idx_null, NULL);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	for (uint64 from = 0; from < nvblocks; from += step)
	{
		ItemPointerData lo,
					hi;
		uint64		n = 0;

		ItemPointerSet(&lo, (BlockNumber) from, FirstOffsetNumber);
		ItemPointerSet(&hi, (BlockNumber) Min(from + step - 1, nvblocks - 1), SIFT_TBL_ROWS_PER_VBLOCK);
		sift_tbl_f2_scan_set_tidrange(scan, &lo, &hi);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			CHECK_FOR_INTERRUPTS();
			state->htups += 1;
			MemoryContextReset(econtext->ecxt_per_tuple_memory);
			if (predicate == NULL || ExecQual(predicate, econtext))
				window[n++] = (int64) itemptr_encode(&slot->tts_tid);
		}
		qsort(window, n, sizeof(int64), sift_cmp_i64);
		for (uint64 i = 0; i < n; i++)
		{
			ItemPointerData tid;

			for (; have_idx && DatumGetInt64(idx_val) < window[i]; have_idx = tuplesort_getdatum(state->tuplesort, true, false, &idx_val, &idx_null, NULL))
				state->itups += 1;
			if (have_idx && DatumGetInt64(idx_val) == window[i])
				continue;
			itemptr_decode(&tid, window[i]);
			if (!table_tuple_fetch_row_version(table_rel, &tid, snapshot, slot))
				continue;
			MemoryContextReset(econtext->ecxt_per_tuple_memory);
			FormIndexDatum(index_info, slot, estate, values, isnull);
			SIFT_POINT("sift-tbl-validate-insert");
			index_insert(index_rel, values, isnull, &tid, table_rel, index_info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, index_info);
			state->tups_inserted += 1;
		}
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	pfree(window);
	sift_mem_release_os(max_addrs * sizeof(int64));
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
}
