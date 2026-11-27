/*
 * bm25/facet/rls.c - the matches of sift.facets() that the row-level security policies of the caller let it see (bm25/facet/rls.h).
 *
 * The matches of a batch of pages are resolved to the visible version of each row (the HOT chain a row came from), and the policies decide which of those the caller may read: one
 * SELECT of the table by those addresses, planned and run as the caller's own, so the answer is the one any statement of the caller gets. The batches are bounded, so the memory
 * is: a page waits here until its batch is decided, and then goes on to the facet's bitmaps in the order the pages came.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "bm25/facet/rls.h"
#include "core/memtrack.h"
#include "core/sql/sql.h"

#define SIFT_FACET_RLS_BATCH	4096	/* matches decided by one statement */

typedef struct QueuedPage
{
	BlockNumber blk;
	uint64		mask[SIFT_OFFMASK_WORDS];
} QueuedPage;

struct SiftFacetRls
{
	Relation	heap;
	MemoryContext mcxt;
	SiftVis		vis;
	TupleTableSlot *slot;
	char	   *query;
	SiftFacetRlsFn out;
	void	   *arg;
	QueuedPage *pages;
	int			npages;
	int			pagecap;
	int			nbits;			/* the matches queued */
	int64		passed;
};

static inline uint64
pack_tid(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

SiftFacetRls *
sift_facet_rls_begin(Relation heap, Snapshot snapshot, MemoryContext mcxt, SiftFacetRlsFn out, void *arg)
{
	SiftFacetRls *r;

	if (check_enable_rls(RelationGetRelid(heap), InvalidOid, false) != RLS_ENABLED)
		return NULL;
	r = MemoryContextAllocZero(mcxt, sizeof(SiftFacetRls));
	r->heap = heap;
	r->mcxt = mcxt;
	r->out = out;
	r->arg = arg;
	r->slot = table_slot_create(heap, NULL);
	r->query = MemoryContextStrdup(mcxt, psprintf("SELECT ctid FROM ONLY %s WHERE ctid OPERATOR(pg_catalog.=) ANY ($1)",
												  quote_qualified_identifier(get_namespace_name(RelationGetNamespace(heap)), RelationGetRelationName(heap))));
	sift_vis_begin(&r->vis, heap, snapshot, NULL, 0);
	return r;
}

/* The version of every queued match that a read of the caller's snapshot returns, in the order of the queue, 0 for a match that has none; how many have one */
static int
resolve_versions(SiftFacetRls *r, uint64 *seen)
{
	int			n = 0,
				found = 0;

	for (int i = 0; i < r->npages; i++)
		for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
			for (uint64 m = r->pages[i].mask[w]; m; m &= m - 1)
			{
				ItemPointerData tid;

				ItemPointerSet(&tid, r->pages[i].blk, (OffsetNumber) (w * 64 + pg_rightmost_one_pos64(m) + 1));
				seen[n] = sift_vis_fetch(&r->vis, &tid, r->slot) ? pack_tid(&tid) : 0;
				found += seen[n++] != 0;
			}
	return found;
}

/* The versions the policies of the caller show, sorted */
static uint64 *
readable_versions(SiftFacetRls *r, const uint64 *seen, int nfound, int *nreadable)
{
	Datum	   *elems = palloc(nfound * sizeof(Datum));
	ItemPointerData *tids = palloc(nfound * sizeof(ItemPointerData));
	Oid			argtype = TIDARRAYOID;
	Datum		arg;
	uint64	   *readable;
	int			n = 0;

	for (int i = 0; i < r->nbits; i++)
		if (seen[i] != 0)
		{
			ItemPointerSet(&tids[n], (BlockNumber) (seen[i] >> 16), (OffsetNumber) (seen[i] & 0xFFFF));
			elems[n] = PointerGetDatum(&tids[n]);
			n++;
		}
	arg = PointerGetDatum(construct_array(elems, n, TIDOID, sizeof(ItemPointerData), false, TYPALIGN_SHORT));
	SIFT_INVARIANT(SPI_connect() == SPI_OK_CONNECT, "sift.facets: SPI_connect failed");
	SIFT_INVARIANT(SPI_execute_with_args(r->query, 1, &argtype, &arg, NULL, true, 0) == SPI_OK_SELECT, "sift.facets: the row-level security read failed");
	*nreadable = (int) SPI_processed;
	readable = MemoryContextAlloc(r->mcxt, Max(*nreadable, 1) * sizeof(uint64));
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		Datum		d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);

		readable[i] = pack_tid(DatumGetItemPointer(d));
	}
	SPI_finish();
	qsort(readable, *nreadable, sizeof(uint64), cmp_u64);
	pfree(elems);
	pfree(tids);
	return readable;
}

static void
decide_batch(SiftFacetRls *r)
{
	int			nreadable = 0,
				n = 0;
	uint64	   *seen,
			   *readable = NULL;

	if (r->npages == 0)
		return;
	seen = MemoryContextAlloc(r->mcxt, r->nbits * sizeof(uint64));
	if (resolve_versions(r, seen) > 0)
		readable = readable_versions(r, seen, r->nbits, &nreadable);
	for (int i = 0; i < r->npages; i++)
	{
		uint64		kept[SIFT_OFFMASK_WORDS] = {0};
		int			any = 0;

		for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
			for (uint64 m = r->pages[i].mask[w]; m; m &= m - 1, n++)
				if (seen[n] != 0 && bsearch(&seen[n], readable, nreadable, sizeof(uint64), cmp_u64) != NULL)
				{
					kept[w] |= UINT64CONST(1) << pg_rightmost_one_pos64(m);
					any++;
				}
		if (any > 0)
		{
			r->passed += any;
			r->out(r->arg, r->pages[i].blk, kept);
		}
	}
	if (readable != NULL)
		pfree(readable);
	pfree(seen);
	r->npages = 0;
	r->nbits = 0;
}

void
sift_facet_rls_page(SiftFacetRls *r, BlockNumber blk, const uint64 *offmask)
{
	if (r->npages == r->pagecap)
	{
		r->pagecap = Max(64, 2 * r->pagecap);
		r->pages = r->pages == NULL ? MemoryContextAlloc(r->mcxt, r->pagecap * sizeof(QueuedPage)) : repalloc(r->pages, r->pagecap * sizeof(QueuedPage));
	}
	r->pages[r->npages].blk = blk;
	memcpy(r->pages[r->npages].mask, offmask, sizeof(r->pages[0].mask));
	r->npages++;
	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
		r->nbits += pg_popcount64(offmask[w]);
	if (r->nbits >= SIFT_FACET_RLS_BATCH)
		decide_batch(r);
}

int64
sift_facet_rls_finish(SiftFacetRls *r)
{
	int64		passed;

	decide_batch(r);
	passed = r->passed;
	sift_vis_end(&r->vis);
	ExecDropSingleTupleTableSlot(r->slot);
	if (r->pages != NULL)
		pfree(r->pages);
	pfree(r->query);
	pfree(r);
	return passed;
}
