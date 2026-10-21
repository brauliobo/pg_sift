/*
 * core/freelist.c - the free list: walks and counts, space mode, block allocation, release planning, recycling and the
 * truncation of the file tail (P2-P4 and P7 in core/store.h).
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/nbtxlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "catalog/pg_control.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bulk_write.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "col/col.h"
#include "bm25/estimate.h"
#include "core/lane.h"
#include "core/store.h"
#include "core/store_int.h"

/* ---------- free list ---------- */

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
 * Space mode (sift_store_space_exclusive): the caller holds AccessExclusiveLock on the relation, so no scan, directory copy or write target
 * of another backend exists and every released extent is reusable at once, whatever its safexid (P7 reasons the same way for the cut).
 */
uint64 space_written = 0;
bool space_exclusive = false;
BlockNumber space_limit = InvalidBlockNumber;

void
sift_store_space_exclusive(Relation index, bool on)
{
	Assert(!on || CheckRelationLockedByMe(index, AccessExclusiveLock, false));
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

/*
 * Generic WAL has no recovery conflicts, so a standby's snapshot is only protected by
 * hot_standby_feedback. With sift.standby_conflicts, before blocks released with safexid are
 * reused (or their record is cleared for reuse) this logs a btree page-reuse record for the index:
 * a standby replays it as the usual recovery conflict with the snapshots older than safexid (wait for
 * them up to max_standby_streaming_delay, then cancel them), touching no page. Once per backend and
 * horizon: an older one is covered by a newer.
 */
static void
log_reuse(Relation index, BlockNumber blk, FullTransactionId safexid)
{
	static RelFileNumber last_rel = InvalidRelFileNumber;
	static FullTransactionId last = {0};
	xl_btree_reuse_page xlrec;

	if (!sift_standby_conflicts || !XLogStandbyInfoActive() || !RelationNeedsWAL(index) ||
		(last_rel == index->rd_locator.relNumber && !FullTransactionIdPrecedes(last, safexid)))
		return;
	xlrec = (xl_btree_reuse_page) {.locator = index->rd_locator,.block = blk,.snapshotConflictHorizon = safexid};
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfBtreeReusePage);
	XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE);
	last_rel = index->rd_locator.relNumber;
	last = safexid;
}

/* horizon: computed by the first record that needs it, InvalidFullTransactionId until then */
static bool
reusable(Relation index, FullTransactionId safexid, FullTransactionId *horizon)
{
	if (!FullTransactionIdIsValid(safexid) || space_exclusive)
		return true;
	if (!FullTransactionIdIsValid(*horizon))
		*horizon = sift_store_horizon(index);
	return FullTransactionIdPrecedes(safexid, *horizon);
}

static BlockNumber
take_free(Relation index, BlockNumber blk, int idx, uint32 n)
{
	Buffer		buf = read_locked(index, blk, BUFFER_LOCK_EXCLUSIVE);
	GenericXLogState *state = GenericXLogStart(index);
	Page		page = GenericXLogRegisterBuffer(state, buf, 0);
	SiftFreeListData *fl = FL(page);
	SiftFreeExtent *e = &fl->ext[idx];
	BlockNumber start = e->start;

	if (n == e->nblocks)
		*e = fl->ext[--fl->n];
	else
	{
		e->start += n;
		e->nblocks -= n;
	}
	SiftPageSetUsed(page, FL_USED(fl->n));
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	return start;
}

/* P3: the new last block's full image restores the physical size on replay */
static BlockNumber
extend(Relation index, Buffer metabuf, uint32 n)
{
	BlockNumber first = META(BufferGetPage(metabuf))->high_water;
	GenericXLogState *state;
	Buffer		buf;

	if ((uint64) first + n > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" cannot grow past %u blocks",
						RelationGetRelationName(index), MaxBlockNumber)));
	buf = ExtendBufferedRelTo(BMR_REL(index), MAIN_FORKNUM, NULL, 0, first + n, RBM_ZERO_AND_LOCK);
	state = GenericXLogStart(index);
	META(GenericXLogRegisterBuffer(state, metabuf, 0))->high_water = first + n;
	sift_page_init(GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE), SIFT_PT_FREE, 0);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	return first;
}

/* The reusable record to take from the free list so far */
typedef struct FreePick
{
	Relation	index;
	uint32		want;
	uint32		min;
	BlockNumber blk;			/* its free-list page */
	int			idx;			/* its place in the page, -1 if none */
	uint32		nblocks;
	BlockNumber start;			/* the record's first block (space mode picks by it) */
	FullTransactionId safexid;
	FullTransactionId horizon;
} FreePick;

static bool
pick_free(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreePick   *p = arg;

	for (uint32 i = 0; i < fl->n; i++)
	{
		const SiftFreeExtent *e = &fl->ext[i];
		bool		better;

		if (e->nblocks < p->min || !reusable(p->index, e->safexid, &p->horizon))
			continue;
		if (space_exclusive && BlockNumberIsValid(space_limit))
		{
			/* Exact relocation reservations: the lowest eligible run below the planned end. */
			better = space_fits(e, p->min, space_limit) && (p->idx < 0 || e->start < p->start);
		}
		else
			better = p->nblocks < p->want && e->nblocks > p->nblocks;
		if (better)
		{
			p->blk = blk;
			p->idx = i;
			p->nblocks = e->nblocks;
			p->start = e->start;
			p->safexid = e->safexid;
		}
	}
	return space_exclusive || p->nblocks < p->want;
}

/*
 * Takes up to want contiguous blocks (at least min): the first reusable
 * free-list record holding want, else the largest holding min, else extends
 * by want. metabuf is locked exclusively, which also guards the free list.
 */
BlockNumber
alloc_blocks(Relation index, Buffer metabuf, uint32 want, uint32 min, uint32 *got)
{
	FreePick	p = {index, want, min, InvalidBlockNumber, -1, 0, 0, InvalidFullTransactionId, InvalidFullTransactionId};

	sift_freelist_walk(index, metabuf, pick_free, &p);
	if (space_exclusive && BlockNumberIsValid(space_limit) && p.idx < 0)
		elog(ERROR, "sift: relocation reservation no longer fits below its planned end");
	*got = p.idx >= 0 ? Min(want, p.nblocks) : want;
	if (p.idx >= 0 && FullTransactionIdIsValid(p.safexid) && !space_exclusive)
		log_reuse(index, p.blk, p.safexid);
	return p.idx >= 0 ? take_free(index, p.blk, p.idx, *got) : extend(index, metabuf, want);
}

/* Under the metapage lock, so the orphan sweep never sees them uninitialized */
static void
init_new_pages(Relation index, BlockNumber first, uint32 n, SiftPageType type, uint64 gen,
			   Buffer *bufs)
{
	PGIOAlignedBlock tmpl;
	Page		srcs[SIFT_ALLOC_MAX_LOCKED];

	sift_page_init(tmpl.data, type, gen);
	for (uint32 i = 0; i < n; i++)
	{
		bufs[i] = new_locked(index, first + i);
		srcs[i] = tmpl.data;
	}
	log_pages(index, bufs, srcs, n);
}

BlockNumber
sift_alloc_extent(Relation index, uint32 nblocks, SiftPageType type, uint64 gen, Buffer *bufs)
{
	Buffer		metabuf;
	BlockNumber first;
	uint32		got;

	if (nblocks == 0 || (bufs && nblocks > SIFT_ALLOC_MAX_LOCKED))
		elog(ERROR, "sift: cannot allocate %u locked blocks", nblocks);
	metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	first = alloc_blocks(index, metabuf, nblocks, nblocks, &got);
	if (bufs)
		init_new_pages(index, first, nblocks, type, gen, bufs);
	UnlockReleaseBuffer(metabuf);
	return first;
}

Buffer
sift_alloc_page(Relation index, SiftPageType type, uint64 gen)
{
	Buffer		buf;

	sift_alloc_extent(index, 1, type, gen, &buf);
	return buf;
}

int
cmp_extent(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftExtent *) a)->start, ((const SiftExtent *) b)->start);
}

static int
cmp_free(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftFreeExtent *) a)->start, ((const SiftFreeExtent *) b)->start);
}

/* Logs k free-list pages in blks holding ext, the last one linking to tail */
static void
write_chain(Relation index, const BlockNumber *blks, int k, const SiftFreeExtent *ext, int n,
			BlockNumber tail)
{
	PGIOAlignedBlock tmp;
	Page		page = tmp.data;

	for (int i = 0; i < k; i++)
	{
		SiftFreeListData *fl = FL(page);
		int			first = Min(n, i * (int) SIFT_FREELIST_CAP);
		Buffer		buf = new_locked(index, blks[i]);

		sift_page_init(page, SIFT_PT_FREELIST, 0);
		fl->next = i + 1 < k ? blks[i + 1] : tail;
		fl->n = Min(n - first, (int) SIFT_FREELIST_CAP);
		memcpy(fl->ext, ext + first, fl->n * sizeof(SiftFreeExtent));
		SiftPageSetUsed(page, FL_USED(fl->n));
		log_pages(index, &buf, &page, 1);
		UnlockReleaseBuffer(buf);
	}
}

/* New free-list pages holding ext, chained in front of the current head */
static BlockNumber
write_free_pages(Relation index, Buffer metabuf, const SiftFreeExtent *ext, int n)
{
	int			k = (n + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
	BlockNumber *blks = palloc(k * sizeof(BlockNumber));
	uint32		got;

	for (int i = 0; i < k; i++)
		blks[i] = alloc_blocks(index, metabuf, 1, 1, &got);
	write_chain(index, blks, k, ext, n, META(BufferGetPage(metabuf))->freelist_head);
	return blks[0];
}

/* What prepare_free learns of the free list: every record and page (to refuse a double free) and the first page with room */
typedef struct FreeScan
{
	SiftFreeExtent *have;		/* the records, and the pages of the list as one-block records */
	int			nhave;
	int			maxhave;
	BlockNumber room;			/* the first page with room for need records */
	int			need;
} FreeScan;

static bool
scan_free(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreeScan   *s = arg;

	if (!BlockNumberIsValid(s->room) && SIFT_FREELIST_CAP - fl->n >= (uint32) s->need)
		s->room = blk;
	if (s->nhave + fl->n + 1 > s->maxhave)
	{
		s->maxhave = Max(64, 2 * (s->nhave + fl->n + 1));
		s->have = s->have ? repalloc(s->have, s->maxhave * sizeof(SiftFreeExtent)) :
			palloc(s->maxhave * sizeof(SiftFreeExtent));
	}
	memcpy(s->have + s->nhave, fl->ext, fl->n * sizeof(SiftFreeExtent));
	s->nhave += fl->n;
	s->have[s->nhave++] = (SiftFreeExtent) {blk, 1};
	return true;
}

/*
 * Sorts and coalesces the release list, rejects blocks outside the index or
 * already free (a double free would hand one block to two owners), and finds
 * room: an existing free-list page (returned locked) or new logged pages.
 */
void
prepare_free(Relation index, Buffer metabuf, const SiftExtent *release, int nrelease,
			 FullTransactionId safexid, FreePlan *plan)
{
	SiftMetaPageData *meta = META(BufferGetPage(metabuf));
	SiftExtent *rel = palloc(nrelease * sizeof(SiftExtent));
	FreeScan	scan = {.room = InvalidBlockNumber};
	int			n = 0;

	memcpy(rel, release, nrelease * sizeof(SiftExtent));
	qsort(rel, nrelease, sizeof(SiftExtent), cmp_extent);
	plan->ext = palloc(nrelease * sizeof(SiftFreeExtent));
	for (int i = 0; i < nrelease; i++)
	{
		SiftFreeExtent *last = n > 0 ? &plan->ext[n - 1] : NULL;

		if (rel[i].nblocks == 0 || rel[i].start < FIRST_DATA_BLKNO ||
			(uint64) rel[i].start + rel[i].nblocks > meta->high_water)
			corrupt(index, "Released extent %u+%u lies outside blocks %u..%u.",
					rel[i].start, rel[i].nblocks, FIRST_DATA_BLKNO, meta->high_water - 1);
		if (last && last->start + last->nblocks > rel[i].start)
			corrupt(index, "Block %u is released twice.", rel[i].start);
		if (last && last->start + last->nblocks == rel[i].start)
			last->nblocks += rel[i].nblocks;
		else
			plan->ext[n++] = (SiftFreeExtent) {rel[i].start, rel[i].nblocks, safexid};
	}
	plan->n = n;

	scan.need = n;
	sift_freelist_walk(index, metabuf, scan_free, &scan);
	if (scan.nhave > 0)
		qsort(scan.have, scan.nhave, sizeof(SiftFreeExtent), cmp_free);
	for (int i = 0, j = 0; i < n && j < scan.nhave;)
	{
		if (plan->ext[i].start + plan->ext[i].nblocks <= scan.have[j].start)
			i++;
		else if (scan.have[j].start + scan.have[j].nblocks <= plan->ext[i].start)
			j++;
		else
			corrupt(index, "Block %u is released twice.", Max(plan->ext[i].start, scan.have[j].start));
	}
	if (BlockNumberIsValid(scan.room))
		plan->flbuf = read_locked(index, scan.room, BUFFER_LOCK_EXCLUSIVE);
	else
		plan->newhead = write_free_pages(index, metabuf, plan->ext, n);
}

void
apply_free(SiftMetaPageData *meta, Page flpage, const FreePlan *plan)
{
	if (flpage)
	{
		SiftFreeListData *fl = FL(flpage);

		memcpy(fl->ext + fl->n, plan->ext, plan->n * sizeof(SiftFreeExtent));
		fl->n += plan->n;
		SiftPageSetUsed(flpage, FL_USED(fl->n));
	}
	else if (BlockNumberIsValid(plan->newhead))
		meta->freelist_head = plan->newhead;
}


/* Sorts by start and merges adjacent reusable records; returns the count */
static int
coalesce(SiftFreeExtent *ext, int n)
{
	int			m = 0;

	qsort(ext, n, sizeof(SiftFreeExtent), cmp_free);
	for (int i = 0; i < n; i++)
	{
		SiftFreeExtent *last = m > 0 ? &ext[m - 1] : NULL;

		if (last && !FullTransactionIdIsValid(last->safexid) &&
			!FullTransactionIdIsValid(ext[i].safexid) && last->start + last->nblocks == ext[i].start)
			last->nblocks += ext[i].nblocks;
		else
			ext[m++] = ext[i];
	}
	return m;
}

static bool
collect_chain(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	FreeChain  *c = arg;

	if (c->n + fl->n + c->npages + 1 > c->maxext)
	{
		c->maxext = Max(64, 2 * (c->n + (int) fl->n + c->npages + 1));
		c->ext = c->ext ? repalloc(c->ext, c->maxext * sizeof(SiftFreeExtent)) : palloc(c->maxext * sizeof(SiftFreeExtent));
	}
	if (c->npages + 1 > c->maxpage)
	{
		c->maxpage = Max(8, 2 * c->maxpage);
		c->page = c->page ? repalloc(c->page, c->maxpage * sizeof(BlockNumber)) : palloc(c->maxpage * sizeof(BlockNumber));
	}
	for (uint32 i = 0; i < fl->n; i++)
	{
		SiftFreeExtent *e = &c->ext[c->n++];

		*e = fl->ext[i];
		if (FullTransactionIdIsValid(e->safexid) && reusable(c->index, e->safexid, &c->horizon))
		{
			if (!FullTransactionIdIsValid(c->newest) || FullTransactionIdPrecedes(c->newest, e->safexid))
				c->newest = e->safexid;
			e->safexid = InvalidFullTransactionId;
			c->cleared++;
		}
	}
	c->page[c->npages++] = blk;
	return true;
}

/* metabuf is locked, which also guards the list. ext has room for the list's pages after the records */
void
read_chain(Relation index, Buffer metabuf, FreeChain *c, FullTransactionId horizon)
{
	*c = (FreeChain) {.index = index,.horizon = horizon};
	sift_freelist_walk(index, metabuf, collect_chain, c);
}

static void
coalesce_chain(FreeChain *c)
{
	c->n = coalesce(c->ext, c->n);
}

/* The pages of the list itself as free records, for when the list that holds them is replaced */
static void
free_own_pages(FreeChain *c)
{
	for (int i = 0; i < c->npages; i++)
		c->ext[c->n++] = (SiftFreeExtent) {c->page[i], 1};
	coalesce_chain(c);
}

/* Where the reusable blocks at the end of the relation start: the last of the sorted, coalesced records if it ends at hwm, else hwm */
static BlockNumber
free_tail(const FreeChain *c, BlockNumber hwm)
{
	const SiftFreeExtent *last = c->n > 0 ? &c->ext[c->n - 1] : NULL;

	return last && !FullTransactionIdIsValid(last->safexid) && last->start + last->nblocks == hwm ?
		last->start : hwm;
}

/*
 * Writes the free list anew as one sorted, coalesced chain in blocks that are free now, then switches
 * the head in one record; the old chain pages become free records. A crash before the switch leaves
 * the old chain intact. trim: the free run at the end of the relation also leaves the list and the
 * high-water mark moves to its start (the caller then truncates the file, after this record: a
 * relation longer than the mark is harmless, one shorter is not). With trim, returns the new mark, or
 * InvalidBlockNumber when there is nothing to give back (nothing is written then).
 */
static BlockNumber
rewrite_chain(Relation index, Buffer metabuf, FreeChain *c, bool trim)
{
	BlockNumber hwm = META(BufferGetPage(metabuf))->high_water;
	BlockNumber newhwm = hwm;
	SiftMetaPageData *meta;
	BlockNumber *blks;
	GenericXLogState *state;
	int			k;

	coalesce_chain(c);
	k = (c->n + c->npages + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
	blks = palloc(k * sizeof(BlockNumber));
	/* New pages come from free records, never from the live old chain; a trim never extends the relation */
	for (int i = 0, j = 0; i < k; i++)
	{
		while (j < c->n && FullTransactionIdIsValid(c->ext[j].safexid))
			j++;
		if (j == c->n && trim)
			return InvalidBlockNumber;
		if (j == c->n)
			blks[i] = extend(index, metabuf, 1);
		else
		{
			blks[i] = c->ext[j].start++;
			c->ext[j].nblocks--;
			if (c->ext[j].nblocks == 0)
				memmove(&c->ext[j], &c->ext[j + 1], (--c->n - j) * sizeof(SiftFreeExtent));
		}
	}
	free_own_pages(c);
	if (trim && (newhwm = free_tail(c, hwm)) == hwm)
		return InvalidBlockNumber;
	if (trim)
		c->n--;
	if (c->cleared > 0 && !space_exclusive)
		log_reuse(index, InvalidBlockNumber, c->newest);
	write_chain(index, blks, k, c->ext, c->n, InvalidBlockNumber);
	state = GenericXLogStart(index);
	meta = META(GenericXLogRegisterBuffer(state, metabuf, 0));
	meta->freelist_head = blks[0];
	if (trim)
		meta->high_water = newhwm;
	GenericXLogFinish(state);
	return newhwm;
}

/*
 * What the cut of the file counts as reusable (InvalidFullTransactionId: what the horizon says). A lazy VACUUM releases with the next xid
 * (xid_safexid) and cuts under AccessExclusiveLock, which keeps every reader out, so for it every record is reusable.
 */
static FullTransactionId
cut_horizon(void)
{
	return (MyProc->statusFlags & PROC_IN_VACUUM) ? FullTransactionIdFromU64(PG_UINT64_MAX) : InvalidFullTransactionId;
}

/* Clears the safexid of free-list records past the horizon and coalesces adjacent reusable records */
int
sift_recycle(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	FreeChain	c;
	int			raw;

	read_chain(index, metabuf, &c, InvalidFullTransactionId);
	raw = c.n;
	coalesce_chain(&c);
	if (c.npages == 0 || (c.cleared == 0 && c.n == raw && c.npages == 1))
		c.cleared = 0;
	else
		rewrite_chain(index, metabuf, &c, false);
	UnlockReleaseBuffer(metabuf);
	return c.cleared;
}

BlockNumber
sift_store_tail_blocks(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	BlockNumber hwm = META(BufferGetPage(metabuf))->high_water;
	FreeChain	c;
	BlockNumber tail = hwm;

	read_chain(index, metabuf, &c, cut_horizon());
	if (c.npages > 0)
	{
		coalesce_chain(&c);
		free_own_pages(&c);
		tail = free_tail(&c, hwm);
	}
	UnlockReleaseBuffer(metabuf);
	return RelationGetNumberOfBlocks(index) - tail;
}

/*
 * No backend uses the index (the caller holds AccessExclusiveLock), so none holds a directory copy, a
 * scan or a cached block number (see below) that the truncation could outdate; a standby replays
 * the lock first and cancels what holds one there.
 */
BlockNumber
sift_store_truncate(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	BlockNumber hwm = META(BufferGetPage(metabuf))->high_water;
	BlockNumber newhwm = InvalidBlockNumber;
	BlockNumber nblocks;
	FreeChain	c;

	read_chain(index, metabuf, &c, cut_horizon());
	if (c.npages > 0)
		newhwm = rewrite_chain(index, metabuf, &c, true);
	UnlockReleaseBuffer(metabuf);
	/* a crash between the record and the truncation left a file longer than the mark: it ends there too */
	if (!BlockNumberIsValid(newhwm))
		newhwm = hwm;
	if ((nblocks = RelationGetNumberOfBlocks(index)) <= newhwm)
		return 0;
	if (space_exclusive)
		SIFT_SPACE_TEST_POINT(3);
	RelationTruncate(index, newhwm);
	/*
	 * A backend appending to the write buffer keeps the blocks of its lane tails in the relcache
	 * entry (sift_wbuf_append), validated by reading them, and a block past the new end cannot be
	 * read. Their next lock on the index happens after ours is released, and takes this message first.
	 */
	invalidate_mapping(index);
	return nblocks - newhwm;
}
