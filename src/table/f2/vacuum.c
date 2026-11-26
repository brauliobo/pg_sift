/*
 * table/f2/vacuum.c - VACUUM of a sift_table: the freeze that moves pg_class.relfrozenxid, the cleanup of the secondary indexes, and ANALYZE.
 *
 * The steps are those of a heap VACUUM in the order the heap keeps: the aged slabs and pending deletes are written out and folded, the addresses of the rows that are dead for every
 * snapshot (a delete that committed before the horizon, an insert that aborted) are given to the indexes in batches that fit the maintenance budget, then the parts that hold such
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
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "core/compat.h"
#include "core/f2/progress.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/del/pend.h"
#include "col/f2/l0/slab.h"
#include "core/cmp.h"
#include "core/horizon.h"
#include "core/page_io.h"
#include "core/maint/vacuum.h"
#include "core/memtrack.h"
#include "core/wal_conflict.h"
#include "core/u64set.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/lock/mxid.h"
#include "col/f2/f2.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/tier/host.h"
#include "table/f2/tbl.h"
#include "table/f2/vm/vm.h"

#define SIFT_VAC_L0_FLUSH_ROWS 2048

#define SIFT_VAC_ROW_BYTES 64
#define SIFT_VAC_EXPANSION 8

/* The position of heap_blks_scanned inside a part, in steps of a part */
#define VAC_PROGRESS_STEPS	1024

typedef struct VacCtx
{
	MemoryContext cx;			/* what outlives a partition: the addresses of a batch and the results of the indexes */
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
	int			parts_done;		/* the parts the first pass has been through, of parts_total: the position of heap_blks_scanned */
	int			parts_total;
	BlockNumber nblocks;		/* the file when the VACUUM began: heap_blks_vacuumed counts against it */
} VacCtx;

/* ---- the indexes ---- */

static bool
tid_reaped(ItemPointer itemptr, void *state)
{
	return TidStoreIsMember((TidStore *) state, itemptr);
}

static uint64
vc_max_bytes(void)
{
	return sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1);
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
	MemoryContext old;
	TidStore   *ts;

	if (vc->ntids == 0)
		return;
	old = MemoryContextSwitchTo(vc->cx);
	pg_qsort(vc->tids, vc->ntids, sizeof(uint64), sift_cmp_u64);
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
	MemoryContextSwitchTo(old);
}

static void
grow_tids(VacCtx *vc, uint32 max)
{
	vc->cap = Min(Max(1024, vc->cap * 2), max);
	vc->tids = vc->tids == NULL ? MemoryContextAlloc(vc->cx, sizeof(uint64) * vc->cap) : repalloc(vc->tids, sizeof(uint64) * vc->cap);
}

static void
drop_tids(VacCtx *vc)
{
	pfree(vc->tids);
	vc->tids = NULL;
	vc->cap = 0;
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

/* The addresses of the rows of granule g that are dead for every snapshot, in tids (rows is the scratch of the walk); how many. A granule with no row in doubt is not read: one that is frozen and has no delete, or that one transaction wrote and has none either */
static uint32
granule_dead(Relation rel, SiftF2PartPin *pin, uint32 g, TransactionId oldest, uint16 *rows, uint64 *tids)
{
	const SiftGranuleDesc *gd = &pin->gdir[g];
	const SiftDvGran *dvg = pin->dv != NULL ? sift_dv_granule(pin->dv, g) : NULL;
	const bool	frozen = (gd->flags & SIFT_GDESC_ALL_FROZEN) != 0,
				single = (gd->flags & SIFT_GDESC_SINGLE_XACT) != 0;
	const bool	single_aborted = !frozen && single && aborted(XidFromFullTransactionId(gd->min_xid));
	SiftRowVer *vers;
	uint32		n = 0;

	if (dvg == NULL && (frozen || (single && !single_aborted)))
		return 0;
	vers = frozen || single ? NULL : sift_part_read_vers(rel, pin->part, pin->gdir, g);
	for (uint16 r = 0; r < gd->nrows; r++)
	{
		bool		isdead = !frozen && (single ? single_aborted : aborted(vers[r].xmin));

		if (!isdead && dvg != NULL)
		{
			bool		del;
			const SiftDvXmaxEntry *x = sift_dv_find(dvg, r, &del);

			isdead = del && settled_delete(x, oldest);
		}
		if (isdead)
			rows[n++] = r;
	}
	if (n > 0)
	{
		sift_f2_pin_granule_tids(rel, pin, g, tids);
		for (uint32 i = 0; i < n; i++)
			tids[i] = tids[rows[i]];
	}
	return n;
}

/* The pin of a part that VACUUM reads once, to find its dead rows or settle its deletion vector, is not cached: the cache of pins keeps nothing of a part about to be rewritten */
static void
collect_part(VacCtx *vc, SiftF2Store *store, const SiftPartitionEntry *e, const SiftPartDesc *d, TransactionId oldest)
{
	SiftF2PartPin *pin = sift_f2_pin_open_uncached(vc->rel, store, d, (uint32) e->part_id);
	MemoryContext gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift table vacuum granule");
	uint64	   *tids = palloc(sizeof(uint64) * SIFT_GRANULE_ROWS);
	uint16	   *rows = palloc(sizeof(uint16) * SIFT_GRANULE_ROWS);

	PG_TRY();
	{
		for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		{
			MemoryContext old = MemoryContextSwitchTo(gcx);
			uint32		n = granule_dead(vc->rel, pin, g, oldest, rows, tids);

			MemoryContextSwitchTo(old);
			MemoryContextReset(gcx);
			if (n > 0)
				store->host->on_vacuum_dead_tids(vc, tids, n);
			sift_f2_vacuum_progress_scanned(Min(vc->parts_done * VAC_PROGRESS_STEPS + (int) ((uint64) VAC_PROGRESS_STEPS * (g + 1) / pin->part->hdr.ngranules),
												vc->parts_total * VAC_PROGRESS_STEPS), vc->parts_total * VAC_PROGRESS_STEPS);
			sift_vacuum_delay_point();
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	MemoryContextDelete(gcx);
	pfree(tids);
	pfree(rows);
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
	SiftF2PartPin *pin = sift_f2_pin_open_uncached(rel, store, cur, (uint32) e->part_id);
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
	{
		if (vc->nindexes > 0 && (parts[i].ndead > 0 || (parts[i].flags & SIFT_PART_FLAG_UNFROZEN) != 0))
			collect_part(vc, store, &e, &parts[i], cutoffs->OldestXmin);
		sift_f2_vacuum_progress_scanned(Min(++vc->parts_done, vc->parts_total) * VAC_PROGRESS_STEPS, vc->parts_total * VAC_PROGRESS_STEPS);
	}
}

static void
collect_all(VacCtx *vc, SiftF2Store *store, const SiftManifestRead *mr, const struct VacuumCutoffs *cutoffs, FullTransactionId oldest_fx, bool aggressive)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift table vacuum partition");
	MemoryContext old = MemoryContextSwitchTo(cx);

	for (int i = 0; i < mr->n; i++)
	{
		sift_vacuum_delay_point();
		collect_partition(vc, store, &mr->entries[i], cutoffs, oldest_fx, aggressive);
		MemoryContextReset(cx);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* What a part takes in the buffer of the compaction that rewrites it */
static uint64
part_mem(const SiftPartDesc *d)
{
	return Max((uint64) d->nrows * SIFT_VAC_ROW_BYTES, d->nbytes * SIFT_VAC_EXPANSION);
}

static int
by_age(const void *a, const void *b)
{
	const SiftPartDesc *x = *(const SiftPartDesc *const *) a,
			   *y = *(const SiftPartDesc *const *) b;
	int			c = SIFT_CMP3(U64FromFullTransactionId(x->min_xid), U64FromFullTransactionId(y->min_xid));

	return c != 0 ? c : SIFT_CMP3(x->part_gen, y->part_gen);
}

/* One compaction of the group (a join of its parts when join), in a context of its own that goes with it; the bytes it wrote */
static uint64
compact_group(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, const uint64 *gens, int ngens, bool join, FullTransactionId oldest_fx)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift table vacuum compaction");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftCompactResult res;
	Size		freed;

	(void) (join ? sift_partition_join_online : sift_partition_compact_online) (rel, e->part_id, e->key_lo, gens, ngens, &store->spec, PG_INT64_MIN, oldest_fx, &res);
	MemoryContextSwitchTo(old);
	freed = MemoryContextMemAllocated(cx, true);
	MemoryContextDelete(cx);
	sift_mem_release_os(freed);
	return res.bytes_out;
}

/*
 * The parts to compact, the oldest transaction first, in groups that a half of the budget holds (a part that is more than it holds goes alone), each group in one commit of the partition:
 * a crash or a cancel between two keeps what the commits before it did, and the oldest transaction a partition holds only goes up. Once the pass has written sift.vacuum_rewrite_limit
 * the rest waits for the next VACUUM.
 */
static void
compact_chunks(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, const SiftPartDesc *parts, int np, const uint64 *gens, int ngens, FullTransactionId oldest_fx, uint64 *rewrite_left)
{
	const SiftPartDesc **chosen = palloc(sizeof(SiftPartDesc *) * ngens);
	uint64	   *group = palloc(sizeof(uint64) * ngens);
	const uint64 budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2);
	SiftU64Set	want;
	int			n = 0;

	sift_u64set_init(&want, ngens);
	for (int i = 0; i < ngens; i++)
		(void) sift_u64set_add(&want, gens[i]);
	for (int i = 0; i < np; i++)
		if (sift_u64set_has(&want, parts[i].part_gen))
			chosen[n++] = &parts[i];
	sift_u64set_free(&want);
	qsort(chosen, n, sizeof(SiftPartDesc *), by_age);
	for (int i = 0; i < n && *rewrite_left > 0;)
	{
		uint64		mem = 0,
					written;
		int			k = 0;

		for (; i < n && (k == 0 || mem + part_mem(chosen[i]) <= budget); i++, k++)
		{
			group[k] = chosen[i]->part_gen;
			mem += part_mem(chosen[i]);
		}
		written = compact_group(rel, store, e, group, k, false, oldest_fx);
		*rewrite_left -= Min(written, *rewrite_left);
	}
	pfree(chosen);
	pfree(group);
}

/* What the plan of the partition rewrites of the parts the compaction left (the level-1 parts of a bulk insert, the parts of the last level that a small budget wrote one granule each): the groups as it makes them, each in one commit */
static void
compact_planned(Relation rel, SiftF2Store *store, const SiftPartitionEntry *cand, FullTransactionId oldest_fx, uint64 *rewrite_left)
{
	SiftPartitionEntry e;
	int			np,
				ngroups;
	SiftPartDesc *parts;
	SiftCompactGroup *groups;

	if (!sift_manifest_find_entry(rel, cand->part_id, cand->key_lo, &e))
		return;
	parts = sift_manifest_read_parts(rel, &e, &np);
	groups = sift_compact_plan_rewrites(parts, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), &ngroups);
	for (int k = 0; k < ngroups && *rewrite_left > 0; k++)
	{
		uint64		written = compact_group(rel, store, &e, groups[k].gens, groups[k].n, groups[k].join, oldest_fx);

		*rewrite_left -= Min(written, *rewrite_left);
	}
}

/* Second pass, once the indexes have given up the addresses of the first: the deletion vectors are settled, the parts sealed and the ones with dead or old rows compacted, which drops the rows the indexes no longer point at */
static void
settle_partition(Relation rel, SiftF2Store *store, const SiftPartitionEntry *cand, FullTransactionId oldest_fx, FullTransactionId target_fx, bool aggressive, uint64 *rewrite_left)
{
	SiftPartitionEntry e;
	int			np;
	SiftPartDesc *parts;
	uint64	   *gens = NULL,
			   *settle = NULL;
	int			ngens = 0,
				nsettle = 0;
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
		ngens = sift_col_delta_fold_gens(rel, &e, parts, np, oldest_fx, aggressive || !AmAutoVacuumWorkerProcess(), gens, ngens);
	}
	if (ngens > 0)
		compact_chunks(rel, store, &e, parts, np, gens, ngens, oldest_fx, rewrite_left);
	compact_planned(rel, store, &e, oldest_fx, rewrite_left);
	if ((e.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		(void) sift_col_delta_gc(rel, e.part_id, e.key_lo, oldest_fx);
}

/* A frozen or invalid id holds nothing back: only a normal transaction id older than lowest lowers it */
static FullTransactionId
older_normal(FullTransactionId lowest, FullTransactionId xid)
{
	return FullTransactionIdIsNormal(xid) && FullTransactionIdPrecedes(xid, lowest) ? xid : lowest;
}

/* The oldest transaction id the pages of a chain (micro-slab, pending deletes) hold in their headers, or lowest when none is older */
static FullTransactionId
chain_oldest(Relation rel, uint64 part_id, BlockNumber head, SiftPageType type, FullTransactionId lowest)
{
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
		const char *data = SiftPageGetData(BufferGetPage(buf));
		const SiftL0SlabPageData *sl = (const SiftL0SlabPageData *) data;
		const SiftPendDelPageData *pd = (const SiftPendDelPageData *) data;
		bool		slab = type == SIFT_PT_L0_SLAB;

		sift_page_expect(rel, BufferGetPage(buf), blk, type, 0, (uint32) part_id);
		lowest = older_normal(lowest, slab ? sl->min_xid : pd->min_xid);
		blk = slab ? sl->next_blk : pd->next_blk;
		UnlockReleaseBuffer(buf);
	}
	return lowest;
}

/* The oldest deleter of a deletion vector that is not settled */
static FullTransactionId
dv_oldest(Relation rel, const SiftPartDesc *d, FullTransactionId lowest)
{
	Buffer		buf = sift_blk_read(rel, d->dv_blk, BUFFER_LOCK_SHARE);

	lowest = older_normal(lowest, ((const SiftDvHeaderData *) SiftPageGetData(BufferGetPage(buf)))->xid_min);
	UnlockReleaseBuffer(buf);
	return lowest;
}

/* The oldest transaction id the pages of a partition still hold, or lowest when none is older: what the summary of its parts says is not enough, it leaves out the micro-slab and the pending deletes */
static FullTransactionId
partition_oldest(Relation rel, const SiftPartitionEntry *e, FullTransactionId lowest)
{
	int			np;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);

	lowest = chain_oldest(rel, e->part_id, e->l0_slab_blk, SIFT_PT_L0_SLAB, lowest);
	lowest = chain_oldest(rel, e->part_id, e->pend_del_blk, SIFT_PT_PEND_DEL, lowest);
	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		lowest = older_normal(lowest, sift_col_delta_min_xid(rel, e));
	for (int j = 0; j < np; j++)
	{
		if ((parts[j].flags & SIFT_PART_FLAG_UNFROZEN) != 0)
			lowest = older_normal(lowest, parts[j].min_xid);
		if (parts[j].dv_blk != InvalidBlockNumber && (parts[j].flags & SIFT_PART_FLAG_DV_SETTLED) == 0)
			lowest = dv_oldest(rel, &parts[j], lowest);
	}
	pfree(parts);
	return lowest;
}

/* The oldest transaction id any page of the table still holds: what relfrozenxid may become */
static FullTransactionId
recompute_oldest(Relation rel, FullTransactionId oldest_fx)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	FullTransactionId lowest = oldest_fx;

	for (int i = 0; i < mr.n; i++)
		lowest = partition_oldest(rel, &mr.entries[i], lowest);
	return lowest;
}

/* The oldest transaction a page still holds, or invalid when none is older than the horizon: the horizon moves with every VACUUM and is not worth a record of the metapage */
static void
store_oldest_active(Relation rel, FullTransactionId oldest, FullTransactionId horizon)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	FullTransactionId held = FullTransactionIdPrecedes(oldest, horizon) ? oldest : InvalidFullTransactionId;
	SiftXlog	x;
	Page		p0;
	SiftColMetaPageData *m;

	if (FullTransactionIdEquals(SIFT_COL_META(BufferGetPage(buf))->oldest_active_xid, held))
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	x = sift_xlog_start(rel);
	p0 = sift_xlog_register(&x, buf, 0);
	m = SIFT_COL_META(p0);
	m->oldest_active_xid = held;
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
	double		live = (double) (meta.total_rows - meta.total_dead),
				allvisfrac;
	BlockNumber relallvisible,
				relallfrozen;

	sift_tbl_vm_fractions(rel, nblocks, &relallvisible, &relallfrozen, &allvisfrac);
#if PG_VERSION_NUM >= 180000
	vac_update_relstats(rel, nblocks, live, relallvisible, relallfrozen, rel->rd_rel->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
#else
	vac_update_relstats(rel, nblocks, live, relallvisible, rel->rd_rel->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
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

/* What the pages hold now, as far as it is known, becomes relfrozenxid at once (the statistics of the table are those of the end of the VACUUM): a VACUUM that does not end has moved it as far as it went */
static void
advance_frozenxid(Relation rel, FullTransactionId oldest, FullTransactionId horizon, const struct VacuumCutoffs *cutoffs)
{
	bool		fz,
				mz;
	Form_pg_class form = rel->rd_rel;

	store_oldest_active(rel, oldest, horizon);
#if PG_VERSION_NUM >= 180000
	vac_update_relstats(rel, form->relpages, form->reltuples, form->relallvisible, form->relallfrozen, form->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
#else
	vac_update_relstats(rel, form->relpages, form->reltuples, form->relallvisible, form->relhasindex, XidFromFullTransactionId(oldest), cutoffs->OldestMxact, &fz, &mz, false);
#endif
}

typedef struct Held
{
	int			part;			/* the place of the partition in the manifest read */
	FullTransactionId oldest;	/* what it holds, before it is settled */
} Held;

static int
by_held(const void *a, const void *b)
{
	const Held *x = a,
			   *y = b;
	int			c = SIFT_CMP3(U64FromFullTransactionId(x->oldest), U64FromFullTransactionId(y->oldest));

	return c != 0 ? c : SIFT_CMP3(x->part, y->part);
}

static FullTransactionId
oldest_held(Relation rel, const SiftPartitionEntry *cand, FullTransactionId lowest)
{
	SiftPartitionEntry e;

	return sift_manifest_find_entry(rel, cand->part_id, cand->key_lo, &e) ? partition_oldest(rel, &e, lowest) : lowest;
}

/*
 * The partitions are settled the one that holds the oldest transaction first, each in a context of its own, with the budget of rewriting of the pass shared: after each, the oldest of what
 * the partitions still hold (the settled ones as they are now, the others as they were) is relfrozenxid, if that is newer than it was.
 */
static void
settle_all(Relation rel, SiftF2Store *store, const SiftManifestRead *mr, const struct VacuumCutoffs *cutoffs, FullTransactionId oldest_fx, FullTransactionId target_fx, bool aggressive,
		   BlockNumber nblocks, int elevel)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift table vacuum partition");
	MemoryContext old = MemoryContextSwitchTo(cx);
	Held	   *order = MemoryContextAlloc(old, sizeof(Held) * Max(mr->n, 1));
	FullTransactionId settled = oldest_fx,
				published = oldest_fx;
	const uint64 rewrite_budget = sift_vacuum_rewrite_budget(aggressive);
	uint64		rewrite_left = rewrite_budget;

	for (int i = 0; i < mr->n; i++)
	{
		order[i] = (Held) {i, oldest_held(rel, &mr->entries[i], oldest_fx)};
		MemoryContextReset(cx);
	}
	qsort(order, mr->n, sizeof(Held), by_held);
	if (mr->n > 0)
		published = order[0].oldest;
	for (int k = 0; k < mr->n; k++)
	{
		const SiftPartitionEntry *cand = &mr->entries[order[k].part];
		FullTransactionId rest = k + 1 < mr->n ? order[k + 1].oldest : oldest_fx;
		FullTransactionId after;

		sift_vacuum_delay_point();
		settle_partition(rel, store, cand, oldest_fx, target_fx, aggressive, &rewrite_left);
		pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, (int64) nblocks * (k + 1) / mr->n);
		after = oldest_held(rel, cand, oldest_fx);
		settled = FullTransactionIdPrecedes(after, settled) ? after : settled;
		MemoryContextReset(cx);
		if (FullTransactionIdPrecedes(published, FullTransactionIdPrecedes(settled, rest) ? settled : rest))
		{
			published = FullTransactionIdPrecedes(settled, rest) ? settled : rest;
			advance_frozenxid(rel, published, oldest_fx, cutoffs);
		}
	}
	if (rewrite_left == 0)
		sift_vacuum_say_deferred(elevel, rewrite_budget);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	pfree(order);
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
	VacCtx		vc = {.cx = CurrentMemoryContext,.rel = rel,.strategy = bstrategy};
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
	sift_horizon_vacuum_read_begin(rel);
	sift_stage_flush(rel);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	sift_f2_vacuum_progress_total(rel);
	vc.nblocks = RelationGetNumberOfBlocks(rel);
	for (int i = 0; i < mr.n; i++)
		vc.parts_total += (int) mr.entries[i].nparts;
	collect_all(&vc, store, &mr, &cutoffs, oldest_fx, aggressive);
	sift_f2_vacuum_progress_scanned(1, 1);
	flush_batch(&vc);
	if (vc.tids != NULL)
		drop_tids(&vc);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_VACUUM_HEAP);
	if (aggressive)
		sift_tbl_rowlock_clear_lockers(rel);
	cutoffs.OldestMxact = sift_tbl_rowlock_vacuum_mxid(rel, cutoffs.OldestMxact);
	settle_all(rel, store, &mr, &cutoffs, oldest_fx, target_fx, aggressive, vc.nblocks, elevel);
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
	(void) sift_proj_refresh(rel, store);
	if (store->nscopes > 0)
		(void) sift_f2_scope_refresh(rel);
	sift_f2_vacuum_progress_phase(PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);
	new_oldest = recompute_oldest(rel, oldest_fx);
	store_oldest_active(rel, new_oldest, oldest_fx);
	sift_tbl_vm_vacuum(rel, oldest_fx, freeze_fx);
	update_relstats(rel, new_oldest, &cutoffs);
	sift_horizon_vacuum_read_end(rel);
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
	sift_mem_trim_os();
}

/* ---- ANALYZE ---- */

bool
sift_tbl_f2_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	for (;;)
	{
		Buffer		buf = read_stream_next_buffer(stream, NULL);
		BlockNumber blk;

		if (!BufferIsValid(buf))
			return false;
		blk = BufferGetBlockNumber(buf);
		ReleaseBuffer(buf);
		if (sift_tbl_f2_scan_analyze_granules(scan, blk, Max(RelationGetNumberOfBlocks(scan->rs_rd), 1)))
			return true;
	}
}

bool
sift_tbl_f2_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows, TupleTableSlot *slot)
{
	if (!sift_tbl_f2_scan_analyze_next(scan, slot, deadrows))
		return false;
	(*liverows)++;
	return true;
}
