/*
 * col/seg/probe.c - the heap rows a compact block of the global order reads its exact keys from (col/seg/gorder.c is portable and reaches them through the hooks of seg_int.h).
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "storage/bufpage.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "col/seg_int.h"

/* The heap rows a window of keys is read from (the relation is opened for the window and closed with it) */
typedef struct HeapProbe
{
	Relation	rel;
	BlockNumber nblocks;
	AttrNumber	att;
	bool		wide;			/* the key is the 8 bytes of the datum, else 4 or 2 */
	bool		two;
} HeapProbe;

/*
 * Whether the keys a compact block does not store can be read from the heap: the segment has no row that bulkdelete cleared (the address of such a row may have a new tenant),
 * and the leaf is a column of a heap table of the type its key is made of. A block that cannot is not used: its scans read the units.
 */
static bool
probe_ok(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	const SiftColDef *d;
	Form_pg_index ix = seg->index->rd_index;
	Relation	heap;
	Oid			ty = InvalidOid;
	bool		ok = false;

	if ((seg->entry.flags & SIFT_SEG_DIRTY) || col >= seg->schema->ncols)
		return false;
	d = &seg->schema->cols[col];
	if (d->attno < 1 || d->attno > ix->indnatts || ix->indkey.values[d->attno - 1] <= 0 || !sift_cgord_leaf_ok(seg->schema, d))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
			ty = INT2OID;
			break;
		case SIFT_COLT_INT4:
			ty = INT4OID;
			break;
		case SIFT_COLT_INT8:
			ty = INT8OID;
			break;
		case SIFT_COLT_DATE:
			ty = DATEOID;
			break;
		case SIFT_COLT_TIMESTAMP:
			ty = TIMESTAMPOID;
			break;
		case SIFT_COLT_TIMESTAMPTZ:
			ty = TIMESTAMPTZOID;
			break;
		default:
			return false;
	}
	g->hatt = ix->indkey.values[d->attno - 1];
	g->hrel = ix->indrelid;
	heap = RelationIdGetRelation(g->hrel);
	if (!RelationIsValid(heap))
		return false;
	if (heap->rd_rel->relkind == RELKIND_RELATION && heap->rd_rel->relam == HEAP_TABLE_AM_OID && g->hatt <= RelationGetDescr(heap)->natts &&
		!TupleDescAttr(RelationGetDescr(heap), g->hatt - 1)->attisdropped && TupleDescAttr(RelationGetDescr(heap), g->hatt - 1)->atttypid == ty)
		ok = true;
	RelationClose(heap);
	return ok;
}


static void *
probe_open(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	HeapProbe  *pr = palloc(sizeof(HeapProbe));

	pr->rel = RelationIdGetRelation(g->hrel);
	if (!RelationIsValid(pr->rel))
	{
		pfree(pr);
		return NULL;
	}
	pr->nblocks = RelationGetNumberOfBlocks(pr->rel);
	pr->att = g->hatt;
	pr->two = seg->schema->cols[col].type == SIFT_COLT_INT2;
	pr->wide = !pr->two && seg->schema->cols[col].type != SIFT_COLT_INT4 && seg->schema->cols[col].type != SIFT_COLT_DATE;
	return pr;
}

static void
probe_close(void *h)
{
	HeapProbe  *pr = h;

	RelationClose(pr->rel);
	pfree(pr);
}

/*
 * The key of the row at addr, read from its heap tuple: a redirect is followed (the versions of a chain keep the indexed columns), a dead or missing tuple or an
 * attribute with no value gives false.
 */
static bool
probe_key(void *h, uint64 addr, int64 *key)
{
	const HeapProbe *pr = h;
	BlockNumber blk = (BlockNumber) (addr >> 16);
	Buffer		b;
	Page		page;
	bool		ok = false;

	if (blk >= pr->nblocks)
		return false;
	b = ReadBuffer(pr->rel, blk);
	LockBuffer(b, BUFFER_LOCK_SHARE);
	page = BufferGetPage(b);
	if (!PageIsNew(page) && !PageIsEmpty(page))
	{
		OffsetNumber off = (OffsetNumber) (addr & 0xFFFF),
					maxoff = PageGetMaxOffsetNumber(page);

		for (int hop = 0; hop < 2 && off >= FirstOffsetNumber && off <= maxoff; hop++)
		{
			ItemId		lp = PageGetItemId(page, off);

			if (ItemIdIsRedirected(lp))
			{
				off = ItemIdGetRedirect(lp);
				continue;
			}
			if (ItemIdIsNormal(lp) && ItemIdGetLength(lp) >= SizeofHeapTupleHeader)
			{
				HeapTupleData tup;
				bool		isnull;
				Datum		v;

				tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
				tup.t_len = ItemIdGetLength(lp);
				v = heap_getattr(&tup, pr->att, RelationGetDescr(pr->rel), &isnull);
				if (!isnull)
				{
					*key = pr->wide ? DatumGetInt64(v) : pr->two ? (int64) DatumGetInt16(v) : (int64) DatumGetInt32(v);
					ok = true;
				}
			}
			break;
		}
	}
	UnlockReleaseBuffer(b);
	return ok;
}


void
sift_cseg_probe_init(void)
{
	sift_gprobe_ok = probe_ok;
	sift_gprobe_open = probe_open;
	sift_gprobe_close = probe_close;
	sift_gprobe_key = probe_key;
}
