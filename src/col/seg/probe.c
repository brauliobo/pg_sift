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
#include "col/am.h"
#include "col/seg_int.h"

int64		sift_col_probe_dead;

/* The rows of a unit (of the segment, cut by key) that bulkdelete cleared, ascending, once listed: a vacuumed address may hold the tuple of another row */
typedef struct ProbeDead
{
	bool		set;
	uint32		n;
	SiftKey    *dead;
} ProbeDead;

/* The heap rows a window of keys is read from (the relation is opened for the window and closed with it) */
typedef struct HeapProbe
{
	Relation	rel;
	SiftColSegment *seg;
	ProbeDead  *dead;			/* NULL: the segment has no row cleared */
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


/*
 * The dead rows a key of block blk is asked of: those of its unit, listed on the first ask (a statement reads the pages of a few units, and a table with tens of thousands of dead
 * rows would otherwise list and sort them all), or, in a segment cut by key, which has no unit of a page, those of all its units sorted.
 */
static const ProbeDead *
probe_dead(const HeapProbe *pr, BlockNumber blk)
{
	SiftColSegment *seg = pr->seg;
	bool		keyed = sift_cseg_keyed(&seg->entry);
	uint32		u = keyed ? 0 : sift_cseg_unit_of_block(seg, blk),
				end = keyed ? seg->nsb : u + 1,
				room = 0;
	ProbeDead  *d;

	if (u >= seg->nsb)
		return NULL;
	d = &pr->dead[u];
	if (d->set)
		return d;
	for (uint32 k = u; k < end; k++)
		if (!sift_cseg_unit_dead(seg, k, seg->mcxt, &d->dead, &d->n, &room))
			corrupt(seg, "The head of unit %u cannot be read.", k);
	if (keyed && d->n > 1)
		qsort(d->dead, d->n, sizeof(SiftKey), sift_cmp_u64);
	sift_col_probe_dead += d->n;
	d->set = true;
	return d;
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
	pr->seg = seg;
	if (seg->entry.flags & SIFT_SEG_DIRTY)
	{
		if (g->pdead == NULL)
			g->pdead = MemoryContextAllocZero(seg->mcxt, (sift_cseg_keyed(&seg->entry) ? 1 : seg->nsb) * sizeof(ProbeDead));
		pr->dead = g->pdead;
	}
	else
		pr->dead = NULL;
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

	if (blk >= pr->nblocks)
		return false;
	if (pr->dead)
	{
		const ProbeDead *d = probe_dead(pr, blk);

		if (d && d->n > 0 && bsearch(&addr, d->dead, d->n, sizeof(SiftKey), sift_cmp_u64) != NULL)
			return false;
	}
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
