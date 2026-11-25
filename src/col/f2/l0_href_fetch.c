/*
 * col/f2/l0_href_fetch.c - the references of a slab resolved against the heap (col/f2/l0_href_fetch.h).
 */
#include "core/host.h"

#include "col/f2/l0_href_fetch.h"

#ifndef SIFT_HREF_STANDALONE
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "utils/combocid.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_vacuum_run.h"
#include "core/damage.h"
#include "core/f2/manifest.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#include "core/compat.h"
#endif

/* ---- sorting, filtering and resolving: no server ---- */

static inline uint64
ref_addr(const SiftL0HeapRef *r)
{
	return ((uint64) ItemPointerGetBlockNumberNoCheck(&r->ctid) << 16) | ItemPointerGetOffsetNumberNoCheck(&r->ctid);
}

static int
ref_cmp(const void *a, const void *b)
{
	const SiftL0HeapRef *x = a,
			   *y = b;
	uint64		ax = ref_addr(x),
				ay = ref_addr(y);

	if (ax != ay)
		return ax < ay ? -1 : 1;
	if (x->xmin == y->xmin)
		return 0;
	return TransactionIdPrecedes(x->xmin, y->xmin) ? -1 : 1;
}

uint32
sift_href_sort_dedup(SiftL0HeapRef *refs, uint32 n)
{
	uint32		k = 0;

	qsort(refs, n, sizeof(SiftL0HeapRef), ref_cmp);
	for (uint32 i = 0; i < n; i++)
		if (k == 0 || ref_addr(&refs[k - 1]) != ref_addr(&refs[i]))
			refs[k++] = refs[i];
	return k;
}

uint32
sift_href_unique_blocks(const SiftL0HeapRef *refs, uint32 n, BlockNumber *out)
{
	uint32		k = 0;

	for (uint32 i = 0; i < n; i++)
	{
		BlockNumber b = ItemPointerGetBlockNumberNoCheck(&refs[i].ctid);

		if (k == 0 || out[k - 1] != b)
			out[k++] = b;
	}
	return k;
}

uint32
sift_href_prefilter(SiftL0HeapRef *refs, uint32 n, const SiftHrefVis *vis, bool drop_unseen)
{
	uint32		k = 0;

	for (uint32 i = 0; i < n; i++)
	{
		bool		keep = true;

		if (TransactionIdIsNormal(refs[i].xmin))
		{
			SiftXidStatus st = vis->xid_status(vis->ctx, refs[i].xmin);

			keep = st != SIFT_XSTAT_ABORTED && !(drop_unseen && st == SIFT_XSTAT_NOT_SEEN);
		}
		if (keep && vis->pend_deleted != NULL && vis->pend_deleted(vis->ctx, &refs[i]))
			keep = false;
		if (keep)
			refs[k++] = refs[i];
	}
	return k;
}

uint32
sift_href_resolve(const SiftL0HeapRef *refs, uint32 n, const SiftHrefHeap *heap)
{
	uint32		hits = 0;

	for (uint32 i = 0; i < n; i++)
	{
		SiftHrefTuple t = {0};

		if (!heap->fetch(heap->ctx, &refs[i], &t))
			continue;
		heap->hit(heap->ctx, &refs[i], t.frozen ? FrozenTransactionId : refs[i].xmin, heap->is_current(heap->ctx, refs[i].xmin) ? t.cmin : 0);
		hits++;
	}
	return hits;
}

#ifndef SIFT_HREF_STANDALONE

/* ---- the heap: a read stream of the blocks, the index fetch of the table access method, the columns of the index formed from each tuple ---- */

typedef struct HrefCursor
{
	Relation	heap;
	Relation	index;
	Snapshot	snap;
	SnapshotData nonvac;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	IndexInfo  *iinfo;
	EState	   *estate;
	Datum	   *values;
	bool	   *isnull;
	const SiftL0HeapRef *refs;
	uint32		n;
	BlockNumber *blks;
	uint32		nblks;
	uint32		bpos;
	ReadStream *stream;
	Buffer		held;
	SiftF2Rel  *f;
	MemoryContext rowcxt;
	SiftCell   *cells;
	SiftCell  **elem;
	SiftRowBuf *rb;
} HrefCursor;

#define SIFT_HREF_STREAM_MIN_BLOCKS	4

static BlockNumber
next_block(ReadStream *stream, void *arg, void *per_buffer)
{
	HrefCursor *c = arg;

	return c->bpos < c->nblks ? c->blks[c->bpos++] : InvalidBlockNumber;
}

/* The block of the reference is in shared buffers and pinned: the stream's pin is moved to it when the references move on */
static void
stream_to(HrefCursor *c, BlockNumber blk)
{
	if (c->stream == NULL)
		return;
	while (!BufferIsValid(c->held) || BufferGetBlockNumber(c->held) != blk)
	{
		if (BufferIsValid(c->held))
			ReleaseBuffer(c->held);
		c->held = read_stream_next_buffer(c->stream, NULL);
		if (!BufferIsValid(c->held))
			SIFT_INVARIANT(false, "the read stream of the heap blocks ended before the references did");
	}
}

static bool
cursor_fetch(void *ctx, const SiftL0HeapRef *ref, SiftHrefTuple *out)
{
	HrefCursor *c = ctx;
	ItemPointerData tid = ref->ctid;
	bool		call_again = false,
				all_dead = false,
				shouldFree;
	HeapTuple	htup;

	stream_to(c, ItemPointerGetBlockNumber(&tid));
	if (!table_index_fetch_tuple(c->fetch, &tid, c->snap, c->slot, &call_again, &all_dead))
		return false;
	htup = ExecFetchSlotHeapTuple(c->slot, false, &shouldFree);
	out->frozen = HeapTupleHeaderXminFrozen(htup->t_data);
	out->cmin = TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(htup->t_data)) ? HeapTupleHeaderGetCmin(htup->t_data) : 0;
	return true;
}

static bool
cursor_is_current(void *ctx, TransactionId xid)
{
	return TransactionIdIsCurrentTransactionId(xid);
}

static void
cursor_hit(void *ctx, const SiftL0HeapRef *ref, TransactionId xmin, uint32 cmin)
{
	HrefCursor *c = ctx;
	uint8		key[SIFT_BOUND_BYTES];
	MemoryContext old;

	slot_getallattrs(c->slot);
	GetPerTupleExprContext(c->estate)->ecxt_scantuple = c->slot;
	FormIndexDatum(c->iinfo, c->slot, c->estate, c->values, c->isnull);
	memset(c->cells, 0, c->f->ncols * sizeof(SiftCell));
	memset(c->elem, 0, c->f->ncols * sizeof(SiftCell *));
	sift_f2_row(c->index, c->f, c->rowcxt, &ref->ctid, c->values, c->isnull, c->cells, c->elem, key);
	old = MemoryContextSwitchTo(c->rb->cx);
	sift_rowbuf_add(c->rb, sift_f2_tid(&ref->ctid), xmin, cmin, 0, key, c->cells, (const SiftCell *const *) c->elem);
	MemoryContextSwitchTo(old);
	MemoryContextReset(c->rowcxt);
	ResetExprContext(GetPerTupleExprContext(c->estate));
}

static HrefCursor *
cursor_open(Relation index, Snapshot snap, const SiftL0HeapRef *refs, uint32 n, SiftRowBuf *rb)
{
	HrefCursor *c = palloc0(sizeof(HrefCursor));

	c->index = index;
	c->heap = table_open(index->rd_index->indrelid, AccessShareLock);
	if (snap != NULL)
		c->snap = snap;
	else
	{
		InitNonVacuumableSnapshot(c->nonvac, GlobalVisTestFor(c->heap));
		c->snap = &c->nonvac;
	}
	c->fetch = sift_table_index_fetch_begin(c->heap);
	c->slot = table_slot_create(c->heap, NULL);
	c->iinfo = BuildIndexInfo(index);
	c->estate = CreateExecutorState();
	c->values = palloc(index->rd_index->indnatts * sizeof(Datum));
	c->isnull = palloc(index->rd_index->indnatts * sizeof(bool));
	c->f = sift_f2_rel(index);
	c->rowcxt = sift_col_rel_rowcxt(sift_col_rel(index));
	c->cells = palloc0(c->f->ncols * sizeof(SiftCell));
	c->elem = palloc0(c->f->ncols * sizeof(SiftCell *));
	c->refs = refs;
	c->n = n;
	c->rb = rb;
	c->blks = palloc(Max(n, 1) * sizeof(BlockNumber));
	c->nblks = sift_href_unique_blocks(refs, n, c->blks);
	if (c->nblks >= SIFT_HREF_STREAM_MIN_BLOCKS)
		c->stream = read_stream_begin_relation(READ_STREAM_DEFAULT, NULL, c->heap, MAIN_FORKNUM, next_block, c, 0);
	c->held = InvalidBuffer;
	return c;
}

static void
cursor_close(HrefCursor *c)
{
	if (BufferIsValid(c->held))
		ReleaseBuffer(c->held);
	if (c->stream != NULL)
		read_stream_end(c->stream);
	table_index_fetch_end(c->fetch);
	ExecDropSingleTupleTableSlot(c->slot);
	FreeExecutorState(c->estate);
	table_close(c->heap, AccessShareLock);
	pfree(c->blks);
	pfree(c);
}

/* ---- the fate of an inserter and the pending deletes a reader sees ---- */

typedef struct VisCtx
{
	SiftXidCache xc;
	SiftXidHorizonCache hc;
	bool		snapshot;
	const SiftPendDelIndex *px;
} VisCtx;

static SiftXidStatus
vis_status(void *ctx, TransactionId xid)
{
	VisCtx	   *v = ctx;

	if (v->snapshot)
		return sift_xid_cache_status(&v->xc, xid);
	return sift_xid_horizon_status(&v->hc, xid) == SIFT_H_ABORTED ? SIFT_XSTAT_ABORTED : SIFT_XSTAT_COMMITTED;
}

static bool
vis_pend_deleted(void *ctx, const SiftL0HeapRef *ref)
{
	const SiftPendDelIndex *px = ((VisCtx *) ctx)->px;

	return px != NULL && sift_pdel_idx_has(px, sift_f2_tid(&ref->ctid));
}

void
sift_href_rows(Relation index, Snapshot snap, const SiftPendDelIndex *px, SiftL0HeapRef *refs, uint32 n, SiftRowBuf *rb)
{
	bool		mvcc = snap != NULL && snap->snapshot_type == SNAPSHOT_MVCC;	/* a dirty snapshot sees the rows of a running transaction: only an aborted one's are dropped, as for a flush */
	VisCtx		vc = {.snapshot = mvcc,.px = px};
	SiftHrefVis vis = {.ctx = &vc,.xid_status = vis_status,.pend_deleted = vis_pend_deleted};
	HrefCursor *c;
	SiftHrefHeap heap;

	if (mvcc)
		sift_xid_cache_init(&vc.xc, snap);
	else
		sift_xid_horizon_init(&vc.hc, InvalidFullTransactionId);
	n = sift_href_sort_dedup(refs, n);
	n = sift_href_prefilter(refs, n, &vis, mvcc);
	if (n == 0)
		return;
	c = cursor_open(index, snap, refs, n, rb);
	heap = (SiftHrefHeap) {.ctx = c,.fetch = cursor_fetch,.is_current = cursor_is_current,.hit = cursor_hit};
	(void) sift_href_resolve(refs, n, &heap);
	cursor_close(c);
}

/* ---- the chain on buffers ---- */

static SiftL0HeapRef *
read_chain(Relation rel, uint64 part_id, BlockNumber tail, uint32 *n, BlockNumber **pages, uint32 *npages)
{
	SiftHrefIo	h;
	const char *why;
	BlockNumber bad = InvalidBlockNumber;
	SiftL0HeapRef *refs;

	sift_href_io_init(&h, rel, (uint32) part_id);
	refs = sift_l0_href_chain_read(&h.io, tail, n, pages, npages, &why, &bad);
	if (refs == NULL)
		SIFT_CORRUPT(rel, "Block %u: the slab of partition %u is damaged: %s.", bad, (uint32) part_id, why);
	return refs;
}

SiftL0HeapRef *
sift_l0_href_refs(Relation rel, uint64 part_id, BlockNumber tail, uint32 *n)
{
	return read_chain(rel, part_id, tail, n, NULL, NULL);
}

bool
sift_l0_slab_is_href(Relation rel, uint64 part_id, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	bool		href = (((const SiftL0SlabPageData *) SiftPageGetData(BufferGetPage(buf)))->flags & SIFT_L0_SLAB_HEAPREF) != 0;

	UnlockReleaseBuffer(buf);
	return href;
}

uint32
sift_l0_href_chain_rows(Relation rel, uint64 part_id, BlockNumber tail)
{
	Buffer		buf = sift_blk_read(rel, tail, BUFFER_LOCK_SHARE);
	uint32		rows = ((const SiftL0SlabPageData *) SiftPageGetData(BufferGetPage(buf)))->chain_rows;

	sift_page_expect(rel, BufferGetPage(buf), tail, SIFT_PT_L0_SLAB, 0, (uint32) part_id);
	UnlockReleaseBuffer(buf);
	return rows;
}

void
sift_l0_href_read(Relation rel, uint64 part_id, BlockNumber tail, SiftRowBuf *rb, const SiftPartSpec *spec, Snapshot snap, const SiftPendDelIndex *px,
				  BlockNumber **pages, int *npages)
{
	uint32		n,
				np;
	SiftL0HeapRef *refs = read_chain(rel, part_id, tail, &n, pages, &np);

	if (npages != NULL)
		*npages = np;
	sift_href_rows(rel, snap, px, refs, n, rb);
	pfree(refs);
}

void
sift_l0_href_flush_chain(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, BlockNumber tail)
{
	SiftRowBuf *rb = sift_rowbuf_new(spec);
	BlockNumber *pages;
	uint32		n,
				npages;
	SiftL0HeapRef *refs = read_chain(rel, part_id, tail, &n, &pages, &npages);

	sift_href_rows(rel, NULL, NULL, refs, n, rb);
	sift_l0_slab_publish(rel, part_id, key_lo, spec, rb, pages, (int) npages);
	sift_rowbuf_free(rb);
	pfree(refs);
}

void
sift_l0_slab_request_flush(Relation rel, uint64 part_id)
{
	(void) sift_f2_maint_request(rel, SIFT_MK_SLAB_FLUSH, part_id);
}

#endif							/* !SIFT_HREF_STANDALONE */
