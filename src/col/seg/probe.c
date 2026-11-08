/*
 * col/seg/probe.c - the heap rows a compact block of the global order reads its exact keys from (col/seg/gorder.c is portable and reaches them through the hooks of seg_int.h).
 */
#include "postgres.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "storage/bufpage.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "core/cmp.h"
#include "col/seg_int.h"

/* The heap rows a window of keys is read from (the relation is opened for the window and closed with it) */
typedef struct HeapProbe
{
	Relation	rel;
	const SiftKey *dead;		/* the rows of the segment that bulkdelete cleared, ascending: a vacuumed address may hold the tuple of another row */
	uint32		ndead;
	BlockNumber nblocks;
	AttrNumber	att;
	bool		wide;			/* the key is the 8 bytes of the datum, else 4 or 2 */
	bool		two;
} HeapProbe;

/*
 * Whether the keys a compact block does not store can be read from the heap (col is the leaf of the block, or ncols + the leaf for its second order, the one sorted by group): the leaf is a column of a heap table of the type its key is made of (the address of a row that
 * bulkdelete cleared may have a new tenant, so such a row is answered as dead without the heap, see probe_dead). A block that cannot is not used: its scans read the units.
 */
static bool
probe_ok(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	const SiftColDef *d;
	Form_pg_index ix = seg->index->rd_index;
	Relation	heap;
	Oid			ty = InvalidOid;
	bool		ok = false;

	if (col >= 2 * seg->schema->ncols || ((seg->entry.flags & SIFT_SEG_DIRTY) && !sift_cseg_dead_fits(seg)))
		return false;
	d = &seg->schema->cols[col % seg->schema->ncols];
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


/* The addresses of the rows of the segment that bulkdelete cleared, listed on first use and kept with the leaf */
static void
probe_dead(SiftColSegment *seg, GOrdLeaf *g)
{
	uint32		room = 0;
	uint64	   *dirty = palloc(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));

	sift_cseg_dirty_units(seg, dirty);
	for (uint32 u = 0; u < seg->nsb; u++)
		if (sift_col_bit_test(dirty, u) && !sift_cseg_unit_dead(seg, u, seg->mcxt, &g->dead, &g->ndead, &room))
			corrupt(seg, "The head of unit %u cannot be read.", u);
	pfree(dirty);
	if (g->ndead > 1)
		qsort(g->dead, g->ndead, sizeof(SiftKey), sift_cmp_u64);
	g->deadset = true;
}

static void *
probe_open(SiftColSegment *seg, GOrdLeaf *g, int col)
{
	HeapProbe  *pr = palloc(sizeof(HeapProbe));
	const SiftColDef *d;

	pr->rel = RelationIdGetRelation(g->hrel);
	if (!RelationIsValid(pr->rel))
	{
		pfree(pr);
		return NULL;
	}
	if ((seg->entry.flags & SIFT_SEG_DIRTY) && !g->deadset)
		probe_dead(seg, g);
	pr->dead = g->dead;
	pr->ndead = g->ndead;
	pr->nblocks = RelationGetNumberOfBlocks(pr->rel);
	pr->att = g->hatt;
	d = &seg->schema->cols[col % seg->schema->ncols];
	pr->two = d->type == SIFT_COLT_INT2;
	pr->wide = !pr->two && d->type != SIFT_COLT_INT4 && d->type != SIFT_COLT_DATE;
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

	if (blk >= pr->nblocks || (pr->ndead > 0 && bsearch(&addr, pr->dead, pr->ndead, sizeof(SiftKey), sift_cmp_u64) != NULL))
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
