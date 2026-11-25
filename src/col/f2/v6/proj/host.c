/*
 * col/f2/v6/proj/host.c - the parts and the rows a projection executor reads, for a sift_col index (the scan of col/f2/sink/sink.h, the rows vetted against the heap) and a sift_table (the store scan of
 * col/f2/store/scan.h: the rows its own visibility rules keep, the cells its column deltas patched). The parts of a partition with column deltas are never settled: a projection holds their cells
 * unpatched. Both hand out the rows a predicate cannot be decided for on the stored values (a numeric NaN) with the tag of the cell, as the
 * table does, and the executor decides what it does with them. Everything is read of the plan the caller pins (col/f2/v6/proj/host.h).
 */
/* check-memory: allow-allocs the arrays follow the parts of one plan, whose partition lists the manifest bounds */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "executor/tuptable.h"
#include "storage/bufmgr.h"

#include "col/f2/v6/colstat.h"
#include "col/f2/v6/proj/host.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/proj/overlay.h"
#include "col/f2/sink/sink.h"
#include "col/f2/store/scan.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/tbl.h"

static SiftProjHostVis
part_vis(const SiftV6ProjHost *h, const SiftPartDesc *d, SiftProjKind kind, FullTransactionId xmin, V6VisMap *vis, BlockNumber *dirty, int *ndirty)
{
	SiftVisClass c = sift_vis_part_class(d, false, xmin);
	SiftProjVisIn in = {.inserts_settled = sift_vis_part_inserts_settled(d, xmin),.has_dv = d->dv_blk != InvalidBlockNumber,.dv_settled = c == SIFT_VIS_PART_SETTLED_DV,
	.heap_host = h->heap != NULL,.sort_kind = kind == SIFT_PROJ_SORT,.has_envelope = d->tid_max != 0};

	*ndirty = 0;
	if (in.inserts_settled && in.heap_host && !in.sort_kind)
		in.ndirty_blocks = sift_v6_vis_dirty_blocks(vis, h->heap, d, dirty, SIFT_PROJ_MAX_DIRTY_VM_BLKS);
	*ndirty = Max(in.ndirty_blocks, 0);
	return sift_proj_vis_class(&in);
}

/* The attributes the column patches of a part touch that the snapshot sees; a sift_col's chain moves the heap addresses a projection holds and patches its leaves: every attribute */
static uint64
part_patched_attrs(const SiftV6ProjHost *h, SiftTblVis *vis, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	SiftColDeltaPartMap *map;
	uint64		bloom;

	if (!(e->flags & SIFT_PARTENTRY_HAS_COL_DELTA))
		return 0;
	if (h->heap != NULL)
		return ~UINT64CONST(0);
	map = sift_col_delta_load_part_map(h->rel, e, d->part_gen, sift_part_open_desc(h->rel, d, (uint32) e->part_id)->hdr.ngranules, vis, 0);
	bloom = map != NULL ? map->dirty_attnum_bloom : 0;
	if (map != NULL)
		sift_col_delta_part_map_free(map);
	return bloom;
}

/* The addresses of the partition the snapshot sees deleted by a pending delete or by this transaction's own deletes (staged_px, built once for the scan); NULL when there are none */
static const SiftPendDelIndex *
partition_pending(const SiftV6ProjHost *h, const SiftPartitionEntry *e, const SiftPendDelIndex *staged_px)
{
	return h->heap == NULL ? sift_tbl_f2_pending_index(h->rel, e, h->snap) : sift_f2_pending_index(h->rel, e, h->snap, staged_px);
}

SiftV6BaseParts
sift_v6_host_base(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, SiftProjKind kind)
{
	FullTransactionId xmin = sift_vis_snap_xmin(h->snap);
	SiftV6BaseParts b = {.nseg = plan->npartitions};
	V6VisMap	vm;
	SiftTblVis	vis;
	BlockNumber dirty[SIFT_PROJ_MAX_DIRTY_VM_BLKS];
	Size		alloc_dirty;
	int			total = 0,
				nstaged = 0,
				ndirty = 0;
	uint64	   *staged = h->heap != NULL ? sift_stage_dead(h->rel, h->snap, &nstaged) : NULL;
	const SiftPendDelIndex *staged_px = sift_f2_staged_index(staged, nstaged);

	for (int i = 0; i < plan->npartitions; i++)
		total += plan->partlists[i].nparts;
	alloc_dirty = Max(total, 1);
	b.part = palloc(Max(total, 1) * sizeof(SiftPartDesc));
	b.part_id = palloc(Max(total, 1) * sizeof(uint32));
	b.part_seg = palloc(Max(total, 1) * sizeof(int));
	b.vis = palloc(Max(total, 1) * sizeof(SiftProjHostVis));
	b.cd_bloom = palloc0(Max(total, 1) * sizeof(uint64));
	b.seg_start = palloc((plan->npartitions + 1) * sizeof(int));
	b.seg_pdel = palloc(Max(plan->npartitions, 1) * sizeof(SiftPendDelIndex *));
	b.dirty_off = palloc((total + 1) * sizeof(uint32));
	b.dirty_blk = palloc(alloc_dirty * sizeof(BlockNumber));
	sift_v6_vis_init(&vm, CurrentMemoryContext);
	if (h->heap == NULL)
		sift_tbl_f2_vis_begin(&vis, h->rel, h->snap);
	for (int i = 0; i < plan->npartitions; i++)
	{
		const SiftPartitionEntry *e = &plan->partitions[i];
		const SiftPlanPartList *list = &plan->partlists[i];

		b.seg_start[i] = b.n;
		b.seg_pdel[i] = partition_pending(h, e, staged_px);
		for (int k = 0; k < list->nparts; k++)
		{
			int			nd;

			b.part[b.n] = list->parts[k];
			b.part_id[b.n] = (uint32) e->part_id;
			b.part_seg[b.n] = i;
			b.vis[b.n] = part_vis(h, &list->parts[k], kind, xmin, &vm, dirty, &nd);
			b.cd_bloom[b.n] = part_patched_attrs(h, &vis, e, &list->parts[k]);
			b.dirty_off[b.n] = ndirty;
			if (ndirty + nd > alloc_dirty)
			{
				alloc_dirty = Max(alloc_dirty * 2, ndirty + nd);
				b.dirty_blk = repalloc(b.dirty_blk, alloc_dirty * sizeof(BlockNumber));
			}
			memcpy(b.dirty_blk + ndirty, dirty, nd * sizeof(BlockNumber));
			ndirty += nd;
			b.n++;
		}
	}
	b.seg_start[plan->npartitions] = b.n;
	b.dirty_off[b.n] = ndirty;
	sift_v6_vis_free(&vm);
	return b;
}

/* ---- what has died in a part since its projection part was written ---- */

typedef struct HeapVisible
{
	const SiftV6ProjHost *h;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
} HeapVisible;

/* The heap row at the address is visible to the snapshot: the chain of a HOT update is followed from its root, as an index scan does */
static bool
heap_row_visible(void *arg, uint64 tid)
{
	HeapVisible *hv = arg;
	ItemPointerData ip;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&ip, tid);
	return table_index_fetch_tuple(hv->fetch, &ip, hv->h->snap, hv->slot, &again, &dead);
}

void
sift_v6_host_overlay_rows(const SiftV6ProjHost *h, const SiftV6BaseParts *b, int i, const SiftProjLink *link, SiftProjRowFn fn, void *arg)
{
	const SiftPartDesc *d = &b->part[i];
	const SiftPendDelIndex *px = b->seg_pdel[b->part_seg[i]];
	SiftProjDirty dirty = {.blk = b->dirty_blk + b->dirty_off[i],.n = (int) (b->dirty_off[i + 1] - b->dirty_off[i])};
	bool		unchanged = sift_proj_dv_unchanged(d, link),
				pend = sift_proj_part_meets_pend(d, px);
	HeapVisible hv = {.h = h};
	SiftPart   *part;
	SiftGranuleDesc *dir;
	SiftDv	   *dv = NULL;
	const SiftTidIndexEntry *tix = NULL;
	SiftTidIndexEntry none = {0};
	uint64		len;

	if (unchanged && !pend && dirty.n == 0)
		return;
	dirty.bloom = sift_proj_dirty_bloom(dirty.blk, dirty.n);
	part = sift_part_open_desc(h->rel, d, b->part_id[i]);
	dir = sift_part_granule_dir(h->rel, part);
	if (d->dv_blk != InvalidBlockNumber)
	{
		uint16	   *rows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < part->hdr.ngranules; g++)
			rows[g] = dir[g].nrows;
		dv = sift_dv_read(h->rel, d, b->part_id[i], rows);
	}
	if (pend || dirty.n > 0)
	{
		tix = sift_part_read_section(h->rel, part, SEC_TID_INDEX, &len);
		SIFT_INVARIANT(tix != NULL && len == (uint64) part->hdr.ngranules * sizeof(*tix), "a projected part has its address index");
	}
	if (dirty.n > 0)
	{
		hv.fetch = sift_table_index_fetch_begin(h->heap);
		hv.slot = table_slot_create(h->heap, NULL);
	}
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		const SiftDvGran *dg = dv != NULL ? sift_dv_granule(dv, g) : NULL;
		uint64		dv_dead[SIFT_DV_MASK_WORDS],
					baseline[SIFT_DV_MASK_WORDS],
					mask[SIFT_DV_MASK_WORDS];
		SiftProjOverlayIn in = {.nrows = dir[g].nrows,.pend = pend ? px : NULL,.dirty = dirty,.visible = heap_row_visible,.arg = &hv};

		if (!sift_proj_overlay_granule_hit(dg != NULL && !unchanged, in.pend, &dirty, tix != NULL ? &tix[g] : &none))
			continue;
		if (dg != NULL && !unchanged && sift_dv_mask64(dg, h->snap, dv_dead) > 0)
			in.dv_dead = dv_dead;
		if (dg != NULL && link->src_ndead > 0 && sift_dv_mask64(dg, NULL, baseline) > 0)
			in.baseline_dead = baseline;
		if (tix != NULL)
			in.tids = sift_part_read_tids(h->rel, part, dir, g);
		if (sift_proj_overlay_mask(&in, mask) > 0)
			sift_proj_scan_granule(h->rel, part, g, &h->store->spec, mask, fn, arg);
	}
	if (hv.fetch != NULL)
	{
		ExecDropSingleTupleTableSlot(hv.slot);
		table_index_fetch_end(hv.fetch);
	}
}

SiftPendDelIndex *
sift_v6_host_dv_dead(const SiftV6ProjHost *h, const SiftV6BaseParts *b, int i)
{
	const SiftPartDesc *d = &b->part[i];
	SiftPart   *part = sift_part_open_desc(h->rel, d, b->part_id[i]);
	SiftGranuleDesc *dir = sift_part_granule_dir(h->rel, part);
	uint16	   *grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));
	uint16		rows[SIFT_GRANULE_ROWS];
	uint64	   *dead = palloc(Max(d->ndead, 1) * sizeof(uint64));
	SiftDv	   *dv;
	int			n = 0;

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
		grows[g] = dir[g].nrows;
	dv = sift_dv_read(h->rel, d, b->part_id[i], grows);
	for (int k = 0; k < dv->ndirty; k++)
	{
		uint16		nr = sift_dv_visible_rows(&dv->gran[k], h->snap, rows, SIFT_GRANULE_ROWS);
		uint64	   *tids = nr > 0 ? sift_part_read_tids(h->rel, part, dir, dv->gran[k].granule_id) : NULL;

		for (int r = 0; r < nr; r++)
			dead[n++] = tids[rows[r]];
	}
	return n > 0 ? sift_pdel_idx_build(dead, n) : NULL;
}

/* The cells of a row the scan sets leaf by leaf: the others stay NULL */
static SiftCell *
null_row(int ncols)
{
	SiftCell   *row = palloc0(Max(ncols, 1) * sizeof(SiftCell));

	for (int c = 0; c < ncols; c++)
		row[c].isnull = true;
	return row;
}

/* ---- a sift_col: the scan of the index with the parts left out, every row vetted against the heap ---- */

typedef struct RestScan
{
	const SiftV6ProjHost *h;
	const int  *leaves;
	int			nleaves;
	SiftV6RowFn fn;
	void	   *arg;
	SiftCell   *row;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	Buffer		vmbuf;
	BlockNumber vm_blk;
	bool		vm_visible;
} RestScan;

static bool
row_visible(RestScan *s, uint64 addr)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&tid, addr);
	if (ItemPointerGetBlockNumber(&tid) != s->vm_blk)
	{
		s->vm_blk = ItemPointerGetBlockNumber(&tid);
		s->vm_visible = VM_ALL_VISIBLE(s->h->heap, s->vm_blk, &s->vmbuf);
	}
	return s->vm_visible || table_index_fetch_tuple(s->fetch, &tid, s->h->snap, s->slot, &again, &dead);
}

static SiftCell
cell_of_view(const ColView *v, int r)
{
	SiftCell	c = {.isnull = v->isnull[r],.tag = v->tag != NULL ? v->tag[r] : 0};

	if (c.isnull)
		return c;
	if (v->kind == SIFT_CELL_INT)
		c.i = v->i[r];
	else if (v->kind == SIFT_CELL_F8)
		c.f = v->f[r];
	else
	{
		c.s = v->s[r];
		c.slen = v->slen[r];
	}
	return c;
}

static void
rest_sink(void *arg, const SiftF2Batch *b)
{
	RestScan   *s = arg;

	for (int r = 0; r < b->n; r++)
	{
		bool		live = ((b->live[r >> 6] >> (r & 63)) & 1) != 0;

		if (!(live || ((b->maybe[r >> 6] >> (r & 63)) & 1)) || (s->h->heap != NULL && !row_visible(s, b->tid[r])))
			continue;
		for (int i = 0; i < s->nleaves; i++)
			s->row[s->leaves[i]] = cell_of_view(&b->views[s->leaves[i]], r);
		if (live || sift_proj_preds_pass(s->h->preds, s->h->npreds, s->row) || !sift_proj_preds_decidable(s->h->preds, s->h->npreds, s->row))
			s->fn(s->arg, b->tid[r], s->row);
	}
}

static bool
rest_of_col(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, const uint64 *skip, int nskip, const int *leaves, int nleaves, SiftV6RowFn fn, void *arg)
{
	RestScan	s = {.h = h,.leaves = leaves,.nleaves = nleaves,.fn = fn,.arg = arg,.vm_blk = InvalidBlockNumber};
	IndexScanDesc scan;

	s.row = null_row(h->store->ncols);
	s.fetch = sift_table_index_fetch_begin(h->heap);
	s.slot = table_slot_create(h->heap, NULL);
	scan = sift_index_beginscan(h->heap, h->rel, h->snap, h->nkeys);
	index_rescan(scan, h->keys, h->nkeys, NULL, 0);
	sift_f2_scan_skip_parts(scan, skip, nskip);
	sift_f2_scan_set_plan(scan, plan);
	sift_f2_scan_sink_maybe(scan, rest_sink, &s, leaves, nleaves);
	index_endscan(scan);
	if (BufferIsValid(s.vmbuf))
		ReleaseBuffer(s.vmbuf);
	ExecDropSingleTupleTableSlot(s.slot);
	table_index_fetch_end(s.fetch);
	pfree(s.row);
	return true;
}

/* ---- a sift_table: the store scan, whose rows are decided by the table's own versions and whose cells have the column deltas of their partition laid over ---- */

static bool
rest_of_table(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, const uint64 *skip, int nskip, const int *leaves, int nleaves, SiftV6RowFn fn, void *arg)
{
	RestScan	s = {.h = h,.leaves = leaves,.nleaves = nleaves,.fn = fn,.arg = arg,.row = null_row(h->store->ncols)};
	SiftStoreQuals q;
	SiftStoreScanOpts o = {.rel = h->rel,.store = h->store,.snap = h->snap,.leaves = leaves,.nleaves = nleaves,.skip_gens = skip,.nskip_gens = nskip,.plan = plan,.emit_maybe = true};

	sift_f2_store_key_preds(h->rel, h->store, h->keys, h->nkeys, CurrentMemoryContext, &q);
	o.preds = q.preds;
	o.npreds = q.npreds;
	if (q.possible)
		sift_f2_store_scan_sink(&o, rest_sink, &s);
	pfree(s.row);
	return true;
}

bool
sift_v6_host_scan_rest(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, const uint64 *skip, int nskip, const int *leaves, int nleaves, SiftV6RowFn fn, void *arg)
{
	return h->heap != NULL ? rest_of_col(h, plan, skip, nskip, leaves, nleaves, fn, arg) : rest_of_table(h, plan, skip, nskip, leaves, nleaves, fn, arg);
}
