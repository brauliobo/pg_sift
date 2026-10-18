/*
 * sift_col_verify.c - sift.col_verify(index, heapallindexed): the structural check of the store, every segment and mutable lane, the
 * totals of the metapage, that no live row is in the index twice and, with heapallindexed, that every heap tuple visible to a snapshot is
 * in the index with the very values the index tuple forms.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"

#define corrupt SIFT_CORRUPT

typedef struct RowHash
{
	SiftKey		key;
	uint64		hash;
} RowHash;

typedef struct Collect
{
	RowHash    *rows;
	int64		n;
	int64		cap;
} Collect;

static uint64
value_hash(uint64 h, const SiftColDef *def, const SiftColValue *v)
{
	uint64		x;

	if (v->overflow)
		x = 2;
	else if (v->isnull)
		x = 1;
	else if (def->store == SIFT_COLS_STR)
		x = hash_bytes_extended((const unsigned char *) v->v.s, v->len, 3);
	else if (def->store == SIFT_COLS_INT)
		x = (uint64) v->v.i;
	else
		x = (uint64) sift_col_f8_key(v->v.f);
	return hash_combine64(h, murmurhash64(x));
}

/* One hash for a row, whether it comes from the heap (the index tuple formed again) or from the index (decoded) */
static uint64
row_hash(const SiftColSchema *sc, const SiftColRow *row)
{
	uint64		h = 0;

	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if (def->parent < 0)
			h = value_hash(h, def, &row->val[c]);
		else
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			for (int64 i = 0; i < n; i++)
				h = value_hash(h, def, &row->elem[c][i]);
		}
	}
	return h;
}

static void
collect_batch(Collect *cl, const SiftColSchema *sc, const SiftColBatch *b)
{
	SiftColRow	row = {0};

	for (uint32 r = 0; r < b->nrows; r++)
	{
		if (cl->n == cl->cap)
		{
			cl->cap = Max(1024, cl->cap * 2);
			cl->rows = cl->rows ? repalloc(cl->rows, cl->cap * sizeof(RowHash)) : palloc(cl->cap * sizeof(RowHash));
		}
		sift_cbatch_row(b, r, &row);
		cl->rows[cl->n++] = (RowHash) {b->keys[r], row_hash(sc, &row)};
	}
}

static int
rowhash_cmp(const void *a, const void *b)
{
	SiftKey		x = ((const RowHash *) a)->key,
				y = ((const RowHash *) b)->key;

	return x < y ? -1 : x > y;
}

typedef struct HeapCheck
{
	Relation	index;
	SiftColRel *r;
	const Collect *cl;
	MemoryContext tmp;
	int64		checked;
} HeapCheck;

static void
heap_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	HeapCheck  *hc = state;
	SiftColRow	row;
	RowHash		key = {sift_key_from_tid(tid), 0};
	const RowHash *found;

	if (!alive)
		return;
	found = bsearch(&key, hc->cl->rows, hc->cl->n, sizeof(RowHash), rowhash_cmp);
	if (found == NULL)
		corrupt(hc->index, "Heap tuple (%u,%u) is not in the index.", ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
	sift_col_make_row(hc->r, tid, values, isnull, &row, hc->tmp);
	if (found->hash != row_hash(hc->r->schema, &row))
		corrupt(hc->index, "The values of heap tuple (%u,%u) differ from the index's.", ItemPointerGetBlockNumber(tid),
				ItemPointerGetOffsetNumber(tid));
	hc->checked++;
	MemoryContextReset(hc->tmp);
}

Datum
sift_col_verify(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	bool		heapallindexed = PG_GETARG_BOOL(1);
	SiftColRel *r;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col verify", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	Snapshot	snapshot = heapallindexed ? RegisterSnapshot(GetTransactionSnapshot()) : InvalidSnapshot;
	Collect		cl = {0};
	SiftColBatch *batch;
	uint64		ndocs = 0;

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	r = sift_col_rel(index);
	batch = sift_cbatch_create(cxt, r->schema);
	/* Keeps promotion, merges and the sweep from releasing what the directory snapshot references */
	LockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	sift_store_verify(index);
	sift_dir_read(index, snap);
	if (!(snap->meta.flags & SIFT_COL_META_FLAG) || snap->meta.analyzer_hash != r->schema->hash)
		corrupt(index, "The metapage does not describe this column layout.");
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, cxt);
			uint64		live = sift_cseg_verify(seg);

			if (live != sift_cseg_live_rows(seg))
				corrupt(index, "Segment %llu holds %llu live rows, its liveness says %llu.", (unsigned long long) e->gen,
						(unsigned long long) live, (unsigned long long) sift_cseg_live_rows(seg));
			ndocs += e->ndocs;
			for (uint32 u = 0; u < seg->nsb; u++)
			{
				sift_cseg_decode_unit(seg, u, batch);
				collect_batch(&cl, r->schema, batch);
			}
			sift_cseg_close(seg);
		}
		else
		{
			int			n;
			SiftColBatch **bs;

			sift_cmut_verify(index, e, r->schema);
			bs = sift_cmut_batches(index, e, r->schema, cxt, &n);
			for (int j = 0; j < n; j++)
				collect_batch(&cl, r->schema, bs[j]);
		}
	}
	if (ndocs != snap->meta.ndocs)
		corrupt(index, "Metapage totals %llu rows, segments %llu.", (unsigned long long) snap->meta.ndocs, (unsigned long long) ndocs);
	if (cl.n > 0)
		qsort(cl.rows, cl.n, sizeof(RowHash), rowhash_cmp);
	for (int64 i = 1; i < cl.n; i++)
		if (cl.rows[i].key == cl.rows[i - 1].key)
			corrupt(index, "Row (%u,%u) is live in the index twice.", sift_key_block(cl.rows[i].key), (uint32) (cl.rows[i].key & 0xFFFF));
	if (heapallindexed)
	{
		Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
		IndexInfo  *info = BuildIndexInfo(index);
		HeapCheck	hc = {index, r, &cl, AllocSetContextCreate(cxt, "sift_col verify tuple", ALLOCSET_DEFAULT_SIZES), 0};

		info->ii_Concurrent = true;
		table_index_build_scan(heap, index, info, false, false, heap_cb, &hc, NULL);
		table_close(heap, AccessShareLock);
		UnregisterSnapshot(snapshot);
	}
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}
