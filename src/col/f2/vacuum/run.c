/*
 * col/f2/vacuum/run.c - the maintenance that VACUUM, autovacuum and sift.col_maintain() ask of a sift_col index (spec 49 7.1).
 *
 * What a vacuum needs of the index before it can finish is metadata work: the stage is written, the matured extents are given back, the slabs that are due become parts, the parts
 * every inserter of which is settled are sealed. The rewriting of parts (compaction, a merge, the scope summaries, the tail of the file) is handed to the maintenance workers as
 * requests of SiftMaintShmem when an autovacuum worker runs it and there is one, so that a vacuum of the heap does not wait for it; a VACUUM someone runs, a server without the queue
 * and sift.col_maintain() do all of it here before they return, an autovacuum's within sift.vacuum_rewrite_limit.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/f2/journal.h"
#include "col/f2/compact/compact.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/f2.h"
#include "col/f2/del/slab.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/store/store.h"
#include "col/f2/tier/host.h"
#include "col/f2/vacuum/run.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "col/pg.h"
#include "core/compat.h"
#include "core/errctx.h"
#include "core/cachebudget.h"
#include "core/f2/pace.h"
#include "core/f2/progress.h"
#include "core/f2/fsm/fsm.h"
#include "core/horizon.h"
#include "core/maint/pacer.h"
#include "core/maint/sched.h"
#include "core/maint/vacuum.h"
#include "core/memtrack.h"
#include "core/sift.h"
#include "core/worker.h"
#include "core/xid_cache.h"
#include "table/f2/col_delta/col_delta.h"

/* Whether a part of the list may be sealed: read without the partition lock, which only the partitions that have one take */
static bool
has_unsealed(Relation rel, const SiftPartitionEntry *e)
{
	int			np;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);
	bool		any = false;

	for (int p = 0; p < np && !any; p++)
		any = (parts[p].flags & SIFT_PART_FLAG_UNFROZEN) && !(parts[p].flags & SIFT_PART_FLAG_ALL_VISIBLE);
	pfree(parts);
	return any;
}

/* Seals the parts of partition part_id (0: of every partition) that every inserter of committed (sift_part_try_seal): their descriptors change in the manifest, not a page of them */
static void
seal_parts(Relation rel, FullTransactionId horizon, uint64 part_id)
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
		if ((part_id != 0 && mr.entries[i].part_id != part_id) || !has_unsealed(rel, &mr.entries[i]))
			continue;
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
static void
settle_due(Relation rel, const SiftPartSpec *spec, FullTransactionId horizon)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];

		sift_vacuum_delay_point();
		if (e->l0_slab_blk != InvalidBlockNumber && sift_l0_href_flush_due(rel, e->l0_slab_blk, horizon))
			(void) sift_l0_slab_flush(rel, e->part_id, e->key_lo, spec);
		(void) sift_dv_consolidate(rel, e->part_id, e->key_lo);
	}
}

/* The groups the plan makes for a partition: with the parts its column-delta chain has records for, when the chain is long enough to fold (SIFT_COL_DELTA_FOLD_PAGES) or the VACUUM someone runs leaves none */
static SiftCompactGroup *
plan_partition(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *parts, int np, FullTransactionId horizon, int64 hot_cut, bool fold_short, int *ngroups)
{
	uint64	   *folds = palloc(Max(np, 1) * sizeof(uint64));
	SiftCompactChain chain = {.fold_gens = folds};

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
	{
		chain.nfold = sift_col_delta_fold_gens(rel, e, parts, np, horizon, fold_short, folds, 0);
		chain.dirty_time = sift_manifest_read_col_delta(rel, e).flags & (SIFT_CDLA_DIRTY_TIME0 | SIFT_CDLA_DIRTY_TIME1);
	}
	return sift_compact_plan_chained(parts, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), hot_cut, &chain, ngroups);
}

/* Whether the plan of the partition, as the groups it wrote left it, has a group: what a worker that wrote asks a second step for */
static bool
replan_due(Relation rel, const SiftPartitionEntry *e, FullTransactionId horizon, int64 hot_cut)
{
	SiftPartitionEntry fresh;
	SiftPartDesc *parts;
	int			np,
				ngroups;

	if (!sift_manifest_find_entry(rel, e->part_id, e->key_lo, &fresh))
		return false;
	parts = sift_manifest_read_parts(rel, &fresh, &np);
	(void) plan_partition(rel, &fresh, parts, np, horizon, hot_cut, false, &ngroups);
	pfree(parts);
	return ngroups > 0;
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
	SiftCompactGroup *groups = plan_partition(rel, e, parts, np, horizon, hot_cut, !paced && !AmAutoVacuumWorkerProcess(), &ngroups);
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
	return r == SIFT_MSTEP_DONE && paced && *written > before && replan_due(rel, e, horizon, hot_cut) ? SIFT_MSTEP_MORE : r;
}

static void
refresh_projections(Relation rel)
{
	(void) sift_proj_refresh(rel, sift_f2_store_open(rel));
}

/* A base part the relocation rewrote has a generation its projection parts are not linked to: they are linked again here, not by the next maintenance */
static void
relocate_tail(Relation rel, FullTransactionId horizon, int64 hot_cut)
{
	sift_f2_tail_relocate(rel, sift_f2_store_open(rel), horizon, hot_cut);
	refresh_projections(rel);
}

/* The blocks a compaction freed wait in the quarantine for the readers of the manifest it replaced: the first cut frees them, so that the holders at the end of the file have room to move into */
static SiftTailShrinkResult
shrink_tail(Relation rel, FullTransactionId horizon, int64 hot_cut)
{
	(void) sift_fsm_tail_shrink(rel);
	relocate_tail(rel, horizon, hot_cut);
	return sift_fsm_tail_shrink(rel);
}

/*
 * What the transaction asks of the queue is asked when it has committed: a worker that starts at once finds the rows its statement deleted or wrote still in doubt, and its step does nothing but
 * come back later. What the transaction that aborts asked is not asked.
 */
static List *asked;
static HTAB *asked_set;
static MemoryContext asked_cx;

typedef struct AskedKey
{
	Oid			rel;
	int32		kind;
	uint64		part_id;
} AskedKey;

static void
ask_queue(XactEvent event, void *arg pg_attribute_unused())
{
	if (asked == NIL || (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE))
		return;
	if (event == XACT_EVENT_COMMIT)
		foreach_ptr(SiftMaintReq, req, asked)
			(void) sift_maint_enqueue_here(req);
	MemoryContextReset(asked_cx);
	asked = NIL;
	asked_set = NULL;
}

bool
sift_f2_maint_request(Relation rel, SiftMaintKind kind, uint64 part_id)
{
	static bool registered;
	SiftMaintReq req = {.db = MyDatabaseId,.rel = RelationGetRelid(rel),.kind = kind,.tier = SIFT_MAINT_TIER_DEFAULT,.part_id = part_id};
	MemoryContext old;
	bool		found;

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP || RecoveryInProgress() || IsParallelWorker() || !sift_maint_here_available())
		return false;
	if (!registered)
	{
		RegisterXactCallback(ask_queue, NULL);
		registered = true;
	}
	if (asked_cx == NULL)
		asked_cx = sift_cache_create_context("sift cache maintenance requests");
	if (asked_set == NULL)
		asked_set = hash_create("sift maintenance requests asked", 16, &(HASHCTL) {.keysize = sizeof(AskedKey),.entrysize = sizeof(AskedKey),.hcxt = asked_cx},
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	(void) hash_search(asked_set, &(AskedKey) {req.rel, kind, part_id}, HASH_ENTER, &found);
	if (found)
		return true;
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
	if (!sift_async_maintenance || !AmAutoVacuumWorkerProcess())
		return false;
	for (int i = 0; i < mr->n; i++)
	{
		const SiftPartitionEntry *e = &mr->entries[i];
		int			np,
					ngroups;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);

		(void) plan_partition(rel, e, parts, np, horizon, hot_cut, false, &ngroups);
		if (e->l0_slab_blk != InvalidBlockNumber && !sift_f2_maint_request(rel, SIFT_MK_SLAB_FLUSH, e->part_id))
			return false;
		if (ngroups > 0 && !sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, e->part_id))
			return false;
	}
	return sift_f2_maint_request(rel, SIFT_MK_MERGE_PART, 0) && (!sift_f2_scoped(rel) || sift_f2_maint_request(rel, SIFT_MK_SCOPE_REFRESH, 0)) &&
		sift_f2_maint_request(rel, SIFT_MK_TAIL_SHRINK, 0);
}

/* Requests of two partitions coalesce into one of part_id 0, the whole relation (core/maint/sched.h) */
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
	FullTransactionId horizon = sift_standby_safe_horizon(rel, false);
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
			for (int i = 0; i < mr.n && r != SIFT_MSTEP_MORE; i++)
				if (claimed(claim, &mr.entries[i]))
					r = later_of(r, compact_partition(rel, spec, &mr.entries[i], horizon, hot_cut, cx, UINT64_MAX, &written, true));
			if (written > 0)
			{
				seal_parts(rel, horizon, claim->part_id);
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
	FullTransactionId horizon = sift_standby_safe_horizon(rel, false);
	int64		hot_cut = sift_f2_hot_cut(f);
	SiftManifestRead mr;
	SiftColMetaPageData meta;
	SiftErrCtx	ctx;
	SiftTailShrinkResult shrink = {0};
	SiftF2Trace tr;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compaction");
	uint64		written = 0,
				budget = force_sync_full ? UINT64_MAX : sift_vacuum_rewrite_budget(false);
	bool		within,
				here;

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
	if (force_sync_full)
		(void) sift_f2_settled_manifest(rel, &f->spec);
	else
		settle_due(rel, &f->spec, horizon);
	sift_horizon_vacuum_read_end(rel);
	(void) sift_fsm_reclaim(rel);
	sift_horizon_vacuum_read_begin(rel);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	sift_f2_trace_phase(&tr, "seal");
	seal_parts(rel, horizon, 0);
	sift_f2_tier_maintain(rel, sift_f2_store_open(rel), horizon);
	here = force_sync_full || !hand_off(rel, f, &mr, horizon, hot_cut);
	if (here)
	{
		sift_f2_trace_phase(&tr, "compact");
		for (int i = 0; i < mr.n; i++)
			if (compact_partition(rel, &f->spec, &mr.entries[i], horizon, hot_cut, cx, budget, &written, false) == SIFT_MSTEP_MORE)
				break;
		sift_mem_release_os(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
		within = written < budget;
		if (within)
		{
			(void) sift_compact_merge_neighbours(rel);
			relocate_tail(rel, horizon, hot_cut);
		}
		else
			sift_vacuum_say_deferred(info->message_level, written);
		if (sift_f2_scoped(rel))
		{
			sift_f2_trace_phase(&tr, "scope");
			(void) sift_f2_scope_refresh(rel);
		}
		if (within)
			(void) sift_compact_pack_tails(rel);
	}
	sift_horizon_vacuum_read_end(rel);
	if (here)
	{
		sift_f2_trace_phase(&tr, "shrink");
		shrink = sift_fsm_tail_shrink(rel);
	}
	MemoryContextDelete(cx);
	(void) sift_col_open_meta(rel, &meta);
	stats->num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_index_tuples = meta.total_rows - meta.total_dead;
	sift_errctx_pop(&ctx);
	sift_f2_say_vacuum(&tr, info->message_level, stats->tuples_removed, shrink.lock_skipped);
	sift_f2_trace_end(&tr);
	return stats;
}
