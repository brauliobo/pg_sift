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
#include "core/errctx.h"
#include "core/f2/f2_pace.h"
#include "core/horizon.h"
#include "core/f2/f2_progress.h"
#include "core/f2/fsm.h"
#include "core/maint_sched.h"
#include "core/memtrack.h"
#include "core/sift.h"
#include "core/xid_cache.h"

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

/* The groups of one partition, compacted in the order of the plan while the bytes written stay under budget (and, in a worker, while the pacer does not ask it to yield); the bytes written are added to *written */
static bool
compact_partition(Relation rel, const SiftPartSpec *spec, const SiftPartitionEntry *e, FullTransactionId horizon, int64 hot_cut, MemoryContext cx, uint64 budget, uint64 *written, bool paced)
{
	int			np,
				ngroups;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);
	SiftCompactGroup *groups = sift_compact_plan(parts, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), hot_cut, &ngroups);

	for (int k = 0; k < ngroups; k++)
	{
		MemoryContext old = MemoryContextSwitchTo(cx);
		SiftCompactResult res;

		if (*written >= budget || (paced && sift_pacer_should_yield(sift_maint_kind_tier(SIFT_MK_COMPACT_PART), GetCurrentTimestamp())))
		{
			MemoryContextSwitchTo(old);
			return false;
		}
		(void) sift_partition_compact_online(rel, e->part_id, e->key_lo, groups[k].gens, groups[k].n, spec, hot_cut, horizon, &res);
		*written += res.bytes_out;
		sift_f2_trace_step(1, res.bytes_out);
		MemoryContextSwitchTo(old);
		MemoryContextReset(cx);
	}
	return true;
}

static void
refresh_projections(Relation rel)
{
	if (sift_proj_relation_has(rel))
		(void) sift_proj_refresh(rel, sift_f2_store_open(rel));
}

static void
shrink_tail(Relation rel, FullTransactionId horizon, int64 hot_cut, SiftTailShrinkResult *shrink)
{
	sift_f2_tail_relocate(rel, sift_f2_store_open(rel), horizon, hot_cut);
	*shrink = sift_fsm_tail_shrink(rel);
}

bool
sift_f2_maint_request(Relation rel, SiftMaintKind kind, uint64 part_id)
{
	SiftMaintReq req = {.db = MyDatabaseId,.rel = RelationGetRelid(rel),.kind = kind,.tier = SIFT_MAINT_TIER_DEFAULT,.part_id = part_id};

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP || RecoveryInProgress() || IsParallelWorker())
		return false;
	return sift_maint_enqueue_here(&req) != SIFT_ENQ_UNAVAILABLE;
}

void
sift_f2_maint_note_parts(Relation rel, const SiftPartitionEntry *entries, int n, bool (*wanted) (const SiftPartitionEntry *e, void *arg), void *arg)
{
	for (int i = 0; i < n; i++)
		if (entries[i].l0_count >= SIFT_COMPACT_L0_FANOUT && (wanted == NULL || wanted(&entries[i], arg)))
			(void) sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, entries[i].part_id);
}

/* Requests what the rewriting of the index needs; false when the queue is not there (nothing is left half asked: the caller does it) */
static bool
hand_off(Relation rel, const SiftF2Rel *f, const SiftManifestRead *mr, int64 hot_cut)
{
	if (!sift_async_maintenance)
		return false;
	for (int i = 0; i < mr->n; i++)
	{
		const SiftPartitionEntry *e = &mr->entries[i];
		int			np,
					ngroups;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);

		(void) sift_compact_plan(parts, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), hot_cut, &ngroups);
		if (e->l0_slab_blk != InvalidBlockNumber && !sift_f2_maint_request(rel, SIFT_MK_SLAB_FLUSH, e->part_id))
			return false;
		if ((ngroups > 0 || (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0) && !sift_f2_maint_request(rel, SIFT_MK_COMPACT_PART, e->part_id))
			return false;
	}
	return sift_f2_maint_request(rel, SIFT_MK_MERGE_PART, 0) && (!sift_f2_scoped(rel) || sift_f2_maint_request(rel, SIFT_MK_SCOPE_REFRESH, 0)) &&
		sift_f2_maint_request(rel, SIFT_MK_TAIL_SHRINK, 0);
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
				if (mr.entries[i].part_id == claim->part_id && mr.entries[i].l0_slab_blk != InvalidBlockNumber)
					(void) sift_l0_slab_flush(rel, mr.entries[i].part_id, mr.entries[i].key_lo, spec);
			break;
		case SIFT_MK_COMPACT_PART:
			for (int i = 0; i < mr.n; i++)
				if (mr.entries[i].part_id == claim->part_id && !compact_partition(rel, spec, &mr.entries[i], horizon, hot_cut, cx, UINT64_MAX, &written, true))
					r = SIFT_MSTEP_MORE;
			seal_parts(rel, horizon);
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
				shrink_tail(rel, horizon, hot_cut, &shrink);
				r = shrink.lock_skipped ? SIFT_MSTEP_BUSY : SIFT_MSTEP_DONE;
			}
			break;
		default:
			break;
	}
	MemoryContextDelete(cx);
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
	if (force_sync_full || !hand_off(rel, f, &mr, hot_cut))
	{
		sift_f2_trace_phase(&tr, "compact");
		for (int i = 0; i < mr.n; i++)
			if (!compact_partition(rel, &f->spec, &mr.entries[i], horizon, hot_cut, cx, budget, &written, false))
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
