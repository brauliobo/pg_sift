/*
 * core/freelist.c - the free list: walks and counts, space mode and the reuse horizon
 * (P2-P4 and P7 in core/store.h).
 */
#include "freelist_int.h"
#include "core/catname.h"
#include "core/horizon.h"

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

bool
sift_store_removable(Relation index, FullTransactionId xid)
{
	return space_exclusive || sift_horizon_removable(index, xid);
}

FullTransactionId
sift_store_horizon(Relation index)
{
	return sift_horizon(index);
}


/* ---- the free list as a profile: the probes of a bisection ---- */

struct SiftFreeProfile
{
	int			n;
	BlockNumber *start;			/* of the reusable runs of at least min_run blocks, ascending */
	BlockNumber *end;
	int64	   *before;			/* before[i]: the blocks of the runs [0, i) */
	uint32		min_run;
};

typedef struct FreeProfileScan
{
	Relation	index;
	uint32		min_run;
	SiftFreeExtent *ext;
	int			n;
	int			cap;
} FreeProfileScan;

static bool
collect_profile(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreeProfileScan *c = arg;

	for (uint32 i = 0; i < fl->n; i++)
	{
		if (fl->ext[i].nblocks < c->min_run || (FullTransactionIdIsValid(fl->ext[i].safexid) && !sift_store_removable(c->index, fl->ext[i].safexid)))
			continue;
		if (c->n == c->cap)
		{
			/* check-memory: allow the runs of the free list, which its pages bound and the walk holds under the metapage lock as the counting walks do */
			c->cap = Max(64, 2 * c->cap);
			c->ext = c->ext ? repalloc(c->ext, c->cap * sizeof(SiftFreeExtent)) : palloc(c->cap * sizeof(SiftFreeExtent));
		}
		c->ext[c->n++] = fl->ext[i];
	}
	return true;
}

static int
cmp_run_start(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftFreeExtent *) a)->start, ((const SiftFreeExtent *) b)->start);
}

/* The reusable runs of at least min_run blocks, read once: sift_free_profile_below(p, limit) is sift_store_free_below(index, limit, min_run) without the walk */
SiftFreeProfile *
sift_store_free_profile(Relation index, uint32 min_run)
{
	FreeProfileScan c = {.index = index,.min_run = min_run};
	SiftFreeProfile *p = palloc0(sizeof(SiftFreeProfile));
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);

	sift_freelist_walk(index, metabuf, collect_profile, &c);
	UnlockReleaseBuffer(metabuf);
	if (c.n > 1)
		qsort(c.ext, c.n, sizeof(SiftFreeExtent), cmp_run_start);
	p->n = c.n;
	p->min_run = min_run;
	p->start = palloc(Max(c.n, 1) * sizeof(BlockNumber));
	p->end = palloc(Max(c.n, 1) * sizeof(BlockNumber));
	p->before = palloc((c.n + 1) * sizeof(int64));
	p->before[0] = 0;
	for (int i = 0; i < c.n; i++)
	{
		p->start[i] = c.ext[i].start;
		p->end[i] = c.ext[i].start + c.ext[i].nblocks;
		p->before[i + 1] = p->before[i] + c.ext[i].nblocks;
	}
	if (c.ext)
		pfree(c.ext);
	return p;
}

/* The reusable free blocks in runs of at least min_run that lie before limit (a run across it counts the part before it, when that is a run of min_run itself) */
int64
sift_free_profile_below(const SiftFreeProfile *p, BlockNumber limit)
{
	int			lo = 0,
				hi = p->n;
	int64		blocks;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (p->end[mid] <= limit)
			lo = mid + 1;
		else
			hi = mid;
	}
	blocks = p->before[lo];
	if (lo < p->n && p->start[lo] < limit && limit - p->start[lo] >= p->min_run)
		blocks += limit - p->start[lo];
	return blocks;
}

void
sift_free_profile_free(SiftFreeProfile *p)
{
	pfree(p->start);
	pfree(p->end);
	pfree(p->before);
	pfree(p);
}
