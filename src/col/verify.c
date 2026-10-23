/*
 * col/verify.c - sift.col_verify(index, heapallindexed): the structural check of the store, every segment and write buffer, the
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
#include "utils/relcache.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/seg.h"
#include "core/cmp.h"

#define corrupt SIFT_CORRUPT

/*
 * A row's values (hash), and the types of its overflow values apart (tags): a value stored under minor 3 has none, and then the row's types are
 * not compared (known is false). A string the unit's byte budget turned into an overflow has no type either, and the heap still holds the string:
 * a row with one (strovf) is compared by the hash of its other leaves (rest).
 */
typedef struct RowHash
{
	SiftKey		key;
	uint64		hash;
	uint64		rest;
	uint64		tags;
	bool		known;
	bool		strovf;
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

static void
value_hashes(RowHash *rh, const SiftColDef *def, const SiftColValue *v)
{
	rh->hash = value_hash(rh->hash, def, v);
	if (def->store != SIFT_COLS_STR && def->store != SIFT_COLS_VAR)
		rh->rest = value_hash(rh->rest, def, v);
	else if (v->overflow && v->tag == SIFT_COLTAG_UNKNOWN)
		rh->strovf = true;
	if (v->overflow)
	{
		rh->tags = hash_combine64(rh->tags, murmurhash64(v->tag));
		rh->known &= v->tag != SIFT_COLTAG_UNKNOWN;
	}
}

/* The hashes of a row, whether it comes from the heap (the index tuple formed again) or from the index (decoded) */
static RowHash
row_hash(const SiftColSchema *sc, const SiftColRow *row, SiftKey key)
{
	RowHash		rh = {.key = key, .known = true};

	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if (def->parent < 0)
			value_hashes(&rh, def, &row->val[c]);
		else
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			for (int64 i = 0; i < n; i++)
				value_hashes(&rh, def, &row->elem[c][i]);
		}
	}
	return rh;
}

/*
 * The lanes of a wide value agree: one NULL and overflow flag for all, a numeric image is a canonical image and its
 * display scale one PostgreSQL has, an interval's total is the one its time, day and month give
 */
static void
check_wide(Relation index, const SiftColSchema *sc, const SiftColRow *row)
{
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		const SiftColValue *v = &row->val[c];

		if (def->lanes < 2)
			continue;
		for (int k = 1; k < def->lanes; k++)
			if (row->val[c + k].isnull != v->isnull || row->val[c + k].overflow != v->overflow)
				corrupt(index, "The lanes of leaf %d disagree on whether a value is NULL.", c);
		if (v->isnull)
			continue;
		if (def->store == SIFT_COLS_STR)
		{
			if (!sift_col_numimg_valid((const uint8 *) v->v.s, v->len))
				corrupt(index, "Leaf %d holds a numeric image that is not canonical.", c);
			if (row->val[c + 1].v.i < 0 || row->val[c + 1].v.i > SIFT_NIMG_DSCALE_MAX)
				corrupt(index, "Leaf %d holds a display scale of %lld.", c, (long long) row->val[c + 1].v.i);
		}
		else if (def->type == SIFT_COLT_INTERVAL)
		{
			sift_w128	total = sift_col_wide_join(v->v.i, row->val[c + 1].v.i);
			int64		time;
			int32		day,
						month;

			sift_col_interval_raw(total, row->val[c + 2].v.i, &time, &day, &month);
			if (sift_col_interval_total(time, day, month) != total)
				corrupt(index, "Leaf %d holds an interval whose total is not that of its month, day and time.", c);
		}
	}
}

static void
collect_batch(Relation index, Collect *cl, const SiftColSchema *sc, const SiftColBatch *b)
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
		check_wide(index, sc, &row);
		cl->rows[cl->n++] = row_hash(sc, &row, b->keys[r]);
	}
}

static int
rowhash_cmp(const void *a, const void *b)
{
	SiftKey		x = ((const RowHash *) a)->key,
				y = ((const RowHash *) b)->key;

	return SIFT_CMP3(x, y);
}

#ifdef SIFT_TESTING
bool		sift_test_col_verify_inval = false;
#endif

typedef struct HeapCheck
{
	Relation	index;
	SiftColRel *r;				/* a private copy: the cached state of the index is freed whenever its relcache entry is reloaded */
	const Collect *cl;
	MemoryContext tmp;
	int64		checked;
} HeapCheck;

static void
heap_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	HeapCheck  *hc = state;
	SiftColRow	row;
	RowHash		key = {sift_key_from_tid(tid)},
				heap;
	const RowHash *found;

	if (!alive)
		return;
#ifdef SIFT_TESTING
	if (sift_test_col_verify_inval)
	{
		/* a session that updates the index's statistics sends this invalidation, which frees the cached state mid-scan */
		memset(sift_col_rel(hc->index), 0x7f, sizeof(SiftColRel));
		RelationCacheInvalidateEntry(RelationGetRelid(hc->index));
	}
#endif
	found = bsearch(&key, hc->cl->rows, hc->cl->n, sizeof(RowHash), rowhash_cmp);
	if (found == NULL)
		corrupt(hc->index, "Heap tuple (%u,%u) is not in the index.", ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
	sift_col_make_row(hc->r, tid, values, isnull, &row, hc->tmp);
	heap = row_hash(hc->r->schema, &row, key.key);
	if (found->strovf ? found->rest != heap.rest : found->hash != heap.hash)
		corrupt(hc->index, "The values of heap tuple (%u,%u) differ from the index's.", ItemPointerGetBlockNumber(tid),
				ItemPointerGetOffsetNumber(tid));
	if (found->known && found->tags != heap.tags)
		corrupt(hc->index, "The types of the overflow values of heap tuple (%u,%u) differ from the index's.", ItemPointerGetBlockNumber(tid),
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
	r = sift_col_rel_private(index, cxt);
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
				collect_batch(index, &cl, r->schema, batch);
			}
			sift_cseg_close(seg);
		}
		else
		{
			int			n;
			SiftColBatch **bs;

			sift_cwbuf_verify(index, e, r->schema);
			bs = sift_cwbuf_batches(index, e, r->schema, cxt, &n);
			for (int j = 0; j < n; j++)
				collect_batch(index, &cl, r->schema, bs[j]);
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
