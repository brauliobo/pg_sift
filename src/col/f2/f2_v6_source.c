/*
 * col/f2/f2_v6_source.c - the batches of a sift_table or a sift_col index of format major 2 for the operators of SiftAggV6 (agg/v6/v6_vexpr.h): the granules of the store scan (f2_store_scan.h) one
 * part at a time, their rows put in the vectors of the input, and the quals of the operator run over each batch.
 *
 * A sorted source merges the parts of a run by the key of the store (the columns of key_order, as they are ordered), a run being the partitions that start at one lower bound, so the rows come in the
 * order of the key whatever the parts a partition has grown: it holds a granule of each part of a run, no more. An unsorted source reads one part after the other. A row of an index goes to a batch
 * only when the heap shows it to the snapshot; a row of a table is shown by its own versions.
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the parts of one run, not the rows of the relation */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/v6/v6_vexpr.h"
#include "col/f2/f2.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/f2_store_schema.h"
#include "col/f2/f2_v6_format.h"
#include "col/f2/f2_v6_scan_int.h"
#include "core/compat.h"
#include "core/memtrack.h"

/* The heap rows of an index: the visibility map says a page is all visible, else the tuple is fetched */
typedef struct HeapVet
{
	Relation	heap;
	Snapshot	snap;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	Buffer		vmbuf;
	BlockNumber vm_blk;
	bool		vm_visible;
} HeapVet;

/* A part of the run and the row of its granule the merge is at */
typedef struct Cursor
{
	SiftStorePart *part;
	SiftF2Batch b;
	int			row;
} Cursor;

typedef struct V6Src
{
	SiftV6Source base;
	MemoryContext cx;
	MemoryContext run_cx;
	Relation	rel;
	Relation	heap;
	SiftF2Store *store;
	Snapshot	snap;
	SiftV6Input in;
	int		   *leaf;			/* of each column of the input, the leaf of the store that holds it */
	int		   *mask_leaf;
	bool		sorted;
	SiftStoreScanOpts opts;
	SiftStoreScan *scan;
	List	   *run;
	int			next_part;
	Cursor	  **heap_of;		/* the cursors with a row, the one with the least key first (a sorted source), else the one being read */
	int			nheap;
	HeapVet    *vet;
	TupleTableSlot *fslot;
	SiftV6Vexpr quals;
	bool		has_quals;
	SiftVexprCtx *vctx;
	SiftVec		vec[SIFT_V6_MAX_COLS];
	void	   *data[SIFT_V6_MAX_COLS];
	uint64	   *valid[SIFT_V6_MAX_COLS];
	uint64	   *nan[SIFT_V6_MAX_COLS];	/* of a numeric column: the rows that hold a NaN, which valid says are NULL */
	bool		has_nan[SIFT_V6_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_V6_MAX_MASKS];
	uint64	   *mbits[SIFT_V6_MAX_MASKS];
	uint64	   *mvalid[SIFT_V6_MAX_MASKS];
	uint64		mask[SIFT_VEC_WORDS];
	uint64		tag[SIFT_VEC_LEN];
	MemoryContext strings;		/* the bytes of the long strings of the batch: the store's own go with the cursor that moves on */
} V6Src;

/* ---- the heap rows of an index ---- */

static HeapVet *
vet_new(MemoryContext cx, Relation heap, Snapshot snap)
{
	HeapVet    *v = MemoryContextAllocZero(cx, sizeof(HeapVet));

	v->heap = heap;
	v->snap = snap;
	v->vm_blk = InvalidBlockNumber;
	v->fetch = sift_table_index_fetch_begin(heap);
	v->slot = table_slot_create(heap, NULL);
	return v;
}

static void
vet_end(HeapVet *v)
{
	if (BufferIsValid(v->vmbuf))
		ReleaseBuffer(v->vmbuf);
	ExecDropSingleTupleTableSlot(v->slot);
	table_index_fetch_end(v->fetch);
}

static bool
vet_row(HeapVet *v, uint64 addr)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&tid, addr);
	if (ItemPointerGetBlockNumber(&tid) != v->vm_blk)
	{
		v->vm_blk = ItemPointerGetBlockNumber(&tid);
		v->vm_visible = VM_ALL_VISIBLE(v->heap, v->vm_blk, &v->vmbuf);
	}
	return v->vm_visible || table_index_fetch_tuple(v->fetch, &tid, v->snap, v->slot, &again, &dead);
}

/* ---- the merge of the parts of a run ---- */

static bool
cursor_advance(Cursor *c)
{
	for (;;)
	{
		for (c->row++; c->row < c->b.n; c->row++)
			if ((c->b.live[c->row >> 6] >> (c->row & 63)) & 1)
				return true;
		if (!sift_f2_store_part_next(c->part, &c->b))
			return false;
		c->row = -1;
	}
}

/* the key of the rows of two cursors as the parts are sorted: each column ascending or descending, the NULLs last or first */
static int
key_cmp(const SiftF2Store *st, const Cursor *a, const Cursor *b)
{
	for (int k = 0; k < st->nkey; k++)
	{
		const ColView *x = &a->b.views[st->key_cols[k]];
		const ColView *y = &b->b.views[st->key_cols[k]];
		const uint8 flags = st->spec.sort_flags[k];
		const bool	nx = x->isnull[a->row],
					ny = y->isnull[b->row];
		int			cmp;

		if (nx || ny)
		{
			if (nx == ny)
				continue;
			return (nx ? 1 : -1) * ((flags & SIFT_KEY_NULLS_FIRST) ? -1 : 1);
		}
		cmp = x->i[a->row] < y->i[b->row] ? -1 : x->i[a->row] > y->i[b->row];
		if (cmp != 0)
			return cmp * ((flags & SIFT_KEY_DESC) ? -1 : 1);
	}
	return 0;
}

static void
heap_swap(V6Src *s, int a, int b)
{
	Cursor	   *t = s->heap_of[a];

	s->heap_of[a] = s->heap_of[b];
	s->heap_of[b] = t;
}

static bool
heap_less(const V6Src *s, int a, int b)
{
	return key_cmp(s->store, s->heap_of[a], s->heap_of[b]) < 0;
}

static void
heap_down(V6Src *s, int i)
{
	for (;;)
	{
		int			least = i,
					l = 2 * i + 1,
					r = l + 1;

		if (l < s->nheap && heap_less(s, l, least))
			least = l;
		if (r < s->nheap && heap_less(s, r, least))
			least = r;
		if (least == i)
			return;
		heap_swap(s, i, least);
		i = least;
	}
}

static void
heap_add(V6Src *s, Cursor *c)
{
	int			i = s->nheap++;

	s->heap_of[i] = c;
	for (; i > 0 && heap_less(s, i, (i - 1) / 2); i = (i - 1) / 2)
		heap_swap(s, i, (i - 1) / 2);
}

static void
cursor_start(V6Src *s, SiftStorePart *part)
{
	Cursor	   *c = MemoryContextAllocZero(s->run_cx, sizeof(Cursor));

	c->part = part;
	c->row = -1;
	if (cursor_advance(c))
		heap_add(s, c);
}

/* the next part of the run, or all of them for a sorted source, whose rows are merged; false when the scan has no more */
static bool
run_refill(V6Src *s)
{
	if (s->next_part == list_length(s->run))
	{
		MemoryContextReset(s->run_cx);
		s->run = sift_f2_store_scan_next_run(s->scan, s->sorted);
		s->next_part = 0;
		s->heap_of = MemoryContextAlloc(s->run_cx, Max(list_length(s->run), 1) * sizeof(Cursor *));
		if (s->run == NIL)
			return false;
	}
	do
		cursor_start(s, list_nth(s->run, s->next_part++));
	while (s->sorted && s->next_part < list_length(s->run));
	return true;
}

/* the cursor of the next row of the relation, NULL after the last */
static Cursor *
merge_top(V6Src *s)
{
	while (s->nheap == 0)
		if (!run_refill(s))
			return NULL;
	return s->heap_of[0];
}

static void
merge_pop(V6Src *s)
{
	Cursor	   *c = s->heap_of[0];

	if (!cursor_advance(c))
		s->heap_of[0] = s->heap_of[--s->nheap];
	heap_down(s, 0);
}

/* ---- the vectors of a batch ---- */

static void
unreadable(const V6Src *s, int leaf)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column \"%s\" holds a value the stored form cannot hold", s->store->schema->cols[leaf].name),
					errhint("Run the statement with sift.enable_agg = off.")));
}

static SiftStrView
text_cell(V6Src *s, const ColView *cv, int r)
{
	SiftStrView v = sift_f2_v6_text_view(cv, r, s->in.is_table);

	if (v.len > 12)
	{
		char	   *own = MemoryContextAlloc(s->strings, v.len);

		memcpy(own, v.u.ptr, v.len);
		v.u.ptr = own;
	}
	return v;
}

static void
put_col(V6Src *s, int i, const ColView *cv, int r, int n)
{
	const bool	valid = !cv->isnull[r];

	if (!valid && cv->tag != NULL && cv->tag[r] != 0)
	{
		if (s->in.col[i].typid != NUMERICOID || cv->tag[r] != SIFT_COLTAG_UNSURE)
			unreadable(s, s->leaf[i]);
		s->nan[i][n >> 6] |= (uint64) 1 << (n & 63);
		s->has_nan[i] = true;
	}
	s->valid[i][n >> 6] |= (uint64) valid << (n & 63);
	switch (s->in.col[i].type)
	{
		case SVEC_I32:
			((int32 *) s->data[i])[n] = valid ? (int32) cv->i[r] : 0;
			break;
		case SVEC_F64:
			((double *) s->data[i])[n] = valid ? cv->f[r] : 0;
			break;
		case SVEC_STRVIEW:
			((SiftStrView *) s->data[i])[n] = valid ? text_cell(s, cv, r) : (SiftStrView) {0};
			break;
		default:
			((int64 *) s->data[i])[n] = valid ? cv->i[r] : 0;
			break;
	}
}

static void
put_mask(V6Src *s, int m, const ColView *cv, int r, int n)
{
	const bool	valid = !cv->isnull[r];

	s->mvalid[m][n >> 6] |= (uint64) valid << (n & 63);
	s->mbits[m][n >> 6] |= (uint64) (valid && cv->i[r] != 0) << (n & 63);
}

static void
put_row(V6Src *s, const SiftF2Batch *b, int r, int n)
{
	for (int i = 0; i < s->in.ncols; i++)
		put_col(s, i, &b->views[s->leaf[i]], r, n);
	for (int m = 0; m < s->in.nmasks; m++)
		put_mask(s, m, &b->views[s->mask_leaf[m]], r, n);
	s->tag[n] = b->tid[r];
}

static void
batch_start(V6Src *s)
{
	MemoryContextReset(s->strings);
	for (int i = 0; i < s->in.ncols; i++)
	{
		memset(s->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->nan[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		s->has_nan[i] = false;
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		memset(s->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->mvalid[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	}
}

/* up to SIFT_VEC_LEN rows of the relation in the vectors */
static int
batch_fill(V6Src *s)
{
	int			n = 0;
	Cursor	   *c;

	batch_start(s);
	while (n < SIFT_VEC_LEN && (c = merge_top(s)) != NULL)
	{
		if (s->vet == NULL || vet_row(s->vet, c->b.tid[c->row]))
			put_row(s, &c->b, c->row, n++);
		merge_pop(s);
	}
	for (int i = 0; i < s->in.ncols; i++)
		s->vec[i] = (SiftVec) {.data = s->data[i],.valid = s->valid[i],.nan = s->has_nan[i] ? s->nan[i] : NULL,.len = (uint16) n,.all_valid = false,.dense = true,.type = s->in.col[i].type,.scale = s->in.col[i].scale};
	return n;
}

/* the instants of the timestamps of the batch, for the zone a program reads (zone_cover) */
static void
cover_zone(const V6Src *s, int n)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	if (s->quals.zone == NULL)
		return;
	for (int i = 0; i < s->in.ncols; i++)
		if (s->in.col[i].typid == TIMESTAMPTZOID || s->in.col[i].typid == TIMESTAMPOID)
			for (int r = 0; r < n; r++)
				if ((s->valid[i][r >> 6] >> (r & 63)) & 1)
				{
					lo = Min(lo, ((int64 *) s->data[i])[r]);
					hi = Max(hi, ((int64 *) s->data[i])[r]);
				}
	if (lo <= hi)
		zone_cover(s->quals.zone, lo, hi);
}

static bool
src_next(SiftV6Source *base, SiftV6Batch *out)
{
	V6Src	   *s = (V6Src *) base;
	int			n;

	while ((n = batch_fill(s)) > 0)
	{
		*out = (SiftV6Batch) {.n = (uint16) n,.col = s->vec,.masks = s->masks,.tag = s->tag};
		if (!s->has_quals)
			return true;
		cover_zone(s, n);
		if (sift_v6_vexpr_filter(s->vctx, &s->quals, out, s->mask) > 0)
		{
			out->mask = s->mask;
			return true;
		}
	}
	return false;
}

static bool
src_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	V6Src	   *s = (V6Src *) base;
	ItemPointerData tid;

	sift_f2_tid_set(&tid, tag);
	if (!table_tuple_fetch_row_version(s->heap, &tid, s->snap, s->fslot))
		return false;
	for (int i = 0; i < n; i++)
		values[i] = slot_getattr(s->fslot, attnos[i], &isnull[i]);
	return true;
}

static void
src_close(SiftV6Source *base)
{
	V6Src	   *s = (V6Src *) base;

	sift_f2_store_scan_close(s->scan);
	ExecDropSingleTupleTableSlot(s->fslot);
	if (s->vet != NULL)
		vet_end(s->vet);
	if (s->heap != s->rel)
		table_close(s->heap, NoLock);
	relation_close(s->rel, NoLock);
	MemoryContextDelete(s->cx);
}

/* ---- the open ---- */

static int
leaf_of(const SiftF2Store *st, AttrNumber attno)
{
	SIFT_INVARIANT(attno >= 1 && attno <= st->natts && sift_f2_store_attr_leaves(st, attno) == 1, "attribute %d of relation %u has no single leaf", attno, st->relid);
	return st->att_first[attno];
}

/* the leaves of the numeric columns, whose NaN the vectors carry */
static bool *
numeric_leaves(const V6Src *s)
{
	bool	   *nan_leaves = palloc0(Max(s->store->ncols, 1) * sizeof(bool));

	for (int i = 0; i < s->in.ncols; i++)
		nan_leaves[s->leaf[i]] = s->in.col[i].typid == NUMERICOID;
	return nan_leaves;
}

/* the leaves the scan decodes: those of the columns and of the booleans of the input, and the key when the rows are merged by it */
static int *
scan_leaves(V6Src *s, int *nleaves)
{
	int		   *leaves = MemoryContextAlloc(s->cx, (s->in.ncols + s->in.nmasks + s->store->nkey) * sizeof(int));
	int			n = 0;

	for (int i = 0; i < s->in.ncols; i++)
		leaves[n++] = s->leaf[i] = leaf_of(s->store, s->in.col[i].attno);
	for (int m = 0; m < s->in.nmasks; m++)
	{
		SIFT_INVARIANT(s->in.mask[m].key == NULL, "a source of relation %u was asked for the keys of a document", s->store->relid);
		leaves[n++] = s->mask_leaf[m] = leaf_of(s->store, s->in.mask[m].attno);
	}
	for (int k = 0; s->sorted && k < s->store->nkey; k++)
		leaves[n++] = s->store->key_cols[k];
	*nleaves = n;
	return leaves;
}

static void
vectors_new(V6Src *s)
{
	for (int i = 0; i < s->in.ncols; i++)
	{
		const size_t width = s->in.col[i].type == SVEC_I32 ? sizeof(int32) : s->in.col[i].type == SVEC_STRVIEW ? sizeof(SiftStrView) : sizeof(int64);

		s->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * width, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->nan[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		s->mbits[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		s->mvalid[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		s->masks[m] = (SiftVexprMaskIn) {.bits = s->mbits[m],.valid = s->mvalid[m]};
	}
}

SiftV6Source *
sift_f2_v6_source_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift v6 source");
	V6Src	   *s = MemoryContextAllocZero(cx, sizeof(V6Src));
	MemoryContext old = MemoryContextSwitchTo(cx);

	s->cx = cx;
	s->run_cx = sift_mem_allocset_create(cx, "sift v6 source run");
	s->strings = sift_mem_allocset_create(cx, "sift v6 source strings");
	s->in = *in;
	s->sorted = sorted;
	s->snap = estate->es_snapshot;
	s->rel = in->is_table ? table_open(in->indexoid, AccessShareLock) : index_open(in->indexoid, AccessShareLock);
	s->heap = in->is_table ? s->rel : table_open(s->rel->rd_index->indrelid, AccessShareLock);
	s->store = sift_f2_store_open(s->rel);
	SIFT_INVARIANT(!sorted || s->store->nkey > 0, "a sorted source of relation %u has no key", in->indexoid);
	s->leaf = palloc(Max(in->ncols, 1) * sizeof(int));
	s->mask_leaf = palloc(Max(in->nmasks, 1) * sizeof(int));
	s->opts = (SiftStoreScanOpts) {.rel = s->rel,.store = s->store,.snap = s->snap};
	s->opts.leaves = scan_leaves(s, &s->opts.nleaves);
	s->opts.nan_leaves = numeric_leaves(s);
	s->vet = in->is_table ? NULL : vet_new(cx, s->heap, s->snap);
	s->fslot = table_slot_create(s->heap, NULL);
	s->has_quals = quals != NULL;
	if (quals != NULL)
		s->quals = *quals;
	s->vctx = sift_vexpr_ctx_new(cx);
	vectors_new(s);
	s->scan = sift_f2_store_scan_open(&s->opts);
	MemoryContextSwitchTo(old);
	s->base = (SiftV6Source) {.next = src_next,.fetch = src_fetch,.close = src_close};
	return &s->base;
}
