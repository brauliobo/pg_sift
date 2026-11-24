/*
 * col/f2/heap_probe.c - which entries of an index the heap has no live version for (heap_probe.h)
 *
 * The track_changes triggers of an index built concurrently are visible to a writer from the commit of the build's first pass: a writer that was running before it deletes and updates rows the
 * first pass indexed without a word to the index. The validation pass, which every such writer has finished before, asks the heap for them, as it asks it for the rows the first pass missed.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"

#include "col/f2/heap_probe.h"

void
sift_heap_probe_begin(SiftHeapProbe *pr, Relation heap)
{
	*pr = (SiftHeapProbe) {.heap = heap,.oldest = GetOldestNonRemovableTransactionId(heap),.vm = InvalidBuffer,.page = InvalidBuffer};
}

void
sift_heap_probe_end(SiftHeapProbe *pr)
{
	if (BufferIsValid(pr->vm))
		ReleaseBuffer(pr->vm);
	if (BufferIsValid(pr->page))
		ReleaseBuffer(pr->page);
}

static Page
probe_page(SiftHeapProbe *pr, BlockNumber blk)
{
	if (!BufferIsValid(pr->page) || BufferGetBlockNumber(pr->page) != blk)
	{
		if (BufferIsValid(pr->page))
			ReleaseBuffer(pr->page);
		pr->page = ReadBuffer(pr->heap, blk);
	}
	LockBuffer(pr->page, BUFFER_LOCK_SHARE);
	return BufferGetPage(pr->page);
}

/* The version at off, or the first one of its chain when the root is only a redirect; false when nothing is there */
static bool
chain_version(Page page, BlockNumber blk, OffsetNumber *off, Oid reloid, HeapTupleData *ht)
{
	ItemId		lp;

	for (;;)
	{
		if (*off > PageGetMaxOffsetNumber(page))
			return false;
		lp = PageGetItemId(page, *off);
		if (!ItemIdIsRedirected(lp))
			break;
		*off = ItemIdGetRedirect(lp);
	}
	if (!ItemIdIsNormal(lp))
		return false;
	ht->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	ht->t_len = ItemIdGetLength(lp);
	ht->t_tableOid = reloid;
	ItemPointerSet(&ht->t_self, blk, *off);
	return true;
}

bool
sift_heap_probe_gone(SiftHeapProbe *pr, uint64 tid, SiftPendDelRec *rec)
{
	const BlockNumber blk = (BlockNumber) (tid >> 16);
	OffsetNumber off = (OffsetNumber) (tid & 0xFFFF);
	Page		page;
	HeapTupleData ht;
	TransactionId xmax = InvalidTransactionId;
	bool		gone = true;

	if ((visibilitymap_get_status(pr->heap, blk, &pr->vm) & VISIBILITYMAP_ALL_VISIBLE) != 0)
		return false;
	page = probe_page(pr, blk);
	while (gone && chain_version(page, blk, &off, RelationGetRelid(pr->heap), &ht))
	{
		switch (HeapTupleSatisfiesVacuum(&ht, pr->oldest, pr->page))
		{
			case HEAPTUPLE_DEAD:
				xmax = InvalidTransactionId;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:
				xmax = HeapTupleHeaderGetUpdateXid(ht.t_data);
				break;
			default:
				gone = false;
				continue;
		}
		if (!HeapTupleHeaderIsHotUpdated(ht.t_data))
			break;
		off = ItemPointerGetOffsetNumber(&ht.t_data->t_ctid);
	}
	LockBuffer(pr->page, BUFFER_LOCK_UNLOCK);
	if (gone)
		*rec = (SiftPendDelRec) {.tid = tid,.xmax = xmax,.cmax = 0};
	return gone;
}
