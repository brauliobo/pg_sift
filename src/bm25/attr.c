/*
 * bm25/attr.c - the values of the INCLUDE columns of a sift index, read from the heap tuple of each doc when a segment is written
 * (bm25/segment.h, SIFT_SEGFMT_ATTR). A tuple never changes, so the value a doc had when its segment was written is the one it keeps; a tuple that
 * is gone (pruned, not yet removed from the index) leaves the doc "unknown", which passes every test and is checked on the heap as before.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bm25/planner.h"
#include "bm25/segment.h"
#include "core/store.h"

typedef struct HeapAttrs
{
	Relation	heap;
	TupleDesc	desc;
	int			ncols;
	AttrNumber	attno[SIFT_ATTR_MAXCOLS];
	Oid			type[SIFT_ATTR_MAXCOLS];
	SiftAttrSrc src;
	MemoryContextCallback cb;
} HeapAttrs;

static bool
intlike(Oid t)
{
	return t == BOOLOID || t == INT2OID || t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

int64
sift_attr_datum(Oid t, Datum d)
{
	switch (t)
	{
		case BOOLOID:
			return DatumGetBool(d) ? 1 : 0;
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static void
heap_fetch_attrs(void *arg, BlockNumber blk, const OffsetNumber *offs, int n, int64 *vals, uint8 *state)
{
	HeapAttrs  *h = arg;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	OffsetNumber maxoff = 0;

	for (int i = 0; i < n * h->ncols; i++)
		state[i] = SIFT_AV_UNKNOWN;
	if (blk < RelationGetNumberOfBlocks(h->heap))
	{
		buf = ReadBuffer(h->heap, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoff = PageGetMaxOffsetNumber(page);
	}
	for (int i = 0; i < n && buf != InvalidBuffer; i++)
	{
		ItemId		lp;
		HeapTupleData tup;

		if (offs[i] > maxoff || !ItemIdIsNormal(lp = PageGetItemId(page, offs[i])))
			continue;
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(h->heap);
		ItemPointerSet(&tup.t_self, blk, offs[i]);
		for (int c = 0; c < h->ncols; c++)
		{
			bool		isnull;
			Datum		d;

			if (!h->src.intlike[c])
				continue;
			d = heap_getattr(&tup, h->attno[c], h->desc, &isnull);
			vals[c * n + i] = isnull ? 0 : sift_attr_datum(h->type[c], d);
			state[c * n + i] = isnull ? SIFT_AV_NULL : SIFT_AV_VALUE;
		}
	}
	if (buf != InvalidBuffer)
		UnlockReleaseBuffer(buf);
}

static void
heap_attrs_close(void *arg)
{
	table_close(((HeapAttrs *) arg)->heap, AccessShareLock);
}

SiftAttrSrc *
sift_attr_src(SiftWriteTarget *wt, MemoryContext mcxt)
{
	Relation	index = sift_wt_index(wt);
	Form_pg_index ix = index->rd_index;
	HeapAttrs  *h;
	int			nkey = IndexRelationGetNumberOfKeyAttributes(index);

	if (IndexRelationGetNumberOfAttributes(index) <= nkey)
		return NULL;
	h = MemoryContextAllocZero(mcxt, sizeof(HeapAttrs));
	h->heap = table_open(ix->indrelid, AccessShareLock);
	if (h->heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift index \"%s\": INCLUDE columns need a heap table", RelationGetRelationName(index))));
	h->desc = RelationGetDescr(h->heap);
	h->cb.func = heap_attrs_close;
	h->cb.arg = h;
	MemoryContextRegisterResetCallback(mcxt, &h->cb);
	h->ncols = Min(IndexRelationGetNumberOfAttributes(index) - nkey, SIFT_ATTR_MAXCOLS);
	for (int c = 0; c < h->ncols; c++)
	{
		h->attno[c] = ix->indkey.values[nkey + c];
		h->type[c] = TupleDescAttr(h->desc, h->attno[c] - 1)->atttypid;
		h->src.intlike[c] = intlike(h->type[c]);
	}
	h->src.ncols = h->ncols;
	h->src.fetch = heap_fetch_attrs;
	h->src.arg = h;
	return &h->src;
}
