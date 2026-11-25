/*
 * col/f2/f2_vacuum_run.c - the maintenance that VACUUM, autovacuum and sift.col_maintain() ask of a sift_col index (spec 49 7.1).
 *
 * What a vacuum needs of the index before it can finish is metadata work: the stage is written, the matured extents are given back, the slabs that are due become parts, the parts
 * every inserter of which is settled are sealed. The rewriting of parts (compaction, a merge, the scope summaries, the tail of the file) is handed to the maintenance workers as
 * requests of SiftMaintShmem when there is one, so that a vacuum of the heap does not wait for it; without one, or for sift.col_maintain(), it is done here, a vacuum's within
 * sift.vacuum_rewrite_limit.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/f2/f2_journal.h"
#include "col/f2/compact.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/f2.h"
#include "col/f2/f2_del_slab.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_tier_host.h"
#include "col/f2/f2_vacuum_run.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "col/pg.h"
#include "core/compat.h"
#include "core/errctx.h"
#include "core/cachebudget.h"
#include "core/f2/f2_pace.h"
#include "core/f2/f2_progress.h"
#include "core/f2/fsm.h"
#include "core/horizon.h"
#include "core/maint_pacer.h"
#include "core/maint_sched.h"
#include "core/memtrack.h"
#include "core/sift.h"
#include "core/worker.h"
#include "core/xid_cache.h"
#include "table/f2/f2_col_delta.h"

/* Seals the parts that every inserter of committed (sift_part_try_seal): their descriptors change in the manifest, not a page of them */
static void
seal_parts(Relation rel, FullTransactionId horizon)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	SiftXidHorizonCache hc;

	sift_xid_horizon_init(&hc, horizon);
	for (int i = 0; i < mr.n; i++)
	{
		SiftPartitionEntry e;
		int			np,
					nsealed = 0;
		SiftPartDesc *parts,
				   *sealed,
				   *expect;

		sift_vacuum_delay_point();
		LockPage(rel, SIFT_PART_LOCK_BLKNO(mr.entries[i].part_id), ExclusiveLock);
		if (sift_manifest_find_entry(rel, mr.entries[i].part_id, mr.entries[i].key_lo, &e))
		{
			parts = sift_manifest_read_parts(rel, &e, &np);
			sealed = palloc(Max(np, 1) * sizeof(SiftPartDesc));
			expect = palloc(Max(np, 1) * sizeof(SiftPartDesc));
			for (int p = 0; p < np; p++)
			{
				SiftPartDesc d = parts[p];

				if (sift_part_try_seal(rel, (uint32) e.part_id, &d, &hc))
				{
					expect[nsealed] = parts[p];
					sealed[nsealed++] = d;
				}
			}
			if (nsealed > 0)
				sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = e.part_id,.key_lo = e.key_lo,.update_parts = sealed,.expect_parts = expect,.nupdate = nsealed});
			pfree(parts);
			pfree(sealed);
			pfree(expect);
		}
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(mr.entries[i].part_id), ExclusiveLock);
	}
}

/* The slabs that are due (f2_del_slab.h) become parts, the deletion vectors are consolidated; a young slab of references stays for the next row that rewrites one of its rows */
static SiftManifestRead
settle_due(Relation rel, const SiftPartSpec *spec, FullTransactionId horizon)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	bool		changed = false;

	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];

		sift_vacuum_delay_point();
		if (e->l0_slab_blk != InvalidBlockNumber && sift_l0_href_flush_due(rel, e->l0_slab_blk, horizon))
			changed |= sift_l0_slab_flush(rel, e->part_id, e->key_lo, spec);
		changed |= sift_dv_consolidate(rel, e->part_id, e->key_lo) > 0;
	}
	return changed ? sift_manifest_read_range(rel, NULL, NULL) : mr;
}

static uint64
rewrite_budget(void)
{
	return sift_vacuum_rewrite_limit > 0 ? (uint64) sift_vacuum_rewrite_limit << 20 : UINT64_MAX;
}

/* The groups the plan makes for a partition: with the parts its column-delta chain has records for, when the chain is long enough to fold (SIFT_COL_DELTA_FOLD_PAGES) */
static SiftCompactGroup *
plan_partition(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *parts, int np, FullTransactionId horizon, int64 hot_cut, int *ngroups)
{
	uint64	   *folds = palloc(Max(np, 1) * sizeof(uint64));
	SiftCompactChain chain = {.fold_gens = folds};

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
	{
		chain.nfold = sift_col_delta_fold_gens(rel, e, parts, np, horizon, false, folds, 0);
		chain.dirty_time = sift_manifest_read_col_delta(rel, e).flags & (SIFT_CDLA_DIRTY_TIME0 | SIFT_CDLA_DIRTY_TIME1);
	}
	return sift_compact_plan_chained(parts, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), hot_cut, &chain, ngroups);
}

/* A file whose free blocks (the matured ones reclaimed) are enough to cut: what sift_fsm_tail_shrink asks before it takes the lock */
static bool
tail_worth_cutting(Relation rel)
{
	SiftFsmStats st;
	uint64		free_blocks;

	(void) sift_fsm_reclaim(rel);
	st = sift_fsm_check(rel);
	free_blocks = st.quarantine_blocks + st.meta_idle;
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		free_blocks += st.class_blocks[k];
	return free_blocks >= SIFT_TAIL_SHRINK_MIN_BLOCKS && free_blocks * 100 >= (uint64) sift_col_meta_read(rel).high_water * SIFT_TAIL_SHRINK_FREE_PCT;
}

/*
 * The lock of the cut is tried before sift_fsm_tail_shrink is, so that a file that is read leaves the metapage alone: the cut that finds it busy writes nothing and is asked again. With the
 * lock held no reader of an older manifest is there, which frees the quarantine at once
 */
static SiftTailShrinkResult
cut_tail(Relation rel)
{
	SiftTailShrinkResult shrink = {0};

	if (!tail_worth_cutting(rel))
		return shrink;
	if (!ConditionalLockRelation(rel, AccessExclusiveLock))
	{
		shrink.lock_skipped = true;
		return shrink;
	}
	shrink = sift_fsm_tail_shrink(rel);
	UnlockRelation(rel, AccessExclusiveLock);
	return shrink;
}

/*
 * The groups of one partition, compacted in the order of the plan while the bytes written stay under budget (and, in a worker, while the pacer does not ask it to yield); the bytes written are
 * added to *written. MORE: the budget or the pacer stopped it with groups left, or (in a worker) the groups it wrote leave parts to be planned again, as the plan makes one group of a tier, as
 * many parts as the budget holds; DEFERRED: a group found a deletion that is not settled, or the partition under another compaction, and is planned again by a later step.
 */
static SiftMaintStep
compact_partition(Relation rel, const SiftPartSpec *spec, const SiftPartitionEntry *e, FullTransactionId horizon, int64 hot_cut, MemoryContext cx, uint64 budget, uint64 *written, bool paced)
{
	int			np,
				ngroups;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);
	SiftCompactGroup *groups = plan_partition(rel, e, parts, np, horizon, hot_cut, &ngroups);
	SiftMaintStep r = SIFT_MSTEP_DONE;
	uint64		before = *written;

	for (int k = 0; k < ngroups; k++)
	{
		MemoryContext old = MemoryContextSwitchTo(cx);
		SiftCompactResult res;

		if (*written >= budget || (paced && sift_pacer_should_yield(sift_maint_kind_tier(SIFT_MK_COMPACT_PART), GetCurrentTimestamp())))
		{
			MemoryContextSwitchTo(old);
			return SIFT_MSTEP_MORE;
		}
		sift_vacuum_delay_point();
		if (!(groups[k].join ? sift_partition_join_online : sift_partition_compact_online) (rel, e->part_id, e->key_lo, groups[k].gens, groups[k].n, spec, hot_cut, horizon, &res))
			r = SIFT_MSTEP_DEFERRED;
		*written += res.bytes_out;
		sift_f2_trace_step(1, res.bytes_out);
		MemoryContextSwitchTo(old);
		MemoryContextReset(cx);
		sift_f2_pace_pay();
	}
	return r == SIFT_MSTEP_DONE && paced && *written > before ? SIFT_MSTEP_MORE : r;
}

static void
refresh_projections(Relation rel)
{
	(void) sift_proj_refresh(rel, sift_f2_store_open(rel));
}

/* The blocks a compaction freed wait in the quarantine for the readers of the manifest it replaced: the first cut frees them, so that the holders at the end of the file have room to move into */
static SiftTailShrinkResult
shrink_tail(Relation rel, FullTransactionId horizon, int64 hot_cut)
{
	(void) cut_tail(rel);
	sift_f2_tail_relocate(rel, sift_f2_store_open(rel), horizon, hot_cut);
	return cut_tail(rel);
}

/*
 * What the transaction asks of the queue is asked when it has committed: a worker that starts at once finds the rows its statement deleted or wrote still in doubt, and its step does nothing but
 * come back later. What the transaction that aborts asked is not asked.
 */
static List *asked;
static MemoryContext asked_cx;

static void
ask_queue(XactEvent event, void *arg pg_attribute_unused())
{
	if (asked == NIL || (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE))
		return;
	if (event == XACT_EVENT_COMMIT)
		foreach_ptr(SiftMaintReq, req, asked)
			(void) sift_maint_enqueue_here(req);
	list_free_deep(asked);
	asked = NIL;
}

bool
sift_f2_maint_request(Relation rel, SiftMaintKind kind, uint64 part_id)
{
	static bool registered;
	SiftMaintReq req = {.db = MyDatabaseId,.rel = RelationGetRelid(rel),.kind = kind,.tier = SIFT_MAINT_TIER_DEFAULT,.part_id = part_id};
	MemoryContext old;

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP || RecoveryInProgress() || IsParallelWorker() || !sift_maint_here_available())
		return false;
	if (!registered)
	{
		RegisterXactCallback(ask_queue, NULL);
		registered = true;
	}
	foreach_ptr(SiftMaintReq, again, asked)
		if (again->rel == req.rel && again->kind == kind && again->part_id == part_id)
			return true;
	if (asked_cx == NULL)
		asked_cx = sift_cache_create_context("sift cache maintenance requests");
	old = MemoryContextSwitchTo(asked_cx);
	asked = lappend(asked, memcpy(palloc(sizeof(req)), &req, sizeof(req)));
	MemoryContextSwitchTo(old);
	return true;
}

/* Whether the rewriting of the partition is due by what its entry says: level-0 parts to merge, dead rows at the share that is rewritten, or a column-delta chain to fold */
static bool
compaction_due(Relation rel, const SiftPartitionEntry *e)
{
	if (e->l0_count >= SIFT_COMPACT_L0_FANOUT)
		return true;
	if (e->total_rows > 0 && e->total_dead * 100 >= e->total_rows * (uint64) sift_dead_rewrite_pct)
		return true;
	return (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 && sift_manifest_read_col_delta(rel, e).npages >= SIFT_COL_DELTA_FOLD_PAGES;
}

void
sift_f2_maint_note_parts(Relation rel, const SiftPartitionEntry *entries, int n, bool (*wanted) (const SiftPartitionEntry *e, void *arg), void *arg)
{
	for (int i = 0; i < n; i++)
		if (compaction_due(rel, &entries[i]) && (wanted == NULL || wanted(&entries[i], arg)))
			(void) sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, entries[i].part_id);
}

/* Requests what the rewriting of the index needs; false when the queue is not there (nothing is left half asked: the caller does it) */
static bool
hand_off(Relation rel, const SiftF2Rel *f, const SiftManifestRead *mr, FullTransactionId horizon, int64 hot_cut)
{
	if (!sift_async_maintenance)
		return false;
	for (int i = 0; i < mr->n; i++)
	{
		const SiftPartitionEntry *e = &mr->entries[i];
		int			np,
					ngroups;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);

		(void) plan_partition(rel, e, parts, np, horizon, hot_cut, &ngroups);
		if (e->l0_slab_blk != InvalidBlockNumber && !sift_f2_maint_request(rel, SIFT_MK_SLAB_FLUSH, e->part_id))
			return false;
		if (ngroups > 0 && !sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, e->part_id))
			return false;
	}
	return sift_f2_maint_request(rel, SIFT_MK_MERGE_PART, 0) && (!sift_f2_scoped(rel) || sift_f2_maint_request(rel, SIFT_MK_SCOPE_REFRESH, 0)) &&
		sift_f2_maint_request(rel, SIFT_MK_TAIL_SHRINK, 0);
}

/* Requests of two partitions coalesce into one of part_id 0, the whole relation (core/maint_sched.h) */
static bool
claimed(const SiftMaintClaim *claim, const SiftPartitionEntry *e)
{
	return claim->part_id == 0 || e->part_id == claim->part_id;
}

static bool
claimed_wanted(const SiftPartitionEntry *e, void *claim)
{
	return claimed(claim, e);
}

/* What a compaction that wrote leaves to do: the scope summaries, the neighbours that are small now and the file that ended shorter */
static void
chain_after_compaction(Relation rel, int npartitions)
{
	if (npartitions > 1 || sift_proj_relation_has(rel))
		(void) sift_f2_maint_request(rel, SIFT_MK_MERGE_PART, 0);
	if (sift_f2_scoped(rel))
		(void) sift_f2_maint_request(rel, SIFT_MK_SCOPE_REFRESH, 0);
	(void) sift_f2_maint_request(rel, SIFT_MK_TAIL_SHRINK, 0);
}

/* The step that must come again (the budget or the pacer stopped it) before the one that waits for a quieter moment, before the one that is done */
static SiftMaintStep
later_of(SiftMaintStep a, SiftMaintStep b)
{
	return a == SIFT_MSTEP_MORE || b == SIFT_MSTEP_MORE ? SIFT_MSTEP_MORE : a == SIFT_MSTEP_DEFERRED ? a : b;
}

/* One request of a worker for a sift_col index: the kinds it is asked, one partition or one pass of the relation for each */
SiftMaintStep
sift_f2_maint_step(Relation rel, const SiftMaintClaim *claim)
{
	bool		index = rel->rd_rel->relkind == RELKIND_INDEX;
	const SiftPartSpec *spec = index ? &sift_f2_rel(rel)->spec : &sift_f2_store_open(rel)->spec;
	FullTransactionId horizon = sift_horizon(rel);
	int64		hot_cut = index ? sift_f2_hot_cut(sift_f2_rel(rel)) : PG_INT64_MIN;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compaction");
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	SiftTailShrinkResult shrink;
	uint64		written = 0;
	SiftMaintStep r = SIFT_MSTEP_DONE;

	switch (claim->kind)
	{
		case SIFT_MK_SLAB_FLUSH:
			for (int i = 0; i < mr.n; i++)
				if (claimed(claim, &mr.entries[i]) && mr.entries[i].l0_slab_blk != InvalidBlockNumber)
					(void) sift_l0_slab_flush(rel, mr.entries[i].part_id, mr.entries[i].key_lo, spec);
			break;
		case SIFT_MK_PEND_DEL_FOLD:
			for (int i = 0; i < mr.n; i++)
				if (claimed(claim, &mr.entries[i]) && mr.entries[i].pend_del_blk != InvalidBlockNumber)
					(void) sift_del_bulk(rel, mr.entries[i].part_id, mr.entries[i].key_lo, NULL, 0, true, spec);
			mr = sift_manifest_read_range(rel, NULL, NULL);
			sift_f2_maint_note_parts(rel, mr.entries, mr.n, claimed_wanted, (void *) claim);
			break;
		case SIFT_MK_COMPACT_PART:
			for (int i = 0; i < mr.n; i++)
				if (claimed(claim, &mr.entries[i]))
					r = later_of(r, compact_partition(rel, spec, &mr.entries[i], horizon, hot_cut, cx, UINT64_MAX, &written, true));
			if (written > 0)
			{
				seal_parts(rel, horizon);
				chain_after_compaction(rel, mr.n);
			}
			break;
		case SIFT_MK_MERGE_PART:
			(void) sift_compact_merge_neighbours(rel);
			refresh_projections(rel);
			(void) sift_compact_pack_tails(rel);
			break;
		case SIFT_MK_SCOPE_REFRESH:
			(void) sift_f2_scope_refresh(rel);
			break;
		case SIFT_MK_TAIL_SHRINK:
			if (index || sift_f2_store_open(rel)->opts.vacuum_truncate)
			{
				shrink = shrink_tail(rel, horizon, hot_cut);
				r = shrink.lock_skipped ? SIFT_MSTEP_BUSY : SIFT_MSTEP_DONE;
			}
			break;
		default:
			break;
	}
	MemoryContextDelete(cx);
	sift_mem_release_os(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	return r;
}

IndexBulkDeleteResult *
sift_f2_vacuum_run(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, bool force_sync_full)
{
	Relation	rel = info->index;
	SiftF2Rel  *f = sift_f2_rel(rel);
	FullTransactionId horizon = sift_horizon(rel);
	int64		hot_cut = sift_f2_hot_cut(f);
	SiftManifestRead mr;
	SiftColMetaPageData meta;
	SiftErrCtx	ctx;
	SiftTailShrinkResult shrink = {0};
	SiftF2Trace tr;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compaction");
	uint64		written = 0,
				budget = force_sync_full ? UINT64_MAX : rewrite_budget();

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	if (info->analyze_only)
		return stats;
	if (!sift_worker_paced())
		VacuumUpdateCosts();
	sift_f2_trace_begin(&tr, rel, SIFT_JOB_VACUUM, info->message_level == INFO);
	sift_errctx_push(&ctx, RelationGetRelid(rel));
	sift_horizon_vacuum_read_begin(rel);
	sift_stage_flush(rel);
	sift_f2_journal_trim(rel);
	sift_f2_trace_phase(&tr, "settle");
	mr = force_sync_full ? sift_f2_settled_manifest(rel, &f->spec) : settle_due(rel, &f->spec, horizon);
	(void) sift_fsm_reclaim(rel);
	sift_f2_trace_phase(&tr, "seal");
	seal_parts(rel, horizon);
	sift_f2_tier_maintain(rel, sift_f2_store_open(rel), horizon);
	if (force_sync_full || !hand_off(rel, f, &mr, horizon, hot_cut))
	{
		sift_f2_trace_phase(&tr, "compact");
		for (int i = 0; i < mr.n; i++)
			if (compact_partition(rel, &f->spec, &mr.entries[i], horizon, hot_cut, cx, budget, &written, false) == SIFT_MSTEP_MORE)
				break;
		sift_mem_release_os(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
		if (written < budget)
		{
			(void) sift_compact_merge_neighbours(rel);
			refresh_projections(rel);
			sift_f2_tail_relocate(rel, sift_f2_store_open(rel), horizon, hot_cut);
			if (sift_f2_scoped(rel))
			{
				sift_f2_trace_phase(&tr, "scope");
				(void) sift_f2_scope_refresh(rel);
			}
			(void) sift_compact_pack_tails(rel);
			sift_f2_trace_phase(&tr, "shrink");
			shrink = sift_fsm_tail_shrink(rel);
		}
	}
	MemoryContextDelete(cx);
	(void) sift_col_open_meta(rel, &meta);
	stats->num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_index_tuples = meta.total_rows - meta.total_dead;
	sift_horizon_vacuum_read_end(rel);
	sift_errctx_pop(&ctx);
	sift_f2_say_vacuum(&tr, info->message_level, stats->tuples_removed, shrink.lock_skipped);
	sift_f2_trace_end(&tr);
	return stats;
}
