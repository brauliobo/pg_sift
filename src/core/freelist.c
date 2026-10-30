/*
 * core/freelist.c - the free list: walks and counts, space mode and the reuse horizon
 * (P2-P4 and P7 in core/store.h).
 */
#include "freelist_int.h"

static SiftFreeListData *
check_freelist(Relation index, Buffer buf, BlockNumber hwm)
{
	Page		page = BufferGetPage(buf);
	SiftFreeListData *fl = FL(page);

	sift_store_check_page(index, buf, SIFT_PT_FREELIST, 0);
	if (fl->n > SIFT_FREELIST_CAP || SiftPageGetUsed(page) != FL_USED(fl->n))
		corrupt(index, "Free list block %u holds %u records in %u bytes.",
				BufferGetBlockNumber(buf), fl->n, SiftPageGetUsed(page));
	for (uint32 i = 0; i < fl->n; i++)
		if (fl->ext[i].nblocks == 0 || fl->ext[i].start < FIRST_DATA_BLKNO ||
			(uint64) fl->ext[i].start + fl->ext[i].nblocks > hwm)
			corrupt(index, "Free list block %u has record %u+%u past the high-water mark %u.",
					BufferGetBlockNumber(buf), fl->ext[i].start, fl->ext[i].nblocks, hwm);
	return fl;
}

void
sift_freelist_walk(Relation index, Buffer metabuf, SiftFreeListVisit visit, void *arg)
{
	SiftMetaPageData *meta = META(BufferGetPage(metabuf));
	BlockNumber blk = meta->freelist_head;

	for (uint32 hops = 0; BlockNumberIsValid(blk); hops++)
	{
		Buffer		buf;
		SiftFreeListData *fl;
		BlockNumber next;
		bool		more;

		if (blk < FIRST_DATA_BLKNO || blk >= meta->high_water || hops >= meta->high_water)
			corrupt(index, "Free list chain reaches block %u after %u pages.", blk, hops);
		buf = read_locked(index, blk, BUFFER_LOCK_SHARE);
		fl = check_freelist(index, buf, meta->high_water);
		next = fl->next;
		more = visit(blk, fl, arg);
		UnlockReleaseBuffer(buf);
		if (!more)
			break;
		blk = next;
	}
}

typedef struct FreeCount
{
	Relation	index;
	SiftFreeStats fs;
} FreeCount;

static bool
count_free(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreeCount  *c = arg;

	c->fs.pages++;
	for (uint32 i = 0; i < fl->n; i++)
	{
		c->fs.records++;
		c->fs.blocks += fl->ext[i].nblocks;
		if (!FullTransactionIdIsValid(fl->ext[i].safexid) || sift_store_removable(c->index, fl->ext[i].safexid))
			c->fs.reusable += fl->ext[i].nblocks;
	}
	return true;
}

/* The free list counted with the metapage share-locked, like the allocator that changes it */
SiftFreeStats
sift_store_free_stats(Relation index)
{
	FreeCount	c = {.index = index};
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);

	sift_freelist_walk(index, metabuf, count_free, &c);
	UnlockReleaseBuffer(metabuf);
	return c.fs;
}

typedef struct FreeBelow
{
	Relation	index;
	BlockNumber limit;
	uint32		min_run;
	int64		blocks;
} FreeBelow;

static bool
count_below(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreeBelow  *c = arg;

	for (uint32 i = 0; i < fl->n; i++)
	{
		uint32		below;

		if (fl->ext[i].start >= c->limit || fl->ext[i].nblocks < c->min_run)
			continue;
		if (FullTransactionIdIsValid(fl->ext[i].safexid) && !sift_store_removable(c->index, fl->ext[i].safexid))
			continue;
		below = Min(fl->ext[i].nblocks, c->limit - fl->ext[i].start);
		if (below >= c->min_run)
			c->blocks += below;
	}
	return true;
}

/* The reusable free blocks in runs of at least min_run that lie before limit (a run across it counts the part before it): what a segment above limit can be rewritten into */
int64
sift_store_free_below(Relation index, BlockNumber limit, uint32 min_run)
{
	FreeBelow	c = {index, limit, min_run, 0};
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);

	sift_freelist_walk(index, metabuf, count_below, &c);
	UnlockReleaseBuffer(metabuf);
	return c.blocks;
}

/*
 * Space mode (sift_store_space_exclusive): the caller holds AccessExclusiveLock on the relation (an ExclusiveLock on an index that is not ready), so no scan, directory copy or write target
 * of another backend exists and every released extent is reusable at once, whatever its safexid (P7 reasons the same way for the cut).
 */
uint64 space_written = 0;
bool space_exclusive = false;
BlockNumber space_limit = InvalidBlockNumber;

void
sift_store_space_exclusive(Relation index, bool on)
{
	Assert(!on || CheckRelationLockedByMe(index, ExclusiveLock, true));
	space_exclusive = on;
	if (!on)
		space_limit = InvalidBlockNumber;
}

uint64
sift_store_space_written(void)
{
	return space_written;
}

/* The GlobalVis horizon is the heap's: it asserts a table, not the index. A sift_table is its own table */
bool
sift_store_removable(Relation index, FullTransactionId xid)
{
	Relation	heap;
	bool		removable;

	if (space_exclusive)
		return true;
	if (index->rd_rel->relkind != RELKIND_INDEX)
		return GlobalVisCheckRemovableFullXid(index, xid);
	heap = RelationIdGetRelation(index->rd_index->indrelid);
	removable = GlobalVisCheckRemovableFullXid(heap, xid);
	RelationClose(heap);
	return removable;
}

/*
 * The horizon computed anew: the one GlobalVis caches only moves when this backend takes a
 * snapshot, so a backend without one (the maintenance worker, or a merge that takes long) would not
 * see the extents it freed become reusable. Records before it are reusable.
 */
FullTransactionId
sift_store_horizon(Relation index)
{
	Relation	heap = index->rd_rel->relkind == RELKIND_INDEX ? RelationIdGetRelation(index->rd_index->indrelid) : index;
	TransactionId oldest = GetOldestNonRemovableTransactionId(heap);

	if (heap != index)
		RelationClose(heap);
	return FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), oldest);
}

