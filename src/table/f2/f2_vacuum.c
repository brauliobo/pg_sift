/*
 * table/f2/f2_vacuum.c - VACUUM of a sift_table: the freeze that moves pg_class.relfrozenxid, the cleanup of the secondary indexes, and ANALYZE.
 *
 * The steps are those of a heap VACUUM in the order the heap keeps: the aged slabs and pending deletes are written out and folded, the addresses of the rows that are dead for every
 * snapshot (a delete that committed before the horizon, an insert that aborted) are given to the indexes in batches that fit maintenance_work_mem, then the parts that hold such
 * rows are compacted and the deletion vectors settled (a crash between the two leaves rows with an index entry and no address to find it by, so no part is rewritten before the indexes
 * let go of its dead rows), and last the oldest transaction id any page of the table still holds is computed again and becomes relfrozenxid.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/tidstore.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "core/f2/f2_progress.h"
#include "col/f2/delvec_delta.h"
#include "core/wal_conflict.h"
#include "core/u64set.h"
#include "table/f2/f2_col_delta.h"
#include "col/f2/f2.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_tier_host.h"
#include "table/f2/f2_tbl.h"

#define SIFT_VAC_L0_FLUSH_ROWS 2048

typedef struct VacCtx
{
	Relation	rel;
	Relation   *indexes;
	int			nindexes;
	IndexBulkDeleteResult **stats;
	BufferAccessStrategy strategy;
	uint64	   *tids;
	uint32		ntids;
	uint32		cap;
	uint64		total_dead;
	int			batches;
} VacCtx;

/* ---- the indexes ---- */

static bool
tid_reaped(ItemPointer itemptr, void *state)
{
	return TidStoreIsMember((TidStore *) state, itemptr);
}

static int
cmp_tid(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static uint64
vc_max_bytes(void)
{
	return (uint64) maintenance_work_mem * 1024;
}

static uint32
vc_max_tids(void)
{
	return (uint32) Max(1024, vc_max_bytes() / 2 / sizeof(uint64));
}

static TidStore *
batch_store(const VacCtx *vc)
{
	/* MEM[MEM-TBL-03]: table vacuum dead TIDs (sorted array <= mwm/2 + TidStore <= mwm/2; was 2x mwm) */
	TidStore   *ts = TidStoreCreateLocal(vc_max_bytes() / 2, true);
	OffsetNumber offsets[MaxHeapTuplesPerPage];
	uint32		i = 0;

	while (i < vc->ntids)
	{
		BlockNumber blk = (BlockNumber) (vc->tids[i] >> 16);
		int			n = 0;

		for (; i < vc->ntids && (BlockNumber) (vc->tids[i] >> 16) == blk && n < MaxHeapTuplesPerPage; i++)
			if (n == 0 || offsets[n - 1] != (OffsetNumber) (vc->tids[i] & 0xFFFF))
				offsets[n++] = (OffsetNumber) (vc->tids[i] & 0xFFFF);
		TidStoreSetBlockOffsets(ts, blk, offsets, n);
	}
	return ts;
}

static void
flush_batch(VacCtx *vc)
{
	TidStore   *ts;

	if (vc->ntids == 0)
		return;
	pg_qsort(vc->tids, vc->ntids, sizeof(uint64), cmp_tid);
	ts = batch_store(vc);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_VACUUM_INDEX);
	sift_f2_vacuum_progress_indexes(vc->nindexes, 0);
	for (int i = 0; i < vc->nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = vc->indexes[i],.heaprel = vc->rel,.estimated_count = true,.message_level = DEBUG2,.strategy = vc->strategy};

		vc->stats[i] = index_bulk_delete(&ivinfo, vc->stats[i], tid_reaped, ts);
		sift_f2_vacuum_progress_indexes(vc->nindexes, i + 1);
	}
	TidStoreDestroy(ts);
	vc->total_dead += vc->ntids;
	vc->ntids = 0;
	vc->batches++;
	sift_f2_vacuum_progress_dead(vc->total_dead, 0, vc_max_bytes(), vc->batches);
}

static void
grow_tids(VacCtx *vc, uint32 max)
{
	vc->cap = Min(Max(1024, vc->cap * 2), max);
	vc->tids = vc->tids == NULL ? palloc(sizeof(uint64) * vc->cap) : repalloc(vc->tids, sizeof(uint64) * vc->cap);
}

void
sift_tbl_f2_vacuum_dead_tids(void *vac_ctx, const uint64 *tids, uint32 ntids)
{
	VacCtx	   *vc = vac_ctx;
	uint32		max = vc_max_tids();

	for (uint32 i = 0; i < ntids; i++)
	{
		if (vc->ntids == max)
			flush_batch(vc);
		if (vc->ntids == vc->cap)
			grow_tids(vc, max);
		vc->tids[vc->ntids++] = tids[i];
	}
	sift_f2_vacuum_progress_dead(vc->total_dead + vc->ntids, (uint64) vc->ntids * sizeof(uint64), vc_max_bytes(), vc->batches);
}

/* ---- the rows dead for every snapshot ---- */

static bool
aborted(TransactionId xid)
{
	return TransactionIdIsNormal(xid) && !TransactionIdIsInProgress(xid) && !TransactionIdDidCommit(xid);
}

static bool
settled_delete(const SiftDvXmaxEntry *e, TransactionId oldest)
{
	return e == NULL || !TransactionIdIsNormal(e->xmax) || (TransactionIdPrecedes(e->xmax, oldest) && TransactionIdDidCommit(e->xmax));
}

static void
collect_part(VacCtx *vc, SiftF2Store *store, const SiftPartitionEntry *e, const SiftPartDesc *d, TransactionId oldest)
{
	SiftF2PartPin *pin = sift_f2_pin_open(vc->rel, store, d, (uint32) e->part_id);
	uint64	   *dead = palloc(sizeof(uint64) * SIFT_GRANULE_ROWS);

	PG_TRY();
	{
		for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		{
			SiftF2GranuleBuf *gb = sift_f2_gbuf_load(vc->rel, store, pin, g);
			const SiftDvGran *dvg = pin->dv != NULL ? sift_dv_granule(pin->dv, g) : NULL;
			const uint64 *tids = sift_f2_gbuf_tids(gb);
			uint32		n = 0;

			for (uint16 r = 0; r < gb->nrows; r++)
			{
				bool		isdead = !gb->is_all_frozen && aborted(gb->is_single_xact ? gb->single_xmin : gb->vers[r].xmin);

				if (!isdead && dvg != NULL)
				{
					bool		del;
					const SiftDvXmaxEntry *x = sift_dv_find(dvg, r, &del);

					isdead = del && settled_delete(x, oldest);
				}
				if (isdead)
					dead[n++] = tids[r];
			}
			if (n > 0)
				store->host->on_vacuum_dead_tids(vc, dead, n);
			sift_f2_gbuf_unref(gb);
			CHECK_FOR_INTERRUPTS();
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	pfree(dead);
}

/* ---- freezing ---- */

static bool
settle_due(const SiftPartDesc *d, FullTransactionId oldest_fx)
{
	/* STOR[STOR-TBL-02]: DV xmax settle horizon (8B/dead_row + 24B/granule; oldest_fx vs 50M-XID freeze_fx) */
	return d->dv_blk != InvalidBlockNumber && (d->flags & SIFT_PART_FLAG_DV_SETTLED) == 0 && FullTransactionIdPrecedes(d->dv_max_xid, oldest_fx);
}

/* The settled deletion vector of a part is written; the descriptor that names it is nd, and old the extent it replaces, for the commit of the partition to publish */
static void
settle_dv(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *cur, FullTransactionId oldest_fx, SiftPartDesc *nd, SiftExtent *old)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftF2PartPin *pin = sift_f2_pin_open(rel, store, cur, (uint32) e->part_id);
	FullTransactionId stripped = InvalidFullTransactionId;
	SiftDv	   *settled;

	*nd = *cur;
	*old = (SiftExtent) {cur->dv_blk, cur->dv_nblocks};
	PG_TRY();
	{
		settled = sift_dv_settle(pin->dv, oldest_fx, &stripped);
		if (FullTransactionIdIsValid(stripped))
			sift_wal_snapshot_conflict(rel, cur->dv_blk, stripped);
		sift_dv_write(rel, settled, (uint32) e->part_id, nd);
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
}

/*
 * Settles the deletion vectors of the parts of one partition that are due, or seals them, from the descriptors the manifest holds under the partition lock (a descriptor read before the lock may
 * name blocks that a delete released since): the lock is taken, the entry and the parts read, and the partition committed once for all of them.
 */
static void
settle_or_seal_parts(Relation rel, const SiftPartitionEntry *cand, const uint64 *gens, int ngens, FullTransactionId oldest_fx, SiftXidHorizonCache *hc)
{
	SiftPartitionEntry e;
	SiftU64Set	want;
	SiftPartDesc *parts,
			   *update,
			   *expect;
	SiftExtent *release;
	int			np,
				nupdate = 0,
				nrelease = 0;

	if (ngens == 0)
		return;
	LockPage(rel, SIFT_PART_LOCK_BLKNO(cand->part_id), ExclusiveLock);
	if (sift_manifest_find_entry(rel, cand->part_id, cand->key_lo, &e))
	{
		parts = sift_manifest_read_parts(rel, &e, &np);
		update = palloc(sizeof(SiftPartDesc) * ngens);
		expect = palloc(sizeof(SiftPartDesc) * ngens);
		release = palloc(sizeof(SiftExtent) * ngens);
		sift_u64set_init(&want, ngens);
		for (int i = 0; i < ngens; i++)
			(void) sift_u64set_add(&want, gens[i]);
		for (int i = 0; i < np; i++)
		{
			if (!sift_u64set_has(&want, parts[i].part_gen))
				continue;
			if (settle_due(&parts[i], oldest_fx))
				settle_dv(rel, &e, &parts[i], oldest_fx, &update[nupdate], &release[nrelease++]);
			else
			{
				update[nupdate] = parts[i];
				if (!sift_part_try_seal(rel, (uint32) e.part_id, &update[nupdate], hc))
					continue;
			}
			expect[nupdate++] = parts[i];
		}
		if (nupdate > 0)
			sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = e.part_id,.key_lo = e.key_lo,.update_parts = update,.expect_parts = expect,.nupdate = nupdate,.release = release,.nrelease = nrelease});
		sift_u64set_free(&want);
		pfree(parts);
		pfree(update);
		pfree(expect);
		pfree(release);
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(cand->part_id), ExclusiveLock);
}

static bool
needs_compaction(const SiftPartDesc *d, FullTransactionId target, int dead_pct)
{
	if (d->ext_flags & SIFT_PART_EXT_REMOTE)
		return false;
	/* STOR[STOR-TBL-04]: dead-part compaction gate (relopt dead_rewrite_pct + 0-WAL drop when ndead==nrows) */
	return d->level == 0 || (d->nrows > 0 && d->ndead * 100 >= d->nrows * (uint64) dead_pct) ||
		((d->flags & SIFT_PART_FLAG_UNFROZEN) != 0 && FullTransactionIdPrecedes(d->min_xid, target));
}

static bool
slab_flush_due(Relation rel, const SiftPartitionEntry *cand, FullTransactionId oldest_fx, bool aggressive)
{
	if (cand->l0_slab_blk == InvalidBlockNumber)
		return false;
	if (aggressive || cand->pend_del_blk != InvalidBlockNumber)
		return aggressive || FullTransactionIdPrecedes(cand->min_xid, oldest_fx);
	return FullTransactionIdPrecedes(cand->min_xid, oldest_fx) && sift_l0_slab_rows(rel, cand->part_id, cand->l0_slab_blk) >= SIFT_VAC_L0_FLUSH_ROWS;
}

/* First pass of a partition: what is written out and folded, and the addresses of the rows that are dead for everyone, queued for the indexes */
static void
collect_partition(VacCtx *vc, SiftF2Store *store, const SiftPartitionEntry *cand, const struct VacuumCutoffs *cutoffs, FullTransactionId oldest_fx, bool aggressive)
{
	Relation	rel = vc->rel;
	SiftPartitionEntry e;
	int			np;
	SiftPartDesc *parts;

	/* STOR[STOR-TBL-03]: L0 slab + pend_del vacuum horizon (40B+96B+raw rows / 16B/del; oldest_fx vs freeze_fx) */
	if (slab_flush_due(rel, cand, oldest_fx, aggressive))
		sift_l0_slab_flush(rel, cand->part_id, cand->key_lo, &store->spec);
	if (cand->pend_del_blk != InvalidBlockNumber && (aggressive || FullTransactionIdPrecedes(cand->min_xid, oldest_fx)))
		sift_del_bulk(rel, cand->part_id, cand->key_lo, NULL, 0, true, &store->spec);
	(void) sift_dv_consolidate(rel, cand->part_id, cand->key_lo);
	if ((cand->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		(void) sift_col_delta_gc(rel, cand->part_id, cand->key_lo, oldest_fx);
	if (!sift_manifest_find_entry(rel, cand->part_id, cand->key_lo, &e))
		return;
	parts = sift_manifest_read_parts(rel, &e, &np);
	for (int i = 0; i < np; i++)
		if (parts[i].ndead > 0 || (parts[i].flags & SIFT_PART_FLAG_UNFROZEN) != 0)
			collect_part(vc, store, &e, &parts[i], cutoffs->OldestXmin);
}

/* Second pass, once the indexes have given up the addresses of the first: the deletion vectors are settled, the parts sealed and the ones with dead or old rows compacted, which drops the rows the indexes no longer point at */
static void
settle_partition(Relation rel, SiftF2Store *store, const SiftPartitionEntry *cand, FullTransactionId oldest_fx, FullTransactionId target_fx, bool aggressive)
{
	SiftPartitionEntry e;
	int			np;
	SiftPartDesc *parts;
	uint64	   *gens = NULL,
			   *settle = NULL;
	int			ngens = 0,
				nsettle = 0;
	SiftCompactResult res;
	SiftXidHorizonCache hc;
	int			dead_pct = store->opts.dead_rewrite_pct > 0 ? store->opts.dead_rewrite_pct : sift_dead_rewrite_pct;

	if (!sift_manifest_find_entry(rel, cand->part_id, cand->key_lo, &e))
		return;
	parts = sift_manifest_read_parts(rel, &e, &np);
	sift_xid_horizon_init(&hc, oldest_fx);
	for (int i = 0; i < np; i++)
	{
		if (needs_compaction(&parts[i], target_fx, dead_pct))
		{
			gens = gens == NULL ? palloc(sizeof(uint64) * np) : gens;
			gens[ngens++] = parts[i].part_gen;
		}
		else
		{
			settle = settle == NULL ? palloc(sizeof(uint64) * np) : settle;
			settle[nsettle++] = parts[i].part_gen;
		}
	}
	settle_or_seal_parts(rel, &e, settle, nsettle, oldest_fx, &hc);
	if ((e.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
	{
		gens = gens == NULL ? palloc(sizeof(uint64) * np) : gens;
		ngens = sift_col_delta_fold_gens(rel, &e, parts, np, oldest_fx, aggressive, gens, ngens);
	}
	if (ngens > 0)
		(void) sift_partition_compact_online(rel, e.part_id, e.key_lo, gens, ngens, &store->spec, PG_INT64_MIN, oldest_fx, &res);
	if ((e.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		(void) sift_col_delta_gc(rel, e.part_id, e.key_lo, oldest_fx);
}

/* The oldest transaction id any page of the table still holds: what relfrozenxid may become */
static FullTransactionId
recompute_oldest(Relation rel, FullTransactionId oldest_fx)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	FullTransactionId lowest = oldest_fx;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber || mr.entries[i].pend_del_blk != InvalidBlockNumber)
			lowest = FullTransactionIdPrecedes(mr.entries[i].min_xid, lowest) ? mr.entries[i].min_xid : lowest;
		if ((mr.entries[i].flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		{
			FullTransactionId chain_min = sift_col_delta_min_xid(rel, &mr.entries[i]);

			if (FullTransactionIdIsValid(chain_min) && FullTransactionIdPrecedes(chain_min, lowest))
				lowest = chain_min;
		}
		for (int j = 0; j < np; j++)
		{
			bool		holds = (parts[j].flags & SIFT_PART_FLAG_UNFROZEN) != 0 || (parts[j].dv_blk != InvalidBlockNumber && (parts[j].flags & SIFT_PART_FLAG_DV_SETTLED) == 0);

			if (holds && FullTransactionIdPrecedes(parts[j].min_xid, lowest))
				lowest = parts[j].min_xid;
		}
	}
	return lowest;
}

static void
store_oldest_active(Relation rel, FullTransactionId oldest)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->oldest_active_xid = oldest;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
}

static void
update_relstats(Relation rel, FullTransactionId oldest, const struct VacuumCutoffs *cutoffs)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	bool		fz,
				mz;
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	double		live = (double) (meta.total_rows - meta.total_dead);

#if PG_VERSION_NUM >= 180000
	vac_update_relstats(rel, nblocks, live, 0, 0, rel->rd_rel->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
#else
	vac_update_relstats(rel, nblocks, live, 0, rel->rd_rel->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
#endif
	sift_pgstat_report_vacuum(rel, (PgStat_Counter) live, (PgStat_Counter) meta.total_dead, GetCurrentTimestamp());
}

/* A limit that wrapped below the first xid of a young cluster (epoch 0) is before every row: the epoch before 0 would turn it into the newest xid */
static FullTransactionId
cutoff_fx(FullTransactionId next, TransactionId xid)
{
	if (EpochFromFullTransactionId(next) == 0 && xid > XidFromFullTransactionId(next))
		return FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
	return FullTransactionIdFromAllowableAt(next, xid);
}

void
sift_tbl_f2_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	struct VacuumCutoffs cutoffs;
	bool		aggressive = vacuum_get_cutoffs(rel, params, &cutoffs);
	FullTransactionId next = ReadNextFullTransactionId();
	FullTransactionId oldest_fx = cutoff_fx(next, cutoffs.OldestXmin);
	FullTransactionId freeze_fx = cutoff_fx(next, cutoffs.FreezeLimit);
	FullTransactionId target_fx = aggressive ? oldest_fx : freeze_fx;
	VacCtx		vc = {.rel = rel,.strategy = bstrategy};
	SiftManifestRead mr;
	FullTransactionId new_oldest;
	int			elevel = (params->options & VACOPT_VERBOSE) != 0 ? INFO : DEBUG2;
	SiftF2TableTrace trace = {0};
	SiftColMetaPageData meta;
	/* STOR[STOR-TBL-01]: vacuum_rel resolves UNSPECIFIED to ENABLED/DISABLED before the AM runs, so the gate is the resolved value (tail shrink of the free trailing extents) */
	const bool	truncate = params->truncate == VACOPTVALUE_ENABLED && store->opts.vacuum_truncate;

	sift_f2_vacuum_progress_begin(rel);
	if (elevel == INFO)
		sift_f2_table_trace_begin(&trace, rel);
	vac_open_indexes(rel, RowExclusiveLock, &vc.nindexes, &vc.indexes);
	vc.stats = palloc0(sizeof(IndexBulkDeleteResult *) * Max(vc.nindexes, 1));
	sift_stage_flush(rel);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	sift_f2_vacuum_progress_total(rel);
	for (int i = 0; i < mr.n; i++)
	{
		collect_partition(&vc, store, &mr.entries[i], &cutoffs, oldest_fx, aggressive);
		sift_f2_vacuum_progress_scanned(i + 1, mr.n);
	}
	flush_batch(&vc);
	for (int i = 0; i < mr.n; i++)
		settle_partition(rel, store, &mr.entries[i], oldest_fx, target_fx, aggressive);
	sift_f2_tier_maintain(rel, store, oldest_fx);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_INDEX_CLEANUP);
	sift_f2_vacuum_progress_indexes(vc.nindexes, 0);
	for (int i = 0; i < vc.nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = vc.indexes[i],.heaprel = rel,.estimated_count = true,.message_level = DEBUG2,.strategy = bstrategy,
		.num_heap_tuples = (double) (sift_col_meta_read(rel).total_rows)};

		vc.stats[i] = index_vacuum_cleanup(&ivinfo, vc.stats[i]);
		sift_f2_vacuum_progress_indexes(vc.nindexes, i + 1);
	}
	vac_close_indexes(vc.nindexes, vc.indexes, NoLock);
	if (sift_proj_relation_has(rel))
		(void) sift_proj_refresh(rel, store);
	if (store->nscopes > 0)
		(void) sift_f2_scope_refresh(rel);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);
	new_oldest = recompute_oldest(rel, oldest_fx);
	store_oldest_active(rel, new_oldest);
	update_relstats(rel, new_oldest, &cutoffs);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_TRUNCATE);
	sift_fsm_reclaim(rel);
	if (truncate)
	{
		sift_f2_tail_relocate(rel, sift_f2_store_open(rel), oldest_fx, PG_INT64_MIN);
		(void) sift_fsm_tail_shrink(rel);
	}
	meta = sift_col_meta_read(rel);
	sift_f2_say_table_vacuum(&trace, rel, elevel, vc.total_dead, vc.batches, (double) (meta.total_rows - meta.total_dead), (double) meta.total_dead);
	sift_f2_vacuum_progress_end();
}

/* ---- ANALYZE ---- */

bool
sift_tbl_f2_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	static TableScanDesc last_scan = NULL;
	static uint64 last_ordinal = UINT64_MAX;
	uint64		ngranules = sift_tbl_f2_scan_ngranules(scan);

	if (last_scan != scan)
	{
		last_scan = scan;
		last_ordinal = UINT64_MAX;
	}
	for (;;)
	{
		Buffer		buf = read_stream_next_buffer(stream, NULL);
		BlockNumber blk,
					nblocks;
		uint64		ordinal;

		if (!BufferIsValid(buf))
			return false;
		blk = BufferGetBlockNumber(buf);
		ReleaseBuffer(buf);
		nblocks = Max(RelationGetNumberOfBlocks(scan->rs_rd), 1);
		ordinal = ngranules * (uint64) blk / nblocks;
		if (ngranules == 0 || ordinal == last_ordinal)
			continue;
		last_ordinal = ordinal;
		if (sift_tbl_f2_scan_goto_granule(scan, ordinal))
			return true;
	}
}

bool
sift_tbl_f2_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows pg_attribute_unused(), TupleTableSlot *slot)
{
	if (!sift_tbl_f2_scan_next_in_granule(scan, slot))
		return false;
	(*liverows)++;
	return true;
}
