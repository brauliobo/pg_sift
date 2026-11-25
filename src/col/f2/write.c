/*
 * col/f2/write.c - the writes of an index of format major 2 after its build: aminsert stages the row in the transaction (col/l0_stage.h), ambulkdelete finds the TIDs VACUUM says are
 * dead and puts them in the deletion vectors (col/delete.h), amvacuumcleanup compacts the partitions and gives the matured extents back (col/compact.h, core/fsm.h).
 */
#include "postgres.h"

#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/f2/f2.h"
#include "col/f2/col_delta/idx.h"
#include "col/f2/journal.h"
#include "col/f2/uniq/unique.h"
#include "col/f2/vacuum/vm.h"
#include "col/f2/heap_probe.h"
#include "col/f2/vacuum/run.h"
#include "col/f2/compact/compact.h"
#include "col/f2/del/delete.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/store/store.h"
#include "col/f2/tier/host.h"
#include "col/f2/l0/href_fetch.h"
#include "col/f2/l0/slab.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "core/errctx.h"
#include "core/horizon.h"
#include "core/visibility/visibility.h"
#include "core/f2/progress.h"
#include "core/memtrack.h"
#include "core/f2/fsm/fsm.h"
#include "core/page_io.h"
#include "col/scope/scope.h"
#include "col/f2/col_delta/idx.h"
#include "core/xid_cache.h"


void
sift_f2_stage_row(Relation index, const Datum *values, const bool *isnull, ItemPointer heap_tid)
{
	SiftF2Rel  *f = sift_f2_rel(index);
	MemoryContext rowcxt = sift_col_rel_rowcxt(sift_col_rel(index));
	SiftCell   *cells = palloc0(f->ncols * sizeof(SiftCell));
	SiftCell  **elem = palloc0(f->ncols * sizeof(SiftCell *));
	uint8		key[SIFT_BOUND_BYTES];
	SiftTxnStage *st = sift_stage_get(index, &f->spec);

	sift_f2_row(index, f, rowcxt, heap_tid, values, isnull, cells, elem, key);
	sift_stage_add(index, st, key, sift_f2_tid(heap_tid), GetCurrentTransactionId(), GetCurrentCommandId(true), cells, (const SiftCell *const *) elem);
	MemoryContextReset(rowcxt);
	pfree(elem);
	pfree(cells);
}

bool
sift_f2_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	sift_f2_journal_added(index);
	if (indexUnchanged)
		sift_f2_track_note_unchanged(index, heap_tid);
	if (checkUnique != UNIQUE_CHECK_NO)
		return sift_f2_unique_insert(index, heap, values, isnull, heap_tid, checkUnique, indexInfo);
	sift_f2_stage_row(index, values, isnull, heap_tid);
	return false;
}

SiftManifestRead
sift_f2_settled_manifest(Relation rel, const SiftPartSpec *spec)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	bool		changed = false;

	for (int i = 0; i < mr.n; i++)
	{
		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber)
			changed |= sift_l0_slab_flush(rel, mr.entries[i].part_id, mr.entries[i].key_lo, spec);
		changed |= sift_dv_consolidate(rel, mr.entries[i].part_id, mr.entries[i].key_lo) > 0;
	}
	return changed ? sift_manifest_read_range(rel, NULL, NULL) : mr;
}

static uint16 *
granule_rows(const SiftGranuleDesc *dir, uint32 n)
{
	uint16	   *rows = palloc(Max(n, 1) * sizeof(uint16));

	for (uint32 g = 0; g < n; g++)
		rows[g] = dir[g].nrows;
	return rows;
}

/* Collects the dead rows the callback of VACUUM names, in the list of a partition in cx; the list leaves for the partition's parts when it holds a quarter of the maintenance budget */
typedef struct DeadList
{
	SiftPendDelRec *recs;
	int			n;
	int			cap;
	int			max;
	MemoryContext cx;
	Relation	rel;
	const SiftPartitionEntry *entry;
	const SiftPartSpec *spec;
	IndexBulkDeleteResult *stats;
	const SiftVmDirtySet *vm;	/* the heap blocks that are not all-visible: the rows in the others have no dead tuple to offer */
	const SiftCdRemapMap *rm;	/* the rows that moved to another address in the column delta chain: the address offered is the one they have now */
	SiftHeapProbe *probe;		/* the validation of a concurrent build: the rows the heap has no live version for are dead too, whatever the callback says */
} DeadList;

/* The rows are gone from the index in the manifest before VACUUM marks their heap pages all-visible (core/vis_f2.h): never a pending delete */
static void
flush_dead(DeadList *dead)
{
	if (dead->n == 0)
		return;
	(void) sift_del_bulk(dead->rel, dead->entry->part_id, dead->entry->key_lo, dead->recs, dead->n, dead->probe == NULL, dead->spec);
	dead->stats->tuples_removed += dead->n;
	dead->stats->num_index_tuples -= dead->n;
	dead->n = 0;
}

/* Offers one address to the callback of VACUUM (the one of CREATE INDEX CONCURRENTLY says none dead, and the heap says which are) */
static void
offer_tid(uint64 tid, IndexBulkDeleteCallback cb, void *state, DeadList *dead)
{
	ItemPointerData ip;
	SiftPendDelRec rec = {.tid = tid,.xmax = InvalidTransactionId,.cmax = 0};

	sift_f2_tid_set(&ip, tid);
	if (!cb(&ip, state) && (dead->probe == NULL || !sift_heap_probe_gone(dead->probe, tid, &rec)))
		return;
	if (dead->n == dead->cap)
	{
		dead->cap = Min(Max(64, dead->cap * 2), dead->max);
		dead->recs = dead->recs ? repalloc(dead->recs, dead->cap * sizeof(SiftPendDelRec)) : MemoryContextAlloc(dead->cx, dead->cap * sizeof(SiftPendDelRec));
	}
	dead->recs[dead->n++] = rec;
	if (dead->n == dead->max)
		flush_dead(dead);
}

/* No row of the part has a heap tuple a vacuum may find dead: its addresses, and those its rows moved to, lie in blocks the visibility map says all-visible */
static bool
clean_part(const DeadList *dead, const SiftPartDesc *d)
{
	return d->tid_max != 0 && !sift_vm_dirty_overlaps(dead->vm, d->tid_min >> 16, d->tid_max >> 16) && !sift_cd_remap_part_in_dirty(dead->rm, d->part_gen, dead->vm);
}

static bool
clean_granule(const DeadList *dead, const SiftPartDesc *d, const SiftTidIndexEntry *t, uint32 g)
{
	return t != NULL && !sift_vm_dirty_overlaps_bloom(dead->vm, t->tid_min >> 16, t->tid_max >> 16, t->blk_bloom) && !sift_cd_remap_granule_in_dirty(dead->rm, d->part_gen, g, dead->vm);
}

/* The address of a row a vacuum is offered: the one its row has now, never the one it had, whose tuple the heap reclaims while the row stays */
static uint64
offered_tid(SiftCdRemapCursor *moves, int row, uint64 raw)
{
	uint64		moved = sift_cd_remap_cursor_new_tid(moves, (uint16) row);

	return moved != 0 ? moved : raw;
}

/*
 * The live rows of one part: each TID is offered to the callback, and the ones it says are dead are collected. Only the _tid column is read, the granules a batch at a time through one
 * stream of blocks of the maintenance look-ahead; the parts and the granules that lie in blocks all-visible are not read at all.
 */
static uint64
offer_part(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *d, IndexBulkDeleteCallback cb, void *state, DeadList *dead)
{
	SiftPart   *part;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	const SiftTidIndexEntry *tix;
	uint64		len,
				live = 0;

	if (clean_part(dead, d))
		return d->nrows - d->ndead;
	part = sift_part_open_desc(rel, d, (uint32) e->part_id);
	dir = sift_part_granule_dir(rel, part);
	dv = d->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, d, (uint32) e->part_id, granule_rows(dir, part->hdr.ngranules)) : NULL;
	tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);
	for (uint32 g0 = 0; g0 < part->hdr.ngranules;)
	{
		uint32		n = 0;
		uint64	  **tids;

		if (clean_granule(dead, d, tix != NULL ? &tix[g0] : NULL, g0))
		{
			const SiftDvGran *dg = dv ? sift_dv_granule(dv, g0) : NULL;

			live += dir[g0].nrows - (dg != NULL ? dg->ndead : 0);
			g0++;
			continue;
		}
		n = 1;
		while (n < SIFT_TID_BATCH_GRANULES && g0 + n < part->hdr.ngranules && !clean_granule(dead, d, tix != NULL ? &tix[g0 + n] : NULL, g0 + n))
			n++;
		tids = sift_part_read_tids_batch(rel, part, dir, g0, n, SIFT_IOS_MAINT_SEQ);
		for (uint32 i = 0; i < n; i++)
		{
			SiftDvCursor dc;
			SiftCdRemapCursor moves;

			sift_dv_cursor_init(&dc, dv ? sift_dv_granule(dv, g0 + i) : NULL);
			sift_cd_remap_cursor_init(&moves, dead->rm, d->part_gen, g0 + i);
			for (int r = 0; r < dir[g0 + i].nrows; r++)
			{
				bool		gone = false;

				(void) sift_dv_cursor_find(&dc, r, &gone);
				if (gone)
					continue;
				live++;
				offer_tid(offered_tid(&moves, r, tids[i][r]), cb, state, dead);
			}
			pfree(tids[i]);
		}
		pfree(tids);
		g0 += n;
	}
	return live;
}

/*
 * The rows of the slab of a partition, offered as they are: the slab is read under share locks and nothing is written, so that a CREATE INDEX CONCURRENTLY that finds nothing dead
 * stops no writer. The rows the callback names are deleted by sift_del_bulk, which settles the slab first.
 */
static uint64
offer_slab(Relation rel, const SiftPartitionEntry *e, const SiftPartSpec *spec, IndexBulkDeleteCallback cb, void *state, DeadList *dead)
{
	SiftRowBuf *rb;
	uint64		live;

	if (sift_l0_slab_is_href(rel, e->part_id, e->l0_slab_blk))
	{
		uint32		n;
		SiftL0HeapRef *refs = sift_l0_href_refs(rel, e->part_id, e->l0_slab_blk, &n);

		for (uint32 i = 0; i < n; i++)
			offer_tid(sift_f2_tid(&refs[i].ctid), cb, state, dead);
		pfree(refs);
		return n;
	}
	rb = sift_rowbuf_new(spec);
	sift_l0_slab_read(rel, e->part_id, e->l0_slab_blk, rb, spec, NULL, NULL, NULL);
	for (int r = 0; r < rb->n; r++)
		offer_tid(rb->tid[r], cb, state, dead);
	live = rb->n;
	sift_rowbuf_free(rb);
	return live;
}

/* The validation pass of a concurrent build (the only caller of ambulkdelete that asks for progress reports): the journals of the relations of the pairs are filled, and the summaries are built over the parts of the first pass, after the rows the heap ended since are deleted from them */
static void
scope_validated(Relation rel)
{
	Relation	heap = table_open(rel->rd_index->indrelid, AccessShareLock);

	sift_scope_journals_validate();
	sift_f2_progress_scopes(heap, rel);
	sift_f2_scope_build(heap, rel);
	sift_f2_progress_scopes_done(heap, rel);
	table_close(heap, NoLock);
}

IndexBulkDeleteResult *
sift_f2_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	SiftF2Rel  *f = sift_f2_rel(rel);
	SiftManifestRead mr;
	SiftErrCtx	ctx;
	SiftF2Trace tr;
	SiftVmDirtySet vm;
	SiftHeapProbe probe;
	MemoryContext part_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 bulkdelete part"),
				partition_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 bulkdelete partition");
	int			dead_max = (int) Max(64, Min(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1) / sizeof(SiftPendDelRec), INT_MAX / 2));

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	sift_f2_trace_begin(&tr, rel, SIFT_JOB_VACUUM, false);
	sift_errctx_push(&ctx, RelationGetRelid(rel));
	sift_horizon_vacuum_read_begin(rel);
	sift_stage_flush(rel);
	sift_f2_trace_phase(&tr, "bulkdelete");
	if (info->report_progress)
		sift_heap_probe_begin(&probe, info->heaprel);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	sift_vm_dirty_build(&vm, info->heaprel, info->report_progress);
	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];
		MemoryContext old = MemoryContextSwitchTo(partition_cx);
		SiftPartDesc *parts;
		SiftCdRemapMap *rm = sift_cdi_remap_load(rel, e);
		int			np;
		DeadList	dead = {.max = dead_max,.cx = partition_cx,.rel = rel,.entry = e,.spec = &f->spec,.stats = stats,.vm = &vm,
			.rm = rm,.probe = info->report_progress ? &probe : NULL};

		if (e->l0_slab_blk != InvalidBlockNumber)
			stats->num_index_tuples += offer_slab(rel, e, &f->spec, callback, callback_state, &dead);
		parts = sift_manifest_read_parts(rel, e, &np);
		for (int p = 0; p < np; p++)
		{
			MemoryContextSwitchTo(part_cx);
			stats->num_index_tuples += offer_part(rel, e, &parts[p], callback, callback_state, &dead);
			MemoryContextSwitchTo(partition_cx);
			MemoryContextReset(part_cx);
		}
		flush_dead(&dead);
		MemoryContextSwitchTo(old);
		MemoryContextReset(partition_cx);
		sift_f2_trace_step(1, 0);
	}
	MemoryContextDelete(part_cx);
	MemoryContextDelete(partition_cx);
	sift_mem_release_os(dead_max * sizeof(SiftPendDelRec));
	sift_vm_dirty_free(&vm);
	if (info->report_progress)
	{
		sift_heap_probe_end(&probe);
		SIFT_TEST_GATE(sift_test_build_gate, 20);
		if (sift_f2_scoped(rel))
			scope_validated(rel);
	}
	sift_horizon_vacuum_read_end(rel);
	sift_errctx_pop(&ctx);
	sift_f2_trace_end(&tr);
	return stats;
}

IndexBulkDeleteResult *
sift_f2_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_f2_vacuum_run(info, stats, false);
}
