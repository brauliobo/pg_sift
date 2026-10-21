/*
 * sift_store.c - metapage, directory, free list, extent allocation, write
 * targets, byte streams and WAL helpers. The crash-safety protocol (P1-P6)
 * is documented in sift_store.h.
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
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "sift_col.h"
#include "bm25/estimate.h"
#include "sift_lane.h"
#include "sift_store.h"

#define SPECIAL_SIZE		MAXALIGN(sizeof(SiftPageOpaqueData))
#define FIRST_DATA_BLKNO	(SIFT_DIRECTORY_BLKNO + 1)
#define RUN_MIN				16
#define RUN_MAX				1024
#define RUN_BIG				8192
#define EXTENT_TARGET		256

#define META(page)	((SiftMetaPageData *) SiftPageGetData(page))
#define DIR(page)	((SiftDirectoryData *) SiftPageGetData(page))
#define FL(page)	((SiftFreeListData *) SiftPageGetData(page))
#define DIR_USED(n)	(offsetof(SiftDirectoryData, segs) + (n) * sizeof(SiftSegEntry))
#define FL_USED(n)	(offsetof(SiftFreeListData, ext) + (n) * sizeof(SiftFreeExtent))
#define ULL(x)		((unsigned long long) (x))

bool		sift_standby_conflicts = false;

#ifdef SIFT_TESTING
int			sift_test_space_crash = 0;
bool		sift_test_space_error = false;
int			sift_test_fail_pages = 0;
int			sift_test_max_extents = 0;
bool		sift_test_run_gap = false;
#define TEST_KNOB(knob, normal)	((knob) > 0 ? (knob) : (normal))
#else
#define TEST_KNOB(knob, normal)	(normal)
#endif

#define corrupt SIFT_CORRUPT

struct SiftWriteTarget
{
	Relation	index;
	SiftWriteMode mode;
	BulkWriteState *bulk;		/* BULK, INIT */
	BlockNumber next;			/* BULK, INIT: first unused block */
	bool		have_meta;
	SiftMetaPageData meta;		/* BULK, INIT: written at finish */
	SiftExtent *spare;			/* allocated, never written */
	int			nspare;
	int			maxspare;
	SiftExtent *taken;			/* SHARED: every block taken from the store and not given back yet */
	int			ntaken;
	int			maxtaken;
	uint64		expect;			/* SHARED: blocks the streams are expected to need, 0 = unknown */
	int			delay_ms;		/* sift_wt_throttle: sleep per THROTTLE_PAGES charged, 0 = none */
	uint64		charged;		/* pages charged since the last sleep */
#ifdef SIFT_TESTING
	int			npages;			/* SHARED: pages written, for sift_test_fail_pages */
#endif
};

struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		pos;
	Page		page;			/* page being filled, NULL between pages */
	PGIOAlignedBlock *local;	/* SHARED: page buffer */
	Size		used;			/* bytes on page */
	BlockNumber blk;
	BlockNumber run_next;		/* unused part of the current run */
	BlockNumber run_end;
	uint32		grow;
	uint64		blocks;			/* blocks taken for this stream */
	SiftExtentList ext;
};

#ifdef SIFT_TESTING
void
sift_store_space_test_point(int phase)
{
	if (sift_test_space_crash == phase)
	{
		ereport(LOG, (errmsg("sift test: space crash phase %d", phase)));
		XLogFlush(GetXLogInsertRecPtr());
		kill(MyProcPid, SIGKILL);
	}
}
#endif

/* Records to add to the free list, and where they go */
typedef struct FreePlan
{
	SiftFreeExtent *ext;
	int			n;
	Buffer		flbuf;			/* existing page with room, locked */
	BlockNumber newhead;		/* or new pages, already logged */
} FreePlan;

typedef struct BlockMap
{
	Relation	index;
	BlockNumber hwm;
	Bitmapset  *set;
} BlockMap;

/* ---------- pages ---------- */

static bool
layout_ok(Page page)
{
	PageHeader	ph = (PageHeader) page;

	return ph->pd_special == BLCKSZ - SPECIAL_SIZE && ph->pd_upper == ph->pd_special &&
		ph->pd_lower >= SIFT_PAGE_DATA_OFFSET && ph->pd_lower <= ph->pd_upper;
}

static Buffer
read_locked(Relation index, BlockNumber blkno, int mode)
{
	Buffer		buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);

	LockBuffer(buf, mode);
	return buf;
}

/* A block we own: old content is irrelevant, skip the read */
static Buffer
new_locked(Relation index, BlockNumber blkno)
{
	return ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK, NULL);
}

/*
 * Copies srcs into the exclusively locked bufs and logs them as full images
 * in one record, inside one critical section (P4: content enters the buffer
 * only together with its WAL).
 */
static void
log_pages(Relation index, Buffer *bufs, Page *srcs, int n)
{
	bool		wal = RelationNeedsWAL(index);

	Assert(n > 0 && n <= XLR_MAX_BLOCK_ID);
	if (wal)
	{
		XLogEnsureRecordSpace(n - 1, 0);
		XLogBeginInsert();
	}
	START_CRIT_SECTION();
	for (int i = 0; i < n; i++)
	{
		memcpy(BufferGetPage(bufs[i]), srcs[i], BLCKSZ);
		MarkBufferDirty(bufs[i]);
		if (wal)
			XLogRegisterBuffer(i, bufs[i], REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	}
	if (wal)
	{
		XLogRecPtr	lsn = XLogInsert(RM_XLOG_ID, XLOG_FPI);

		for (int i = 0; i < n; i++)
			PageSetLSN(BufferGetPage(bufs[i]), lsn);
	}
	END_CRIT_SECTION();
}

static bool
dir_has_gen(Relation index, uint64 gen)
{
	SiftDirSnapshot snap;

	sift_dir_read(index, &snap);
	for (int i = 0; i < snap.nsegs; i++)
		if (snap.segs[i].gen == gen)
			return true;
	return false;
}

void
sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen)
{
	Page		page = BufferGetPage(buf);
	PageHeader	ph = (PageHeader) page;
	BlockNumber blkno = BufferGetBlockNumber(buf);
	SiftPageOpaqueData found;

	if (!layout_ok(page))
		corrupt(index, "Block %u has an invalid page layout (lower %u, upper %u, special %u).",
				blkno, ph->pd_lower, ph->pd_upper, ph->pd_special);
	found = *SiftPageGetOpaque(page);
	if (likely(found.type == type && found.gen == gen))
		return;
	if (RecoveryInProgress() && gen != 0)
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		if (!dir_has_gen(index, gen))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("sift segment recycled during scan, retry"),
					 errdetail("Index \"%s\", segment generation %llu, block %u.",
							   RelationGetRelationName(index), ULL(gen), blkno),
					 errhint("Set hot_standby_feedback = on on the standby to avoid this.")));
	}
	corrupt(index, "Block %u: expected page type %d generation %llu, found type %d generation %llu.",
			blkno, type, ULL(gen), found.type, ULL(found.gen));
}

/* ---------- metapage and directory ---------- */

/*
 * The metapage minor a build must read to understand the segment: the segment format of a BM25 segment, 1.3 for a column segment with unfrozen
 * rows (which an older build would show as final); the other column segments need no more than the base, their format is the column engine's own
 */
static uint16
seg_minor(Relation index, const SiftSegEntry *s)
{
	if (index->rd_rel->relam == sift_catalog()->am)
		return Min(s->format, SIFT_FORMAT_MINOR);
	return (s->flags & SIFT_SEG_VERS) ? SIFT_FORMAT_MINOR : SIFT_FORMAT_MINOR_BASE;
}

static void
check_meta(Relation index, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	SiftMetaPageData *meta = META(page);

	sift_store_check_page(index, buf, SIFT_PT_META, 0);
	if (meta->magic != SIFT_MAGIC || SiftPageGetUsed(page) < sizeof(SiftMetaPageData))
		corrupt(index, "Bad metapage (magic 0x%08x, %u bytes).", meta->magic, SiftPageGetUsed(page));
	if (meta->format_major != SIFT_FORMAT_MAJOR || meta->format_minor > SIFT_FORMAT_MINOR)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" has on-disk format %u.%u, this build reads formats %u.0 to %u.%u",
						RelationGetRelationName(index), meta->format_major, meta->format_minor,
						SIFT_FORMAT_MAJOR, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR),
				 errhint("REINDEX the index.")));
	if (meta->high_water < FIRST_DATA_BLKNO)
		corrupt(index, "High-water mark %u is below the first data block.", meta->high_water);
}

static void
check_dir(Relation index, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	SiftDirectoryData *dir = DIR(page);

	sift_store_check_page(index, buf, SIFT_PT_DIRECTORY, 0);
	if (dir->nsegs > SIFT_MAX_SEGMENTS || SiftPageGetUsed(page) != DIR_USED(dir->nsegs))
		corrupt(index, "Directory holds %u segments in %u bytes.", dir->nsegs, SiftPageGetUsed(page));
}

static Buffer
lock_meta(Relation index, int mode)
{
	Buffer		buf = read_locked(index, SIFT_METAPAGE_BLKNO, mode);

	check_meta(index, buf);
	return buf;
}

static Buffer
lock_dir(Relation index, int mode)
{
	Buffer		buf = read_locked(index, SIFT_DIRECTORY_BLKNO, mode);

	check_dir(index, buf);
	return buf;
}

void
sift_dir_read(Relation index, SiftDirSnapshot *snap)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	SiftDirectoryData *dir = DIR(BufferGetPage(dirbuf));

	snap->meta = *META(BufferGetPage(metabuf));
	snap->version = dir->version;
	snap->nsegs = dir->nsegs;
	memcpy(snap->segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
	UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
}

Buffer
sift_dir_pin(Relation index)
{
	return ReadBufferExtended(index, MAIN_FORKNUM, SIFT_DIRECTORY_BLKNO, RBM_NORMAL, NULL);
}

uint32
sift_dir_version_peek(Buffer dirbuf)
{
	pg_read_barrier();
	return *(volatile uint32 *) &DIR(BufferGetPage(dirbuf))->version;
}

uint32
sift_dir_version(Relation index)
{
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	uint32		version = DIR(BufferGetPage(dirbuf))->version;

	UnlockReleaseBuffer(dirbuf);
	return version;
}

uint64
sift_store_new_gen(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	GenericXLogState *state = GenericXLogStart(index);
	uint64		gen = META(GenericXLogRegisterBuffer(state, metabuf, 0))->next_gen++;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(metabuf);
	return gen;
}

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
static uint64 space_written = 0;
static bool space_exclusive = false;
static BlockNumber space_limit = InvalidBlockNumber;

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
space_fits(const SiftFreeExtent *e, uint32 min, BlockNumber limit)
{
	return e->nblocks >= min && e->start < limit && min <= limit - e->start;
}

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
static BlockNumber
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

static int
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
static void
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

static void
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

/* Validates the mutator's result inside the record; aborts it on failure */
static void
finish_dir(GenericXLogState *state, const SiftMetaPageData *before, const SiftMetaPageData *meta,
		   Page dirpage)
{
	SiftDirectoryData *dir = DIR(dirpage);
	const char *err = NULL;

	if (meta->magic != before->magic || meta->high_water != before->high_water ||
		meta->freelist_head != before->freelist_head || meta->next_gen < before->next_gen)
		err = "allocation state changed";
	else if (dir->nsegs > SIFT_MAX_SEGMENTS)
	{
		GenericXLogAbort(state);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index cannot hold more than %d segments", SIFT_MAX_SEGMENTS)));
	}
	for (int i = 0; !err && i < dir->nsegs; i++)
	{
		const SiftSegEntry *s = &dir->segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_WBUF_SEALED)
			err = "invalid segment kind";
		else if (s->gen == 0 || s->gen >= meta->next_gen)
			err = "segment generation was not reserved";
		else if (s->format > Max(SIFT_FORMAT_MINOR, SIFT_COL_FORMAT_MAX_READ))	/* the column engine numbers its segment formats on its own */
			err = "unknown segment format";
		for (int j = 0; !err && j < i; j++)
			if (dir->segs[j].gen == s->gen)
				err = "duplicate segment generation";
	}
	if (err)
	{
		GenericXLogAbort(state);
		elog(ERROR, "sift: invalid directory update: %s", err);
	}
	dir->version++;
	SiftPageSetUsed(dirpage, DIR_USED(dir->nsegs));
}

/*
 * One generic WAL record over metapage, [directory], [free-list page],
 * [extra] (P2). publish: a directory change whose released extents wait for
 * the horizon; otherwise only never-referenced blocks are released, reusable
 * at once.
 */
/*
 * P4 safexid: every snapshot that could hold the directory as it was before
 * our lock has xmin <= our xid (assigned before the lock), or <= the next xid
 * read after the lock. Parallel mode cannot assign an xid; the next xid is
 * as safe but is only passed once some later xid completes. A lazy VACUUM
 * (PROC_IN_VACUUM) is left out of snapshots and of the horizon, xid included
 * (procarray.c): a reader that began after its xid was assigned has an xmin
 * past it and would let the extents go while it still reads them, so a
 * VACUUM uses the next xid as well.
 */
static bool
xid_safexid(void)
{
	return !(MyProc->statusFlags & PROC_IN_VACUUM) &&
		(FullTransactionIdIsValid(GetTopFullTransactionIdIfAny()) || !(IsInParallelMode() || IsParallelWorker()));
}

/* Physical directory changes survive transaction abort; publish their cache invalidation before a relation lock can be released. */
static void
invalidate_mapping(Relation index)
{
	SharedInvalidationMessage msg = {.rc = {SHAREDINVALRELCACHE_ID, MyDatabaseId, RelationGetRelid(index)}};

	SendSharedInvalidMessages(&msg, 1);
	LocalExecuteInvalidationMessage(&msg);
}

static void
store_update(Relation index, const SiftDirChange *chg, bool publish, SiftWriteTarget *published)
{
	bool		pending = publish && chg->nrelease > 0 && !chg->immediate && !space_exclusive;
	FullTransactionId safexid = pending && xid_safexid() ?
		GetTopFullTransactionId() : InvalidFullTransactionId;
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	Buffer		dirbuf = publish ? lock_dir(index, BUFFER_LOCK_EXCLUSIVE) : InvalidBuffer;
	FreePlan	plan = {.flbuf = InvalidBuffer, .newhead = InvalidBlockNumber};
	GenericXLogState *state;
	SiftMetaPageData before;
	Page		mp;
	Page		dp = NULL;
	Page		fp = NULL;
	Page		xp = NULL;

	if (pending && !FullTransactionIdIsValid(safexid))
		safexid = ReadNextFullTransactionId();
	if (chg->nrelease > 0)
		prepare_free(index, metabuf, chg->release, chg->nrelease, safexid, &plan);
	state = GenericXLogStart(index);
	mp = GenericXLogRegisterBuffer(state, metabuf, 0);
	if (publish)
		dp = GenericXLogRegisterBuffer(state, dirbuf, 0);
	if (BufferIsValid(plan.flbuf))
		fp = GenericXLogRegisterBuffer(state, plan.flbuf, 0);
	if (BufferIsValid(chg->extra))
		xp = GenericXLogRegisterBuffer(state, chg->extra, 0);
	before = *META(mp);
	if (publish)
	{
		if (chg->fn)
			chg->fn(META(mp), DIR(dp), xp, chg->arg);
		/* a build that reads less than the newest segment format of the index must refuse it; a column segment's format is its own (sift_col.h), its unfrozen rows raise the minor */
		for (int i = 0; i < DIR(dp)->nsegs; i++)
			META(mp)->format_minor = Max(META(mp)->format_minor, seg_minor(index, &DIR(dp)->segs[i]));
		finish_dir(state, &before, META(mp), dp);
	}
	apply_free(META(mp), fp, &plan);
	/* Do not accept cancellation between a durable physical mapping change and its immediate invalidation. */
	if (publish && space_exclusive && chg->nrelease > 0)
		HOLD_INTERRUPTS();
	GenericXLogFinish(state);
	/* These allocations now belong to the directory, even if the caller subsequently errors. */
	if (published)
		published->ntaken = published->nspare = 0;
	if (BufferIsValid(plan.flbuf))
		UnlockReleaseBuffer(plan.flbuf);
	if (publish)
		UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
	if (publish && space_exclusive && chg->nrelease > 0)
	{
		invalidate_mapping(index);
		RESUME_INTERRUPTS();
	}
}

void
sift_dir_update(Relation index, const SiftDirChange *chg)
{
	store_update(index, chg, true, NULL);
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

/* The free list read into memory */
typedef struct FreeChain
{
	SiftFreeExtent *ext;		/* every record; those past the horizon are made reusable (safexid cleared) */
	int			n;
	int			maxext;
	BlockNumber *page;			/* the pages of the list itself */
	int			npages;
	int			maxpage;
	int			cleared;		/* records this read made reusable */
	FullTransactionId newest;	/* the newest safexid among them */
	Relation	index;
	FullTransactionId horizon;
} FreeChain;

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
static void
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


/* ---------- write targets ---------- */

SiftWriteTarget *
sift_wt_begin(Relation index, SiftWriteMode mode)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));
	ForkNumber	fork = mode == SIFT_WRITE_INIT ? INIT_FORKNUM : MAIN_FORKNUM;

	wt->index = index;
	wt->mode = mode;
	if (mode != SIFT_WRITE_SHARED)
	{
		if (smgrnblocks(RelationGetSmgr(index), fork) != 0)
			elog(ERROR, "sift: bulk write into non-empty index \"%s\"", RelationGetRelationName(index));
		wt->bulk = smgr_bulk_start_rel(index, fork);
		wt->next = FIRST_DATA_BLKNO;
	}
	return wt;
}

void
sift_store_init_meta_ex(SiftWriteTarget *wt, float4 k1, float4 b, uint32 analyzer_hash, uint32 flags)
{
	SiftMetaPageData *meta = &wt->meta;

	if (wt->bulk == NULL)
		elog(ERROR, "sift: the metapage is written by BULK or INIT write targets only");
	memset(meta, 0, sizeof(SiftMetaPageData));
	meta->magic = SIFT_MAGIC;
	meta->format_major = SIFT_FORMAT_MAJOR;
	meta->format_minor = SIFT_FORMAT_MINOR_BASE;
	meta->k1 = k1;
	meta->b = b;
	meta->analyzer_hash = analyzer_hash;
	meta->flags = flags;
	meta->next_gen = 1;
	meta->freelist_head = InvalidBlockNumber;
	wt->have_meta = true;
}

/* Room for one more extent in a list that lives as long as wt, whatever context the stream writer ran in */
static SiftExtent *
wt_list_room(SiftWriteTarget *wt, SiftExtent *list, int n, int *max)
{
	if (n < *max)
		return list;
	*max = Max(8, 2 * *max);
	return list ? repalloc(list, *max * sizeof(SiftExtent)) :
		MemoryContextAlloc(GetMemoryChunkContext(wt), *max * sizeof(SiftExtent));
}

/* a minus b (disjoint extents each) in a list of wt's lifetime */
static SiftExtent *
extents_subtract(SiftWriteTarget *wt, SiftExtent *a, int *na, SiftExtent *b, int nb)
{
	SiftExtent *out = MemoryContextAlloc(GetMemoryChunkContext(wt), Max(1, *na + nb) * sizeof(SiftExtent));
	int			m = 0;

	qsort(a, *na, sizeof(SiftExtent), cmp_extent);
	qsort(b, nb, sizeof(SiftExtent), cmp_extent);
	for (int i = 0, j = 0; i < *na; i++)
	{
		uint64		s = a[i].start,
					e = (uint64) a[i].start + a[i].nblocks;

		while (j < nb && (uint64) b[j].start + b[j].nblocks <= s)
			j++;
		for (int k = j; k < nb && b[k].start < e; k++)
		{
			if (b[k].start > s)
				out[m++] = (SiftExtent) {s, b[k].start - s};
			s = Max(s, (uint64) b[k].start + b[k].nblocks);
		}
		if (s < e)
			out[m++] = (SiftExtent) {s, e - s};
	}
	pfree(a);
	*na = m;
	return out;
}

void
sift_store_init_meta(SiftWriteTarget *wt, const SiftOptions *opts, uint32 analyzer_hash)
{
	sift_store_init_meta_ex(wt, (float4) opts->k1, (float4) opts->b, analyzer_hash, 0);
}

/* Takes want blocks (at least min): spare runs first, then fresh ones */
static BlockNumber
wt_take(SiftWriteTarget *wt, uint32 want, uint32 min, uint32 *got)
{
	Buffer		metabuf;
	BlockNumber start;
	SiftExtent *last;

	for (int i = 0; i < wt->nspare; i++)
	{
		SiftExtent *s = &wt->spare[i];

		if (s->nblocks >= min)
		{
			start = s->start;
			*got = Min(want, s->nblocks);
			s->start += *got;
			s->nblocks -= *got;
			if (s->nblocks == 0)
				*s = wt->spare[--wt->nspare];
			return start;
		}
	}
	if (wt->bulk)
	{
		/* Continue a trailing spare run so BULK layouts stay hole-free */
		for (int i = 0; i < wt->nspare; i++)
			if (wt->spare[i].start + wt->spare[i].nblocks == wt->next)
			{
				wt->next = wt->spare[i].start;
				wt->spare[i] = wt->spare[--wt->nspare];
				break;
			}
		if ((uint64) wt->next + want > MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" cannot grow past %u blocks",
							RelationGetRelationName(wt->index), MaxBlockNumber)));
		*got = want;
		wt->next += want;
		return wt->next - want;
	}
	/* the note cannot fail once the blocks are taken */
	wt->taken = wt_list_room(wt, wt->taken, wt->ntaken, &wt->maxtaken);
	metabuf = lock_meta(wt->index, BUFFER_LOCK_EXCLUSIVE);
	start = alloc_blocks(wt->index, metabuf, want, min, got);
	UnlockReleaseBuffer(metabuf);
	last = wt->ntaken > 0 ? &wt->taken[wt->ntaken - 1] : NULL;
	if (last && last->start + last->nblocks == start)
		last->nblocks += *got;
	else
		wt->taken[wt->ntaken++] = (SiftExtent) {start, *got};
#ifdef SIFT_TESTING
	/* another backend allocating in between: the next run is not adjacent to this one */
	if (sift_test_run_gap)
		UnlockReleaseBuffer(sift_alloc_page(wt->index, SIFT_PT_FREE, 0));
#endif
	return start;
}

static void
wt_spare(SiftWriteTarget *wt, BlockNumber start, uint32 n)
{
	if (n == 0)
		return;
	wt->spare = wt_list_room(wt, wt->spare, wt->nspare, &wt->maxspare);
	wt->spare[wt->nspare++] = (SiftExtent) {start, n};
}

#define THROTTLE_PAGES	16		/* pages (128 kB) of I/O that earn one delay */

void
sift_wt_throttle(SiftWriteTarget *wt, int delay_ms)
{
	wt->delay_ms = delay_ms;
}

/* Counts pages of I/O of a throttled target and sleeps a delay for every THROTTLE_PAGES, interruptibly */
void
sift_wt_charge(SiftWriteTarget *wt, uint64 pages)
{
	if (wt->delay_ms == 0)
		return;
	wt->charged += pages;
	for (; wt->charged >= THROTTLE_PAGES; wt->charged -= THROTTLE_PAGES)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, wt->delay_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* Writes a complete page; BULK takes ownership of a smgr_bulk_get_buf page */
static void
wt_put(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	Buffer		buf;

	sift_wt_charge(wt, 1);

	if (wt->bulk)
	{
		smgr_bulk_write(wt->bulk, blkno, (BulkWriteBuffer) page, true);
		return;
	}
#ifdef SIFT_TESTING
	if (sift_test_fail_pages > 0 && ++wt->npages > sift_test_fail_pages)
		elog(ERROR, "sift test: injected failure after %d pages", sift_test_fail_pages);
#endif
	buf = new_locked(wt->index, blkno);
	log_pages(wt->index, &buf, &page, 1);
	UnlockReleaseBuffer(buf);
}

static Page
wt_page(SiftWriteTarget *wt, PGIOAlignedBlock *local)
{
	return wt->bulk ? smgr_bulk_get_buf(wt->bulk)->data : local->data;
}

BlockNumber
sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks)
{
	uint32		got;

	return wt_take(wt, nblocks, nblocks, &got);
}

void
sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	Page		dst = wt_page(wt, (PGIOAlignedBlock *) page);

	Assert(layout_ok(page));
	if (dst != page)
		memcpy(dst, page, BLCKSZ);
	wt_put(wt, blkno, dst);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	BlockNumber blkno = sift_wt_reserve(wt, 1);

	sift_wt_put_page(wt, blkno, page);
	return blkno;
}

static void
bulk_put_meta(SiftWriteTarget *wt)
{
	Page		page = wt_page(wt, NULL);

	sift_page_init(page, SIFT_PT_META, 0);
	*META(page) = wt->meta;
	SiftPageSetUsed(page, sizeof(SiftMetaPageData));
	wt_put(wt, SIFT_METAPAGE_BLKNO, page);
	page = wt_page(wt, NULL);
	sift_page_init(page, SIFT_PT_DIRECTORY, 0);
	SiftPageSetUsed(page, DIR_USED(0));
	wt_put(wt, SIFT_DIRECTORY_BLKNO, page);
}

/* BULK spare: trailing runs shrink the relation, the rest go to a free list */
static void
bulk_release_spare(SiftWriteTarget *wt)
{
	SiftFreeExtent *ext;
	int			k;

	qsort(wt->spare, wt->nspare, sizeof(SiftExtent), cmp_extent);
	while (wt->nspare > 0 && wt->spare[wt->nspare - 1].start + wt->spare[wt->nspare - 1].nblocks == wt->next)
		wt->next = wt->spare[--wt->nspare].start;
	if (wt->nspare == 0)
		return;
	ext = palloc(wt->nspare * sizeof(SiftFreeExtent));
	for (int i = 0; i < wt->nspare; i++)
		ext[i] = (SiftFreeExtent) {wt->spare[i].start, wt->spare[i].nblocks};
	k = (wt->nspare + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
	wt->meta.freelist_head = wt->next;
	for (int i = 0; i < k; i++)
	{
		Page		page = wt_page(wt, NULL);
		SiftFreeListData *fl = FL(page);

		sift_page_init(page, SIFT_PT_FREELIST, 0);
		fl->next = i + 1 < k ? wt->next + 1 : InvalidBlockNumber;
		fl->n = Min(wt->nspare - i * (int) SIFT_FREELIST_CAP, (int) SIFT_FREELIST_CAP);
		memcpy(fl->ext, ext + i * SIFT_FREELIST_CAP, fl->n * sizeof(SiftFreeExtent));
		SiftPageSetUsed(page, FL_USED(fl->n));
		wt_put(wt, wt->next++, page);
	}
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	if (wt->bulk == NULL)
	{
		if (wt->nspare > 0)
		{
			store_update(wt->index, &(SiftDirChange) {.release = wt->spare, .nrelease = wt->nspare,
					.extra = InvalidBuffer}, false, NULL);
			wt->taken = extents_subtract(wt, wt->taken, &wt->ntaken, wt->spare, wt->nspare);
			wt->maxtaken = wt->ntaken;
		}
		wt->nspare = 0;
		return;
	}
	if (!wt->have_meta)
		elog(ERROR, "sift: bulk write target finished without a metapage");
	bulk_release_spare(wt);
	wt->meta.high_water = wt->next;
	bulk_put_meta(wt);
	smgr_bulk_finish(wt->bulk);
	wt->bulk = NULL;
}

void
sift_wt_expect(SiftWriteTarget *wt, uint64 bytes)
{
	wt->expect = bytes / BLCKSZ;
}

/* Transfer completed allocations to the published directory before any subsequent error can abort the write target. */
void
sift_wt_publish(SiftWriteTarget *wt, const SiftDirChange *chg)
{
	Assert(wt->mode == SIFT_WRITE_SHARED);
	store_update(wt->index, chg, true, wt);
}

/* The step that wrote through a SHARED target failed (its transaction is rolled back): the blocks go back */
void
sift_wt_abort(SiftWriteTarget *wt)
{
	if (wt->ntaken > 0)
		store_update(wt->index, &(SiftDirChange) {.release = wt->taken, .nrelease = wt->ntaken,
				.extra = InvalidBuffer}, false, NULL);
	wt->ntaken = wt->nspare = 0;
}

/* ---------- stream writer ---------- */

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->grow = RUN_MIN;
	sift_extlist_init(&sw->ext);
	sw->ext.max = TEST_KNOB(sift_test_max_extents, (int) SIFT_MAX_EXTENTS);
	if (wt->bulk == NULL)
		sw->local = palloc_aligned(sizeof(PGIOAlignedBlock), PG_IO_ALIGN_SIZE, 0);
	return sw;
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	return sw->pos;
}

static void
sw_flush(SiftStreamWriter *sw)
{
	if (sw->page == NULL)
		return;
	SiftPageSetUsed(sw->page, sw->used);
	wt_put(sw->wt, sw->blk, sw->page);
	sw->page = NULL;
	CHECK_FOR_INTERRUPTS();
}

/*
 * Runs grow with the stream, so a segment of any size fits the extents its header page lists: by
 * RUN_MAX blocks as before, then a 16th of what is written, up to RUN_BIG (a metapage-locked
 * extension of 64 MB at most).
 */
static uint32
run_cap(const SiftStreamWriter *sw)
{
	uint32		share = sw->pos / SIFT_PAGE_PAYLOAD / 16;

	/*
	 * A shared stream of unknown size (a parallel build's) gives back the unused end of its last run as a hole in the file, and
	 * the other streams are written next to it: the run is a 16th of what the stream has written, so the hole is at most that.
	 */
	if (sw->wt->expect == 0 && sw->wt->bulk == NULL)
		return Min(RUN_BIG, Max(RUN_MIN, share));
	return Max(RUN_MAX, Min(RUN_BIG, share));
}

/*
 * The shortest run worth taking off the free list. With the size of the stream known, anything that
 * keeps its extents within EXTENT_TARGET, so fragmented space left by merged segments is reused
 * instead of extending the file; unknown, a quarter of the run wanted.
 */
static uint32
run_min(const SiftStreamWriter *sw)
{
	if (sw->wt->expect == 0)
		return Max(RUN_MIN, sw->grow / 4);
	return Max(RUN_MIN, Min(sw->grow, sw->wt->expect / EXTENT_TARGET));
}

/* The run wanted: the growing one, but not far past what the stream is expected to still need */
static uint32
run_want(const SiftStreamWriter *sw, uint32 min)
{
	uint64		expect = sw->wt->expect + sw->wt->expect / 16 + RUN_MIN;

	if (sw->wt->expect == 0)
		return sw->grow;
	return Max(min, Min(sw->grow, expect > sw->blocks ? expect - sw->blocks : 0));
}

static void
sw_start_page(SiftStreamWriter *sw)
{
	if (sw->run_next == sw->run_end)
	{
		uint32		got;
		uint32		min = run_min(sw);

		sw->run_next = wt_take(sw->wt, run_want(sw, min), min, &got);
		sw->run_end = sw->run_next + got;
		sw->blocks += got;
		sw->grow = Min(sw->grow * 2, run_cap(sw));
	}
	sw->blk = sw->run_next++;
	sift_extlist_add(&sw->ext, sw->blk);
	sw->page = wt_page(sw->wt, sw->local);
	sift_page_init(sw->page, SIFT_PT_DATA, sw->gen);
	sw->used = 0;
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0)
	{
		size_t		n;

		if (sw->page == NULL || sw->used == SIFT_PAGE_PAYLOAD)
		{
			sw_flush(sw);
			sw_start_page(sw);
		}
		n = Min(len, SIFT_PAGE_PAYLOAD - sw->used);
		memcpy(SiftPageGetData(sw->page) + sw->used, p, n);
		sw->used += n;
		sw->pos += n;
		p += n;
		len -= n;
	}
}

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	sw_flush(sw);
	wt_spare(sw->wt, sw->run_next, sw->run_end - sw->run_next);
	sw->run_next = sw->run_end;
	*nextents = sw->ext.n;
	return sw->ext.ext;
}

/* ---------- stream reader ---------- */

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents,
			 uint64 gen)
{
	uint64		total;

	memset(sr, 0, sizeof(SiftStreamReader));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	for (int i = 0; i < nextents; i++)
	{
		if (extents[i].nblocks == 0 || extents[i].start < FIRST_DATA_BLKNO ||
			(uint64) extents[i].start + extents[i].nblocks > MaxBlockNumber)
			corrupt(index, "Segment %llu has an invalid extent %u+%u.",
					ULL(gen), extents[i].start, extents[i].nblocks);
	}
	total = sift_extents_pagestart(extents, nextents, sr->pagestart);
	if (total > PG_UINT32_MAX)
		corrupt(index, "Segment %llu spans %llu pages.", ULL(gen), ULL(total));
	sr->npages = total;
}

static BlockNumber
page_block(const SiftStreamReader *sr, uint32 lpage, int *hint)
{
	return sift_extents_block(sr->extents, sr->pagestart, sr->nextents, lpage, hint);
}

static BlockNumber
sr_stream_cb(ReadStream *stream, void *arg, void *per_buffer_data)
{
	SiftStreamReader *sr = arg;

	return sr->pf_issue < sr->pf_end ? page_block(sr, sr->pf_issue++, &sr->pf_cur) : InvalidBlockNumber;
}

static void
sr_unpin(SiftStreamReader *sr)
{
	if (sr->locked)
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
	if (BufferIsValid(sr->buf))
		ReleaseBuffer(sr->buf);
	sr->locked = false;
	sr->buf = InvalidBuffer;
}

static Buffer
sr_fetch(SiftStreamReader *sr, uint32 lpage, BlockNumber blk)
{
	Buffer		buf;

	if (sr->stream == NULL || lpage < sr->pf_next || lpage >= sr->pf_end)
		return ReadBufferExtended(sr->index, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	for (; sr->pf_next < lpage; sr->pf_next++)
		ReleaseBuffer(read_stream_next_buffer(sr->stream, NULL));
	buf = read_stream_next_buffer(sr->stream, NULL);
	Assert(BufferGetBlockNumber(buf) == blk);
	if (++sr->pf_next == sr->pf_end)
	{
		read_stream_end(sr->stream);
		sr->stream = NULL;
	}
	return buf;
}

/* Bytes [in, in + len) of logical page lpage; the page stays share-locked */
static const char *
sr_bytes(SiftStreamReader *sr, uint32 lpage, size_t in, size_t len)
{
	BlockNumber blk = page_block(sr, lpage, &sr->cur);
	Page		page;

	if (sr->buf == InvalidBuffer || sr->blkno != blk)
	{
		sr_unpin(sr);
		sr->buf = sr_fetch(sr, lpage, blk);
		sr->blkno = blk;
	}
	if (!sr->locked)
	{
		LockBuffer(sr->buf, BUFFER_LOCK_SHARE);
		sr->locked = true;
		sift_store_check_page(sr->index, sr->buf, SIFT_PT_DATA, sr->gen);
	}
	page = BufferGetPage(sr->buf);
	if (in + len > SiftPageGetUsed(page))
		corrupt(sr->index, "Read past the %u written bytes of block %u (segment %llu).",
				SiftPageGetUsed(page), blk, ULL(sr->gen));
	return SiftPageGetData(page) + in;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;
	uint32		lpage = off / SIFT_PAGE_PAYLOAD;
	size_t		in = off % SIFT_PAGE_PAYLOAD;
	char	   *dst = scratch;

	if (len == 0)
		return scratch;
	if (off > size || len > size - off)
		corrupt(sr->index, "Read of %zu bytes at offset %llu is past the end of segment %llu.",
				len, ULL(off), ULL(sr->gen));
	if (in + len <= SIFT_PAGE_PAYLOAD)
		return sr_bytes(sr, lpage, in, len);
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - in);

		memcpy(dst, sr_bytes(sr, lpage++, in, n), n);
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
		sr->locked = false;
		CHECK_FOR_INTERRUPTS();
		dst += n;
		len -= n;
		in = 0;
	}
	return scratch;
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;

	if (sr->stream)
		read_stream_end(sr->stream);
	sr->stream = NULL;
	if (len == 0 || off >= size)
		return;
	len = Min(len, size - off);
	sr->pf_issue = sr->pf_next = off / SIFT_PAGE_PAYLOAD;
	sr->pf_end = (off + len - 1) / SIFT_PAGE_PAYLOAD + 1;
	sr->pf_cur = sr->cur;
	if (sr->pf_end - sr->pf_next > 1)
		sr->stream = read_stream_begin_relation(READ_STREAM_FULL, NULL, sr->index, MAIN_FORKNUM,
												sr_stream_cb, sr, 0);
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
	if (sr->locked)
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
	sr->locked = false;
}

void
sift_sr_release(SiftStreamReader *sr)
{
	sr_unpin(sr);
	if (sr->stream)
		read_stream_end(sr->stream);
	sr->stream = NULL;
}

/* ---------- reference maps, orphans, verification ---------- */

static BlockNumber
high_water(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	BlockNumber hwm = META(BufferGetPage(metabuf))->high_water;

	UnlockReleaseBuffer(metabuf);
	return hwm;
}

static void
mark(BlockMap *m, BlockNumber start, uint64 n, const char *what, uint64 gen)
{
	if (n == 0 || start + n > m->hwm)
		corrupt(m->index, "The %s of generation %llu (blocks %u+%llu) lies past the high-water mark %u.",
				what, ULL(gen), start, ULL(n), m->hwm);
	for (BlockNumber b = start; b < start + n; b++)
		if (bms_is_member(b, m->set))
		{
			if (gen == 0)
				corrupt(m->index, "Block %u is referenced twice (%s).", b, what);
			corrupt(m->index, "Block %u is referenced twice (%s of generation %llu).", b, what, ULL(gen));
		}
	m->set = bms_add_range(m->set, start, start + n - 1);
}

static bool
mark_freelist_page(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	BlockMap   *m = arg;

	mark(m, blk, 1, "free list page", 0);
	for (uint32 i = 0; i < fl->n; i++)
		mark(m, fl->ext[i].start, fl->ext[i].nblocks, "free extent", 0);
	return true;
}

/* The extents and the liveness run of a segment, read from its header page */
static SiftExtent *
read_seg_extents(Relation index, const SiftSegEntry *s, uint32 *n, BlockNumber *live_first,
				 uint32 *live_npages)
{
	Buffer		buf = read_locked(index, s->header_blk, BUFFER_LOCK_SHARE);
	Page		page;
	SiftSegHeaderData *hdr;
	SiftExtent *ext;

	sift_store_check_page(index, buf, SIFT_PT_SEGHEADER, s->gen);
	page = BufferGetPage(buf);
	hdr = (SiftSegHeaderData *) SiftPageGetData(page);
	*n = hdr->nextents;
	if (*n > SIFT_MAX_EXTENTS || SiftPageGetUsed(page) < offsetof(SiftSegHeaderData, extents) + *n * sizeof(SiftExtent))
		corrupt(index, "Segment header block %u lists %u extents in %u bytes.",
				s->header_blk, *n, SiftPageGetUsed(page));
	ext = palloc(Max(*n, 1) * sizeof(SiftExtent));
	memcpy(ext, hdr->extents, *n * sizeof(SiftExtent));
	*live_first = hdr->live_first;
	*live_npages = hdr->live_npages;
	UnlockReleaseBuffer(buf);
	return ext;
}

static void
mark_immutable(BlockMap *m, const SiftSegEntry *s)
{
	SiftExtent *ext;
	uint32		n;
	BlockNumber live_first;
	uint32		live_npages;

	mark(m, s->header_blk, 1, "segment header", s->gen);
	ext = read_seg_extents(m->index, s, &n, &live_first, &live_npages);
	for (uint32 i = 0; i < n; i++)
		mark(m, ext[i].start, ext[i].nblocks, "segment extent", s->gen);
	if (live_npages > 0)
		mark(m, live_first, live_npages, "liveness run", s->gen);
	pfree(ext);
}

SiftExtent *
sift_store_immutable_extents(Relation index, const SiftSegEntry *s, int *nextents)
{
	uint32		n;
	BlockNumber live_first;
	uint32		live_npages;
	SiftExtent *ext = read_seg_extents(index, s, &n, &live_first, &live_npages);

	ext = repalloc(ext, (n + 2) * sizeof(SiftExtent));
	ext[n++] = (SiftExtent) {s->header_blk, 1};
	if (live_npages > 0)
		ext[n++] = (SiftExtent) {live_first, live_npages};
	*nextents = n;
	return ext;
}

/* Remap a block while preserving its position inside the original extent. */
BlockNumber
sift_store_relocated_block(BlockNumber blk, const SiftExtent *old, const SiftExtent *fresh, int n)
{
	if (!BlockNumberIsValid(blk))
		return blk;
	for (int i = 0; i < n; i++)
		if (blk >= old[i].start && blk - old[i].start < old[i].nblocks)
			return fresh[i].start + (blk - old[i].start);
	elog(ERROR, "sift: relocation references a block outside its segment");
	pg_unreachable();
}

/* Lowest fitting run, shared with allocation's eligibility and placement. */
static int
relocation_pick(const FreeChain *free, uint32 min, BlockNumber limit)
{
	int best = -1;

	for (int k = 0; k < free->n; k++)
		if (space_fits(&free->ext[k], min, limit) &&
			(best < 0 || free->ext[k].start < free->ext[best].start))
			best = k;
	return best;
}

/*
 * Copy stored pages without decoding, format conversion, promotion or growing stream reservations. Preflight every allocation
 * with the same lowest-fit rule as alloc_blocks. DATA and lane runs can split across holes; a LIVE run must remain contiguous.
 * Plan fixed runs first, then data in logical order, and verify the resulting stream directory fits its existing header page.
 * No accepted move, including its free-list publication, needs to extend the file. The caller atomically publishes the copy.
 */
uint64
sift_store_relocate(Relation rel, SiftWriteTarget *wt, const SiftSegEntry *entry, const SiftExtent *old, int n,
					BlockNumber limit, uint64 budget, SiftSegEntry *entry_new, SiftRelocatePage relocate_page)
{
	Buffer metabuf;
	FreeChain free;
	SiftExtent *source;
	SiftExtent *fresh;
	int parts = 0;
	int data_at = 0;
	int ndata = entry->kind == SIFT_SEG_IMMUTABLE ? n - (old[n - 1].start == entry->header_blk ? 1 : 2) : 0;
	uint64 blocks = 0;
	int capacity;
	uint64 gen;
	PGIOAlignedBlock copy;

	Assert(space_exclusive);
	for (int i = 0; i < n; i++)
		blocks += old[i].nblocks;
	if (blocks > budget / BLCKSZ)
		return 0;
	capacity = entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_MAX_EXTENTS + 2 : blocks;
	source = palloc(capacity * sizeof(SiftExtent));
	fresh = palloc(capacity * sizeof(SiftExtent));
	metabuf = lock_meta(rel, BUFFER_LOCK_SHARE);
	read_chain(rel, metabuf, &free, InvalidFullTransactionId);
	UnlockReleaseBuffer(metabuf);
	/* Immutable streams put header/LIVE last in old; reserve those exact runs before filling the small holes with data. */
	for (int phase = 0; phase < 2; phase++)
	{
		int first = phase == 0 ? ndata : 0;
		int end = phase == 0 ? n : ndata;

		if (phase == 1)
			data_at = parts;
		for (int at = first; at < end; at++)
		{
			/* LIVE precedes the one-page header reservation, so that the header cannot fragment its only fitting run. */
			int i = phase == 0 && entry->kind == SIFT_SEG_IMMUTABLE ? n - 1 - (at - first) : at;
			SiftExtent remaining = old[i];
			bool split = phase == 1 || entry->kind != SIFT_SEG_IMMUTABLE;

			while (remaining.nblocks > 0)
			{
				int best = relocation_pick(&free, split ? 1 : remaining.nblocks, limit);
				uint32 take;

				if (best < 0 || parts == capacity)
					goto skip;
				take = Min(remaining.nblocks, Min(free.ext[best].nblocks, limit - free.ext[best].start));
				source[parts] = (SiftExtent) {remaining.start, take};
				fresh[parts++] = (SiftExtent) {free.ext[best].start, take};
				free.ext[best].start += take;
				free.ext[best].nblocks -= take;
				remaining.start += take;
				remaining.nblocks -= take;
			}
		}
	}
	if (entry->kind == SIFT_SEG_IMMUTABLE && parts - data_at > SIFT_MAX_EXTENTS)
		goto skip;
	/* Leave existing free blocks for publication's worst-case free-list page count, too. */
	{
		uint64 spare = 0;

		for (int k = 0; k < free.n; k++)
			spare += free.ext[k].nblocks;
		if (spare < (n + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP)
			goto skip;
	}
	pfree(free.ext);
	pfree(free.page);
	gen = sift_store_new_gen(rel);
	space_limit = limit;
	for (int i = 0; i < parts; i++)
	{
		BlockNumber reserved = sift_wt_reserve(wt, source[i].nblocks);

		if (reserved != fresh[i].start)
			elog(ERROR, "sift: relocation allocation differs from its plan");
	}
	space_limit = InvalidBlockNumber;
	for (int i = 0; i < parts; i++)
		for (uint32 j = 0; j < source[i].nblocks; j++)
		{
			BlockNumber blk = source[i].start + j;
			Buffer buf = read_locked(rel, blk, BUFFER_LOCK_SHARE);
			Page page = copy.data;
			SiftPageType type = blk == entry->header_blk ?
				(entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_PT_SEGHEADER : SIFT_PT_WBUFHEADER) :
				(entry->kind == SIFT_SEG_IMMUTABLE ? (i < data_at ? SIFT_PT_LIVE : SIFT_PT_DATA) : SIFT_PT_WBUFDATA);

			sift_store_check_page(rel, buf, type, entry->gen);
			memcpy(page, BufferGetPage(buf), BLCKSZ);
			UnlockReleaseBuffer(buf);
			SiftPageGetOpaque(page)->gen = gen;
			if (type == SIFT_PT_SEGHEADER)
			{
				SiftSegHeaderData *h = (SiftSegHeaderData *) SiftPageGetData(page);

				h->gen = gen;
				h->nextents = parts - data_at;
				memcpy(h->extents, fresh + data_at, h->nextents * sizeof(SiftExtent));
				SiftPageSetUsed(page, offsetof(SiftSegHeaderData, extents) + h->nextents * sizeof(SiftExtent));
				if (h->live_npages > 0)
					h->live_first = sift_store_relocated_block(h->live_first, source, fresh, parts);
			}
			else if (type == SIFT_PT_WBUFHEADER)
			{
				SiftWbufHeaderData *h = SiftLaneHdr(page);

				h->gen = gen;
				for (int k = 0; k < SIFT_LANES; k++)
				{
					h->lanes[k].head = sift_store_relocated_block(h->lanes[k].head, source, fresh, parts);
					h->lanes[k].tail = sift_store_relocated_block(h->lanes[k].tail, source, fresh, parts);
				}
			}
			else if (type == SIFT_PT_WBUFDATA)
				SiftLaneHead(page)->next = sift_store_relocated_block(SiftLaneHead(page)->next, source, fresh, parts);
			if (relocate_page && (entry->flags & SIFT_TBL_SEG_WBUF))
				relocate_page(page, source, fresh, parts);
			sift_wt_put_page(wt, fresh[i].start + j, page);
			CHECK_FOR_INTERRUPTS();
		}
	sift_wt_finish(wt);
	*entry_new = *entry;
	entry_new->gen = gen;
	entry_new->header_blk = sift_store_relocated_block(entry->header_blk, source, fresh, parts);
	pfree(source);
	pfree(fresh);
	space_written += blocks;
	return blocks * BLCKSZ;
skip:
	pfree(free.ext);
	pfree(free.page);
	pfree(source);
	pfree(fresh);
	return 0;
}

static void
mark_wbuf(BlockMap *m, const SiftSegEntry *s)
{
	Buffer		buf;
	SiftWbufHeaderData hdr;

	mark(m, s->header_blk, 1, "write buffer header", s->gen);
	buf = read_locked(m->index, s->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(m->index, buf, SIFT_PT_WBUFHEADER, s->gen);
	if (SiftPageGetUsed(BufferGetPage(buf)) < sizeof(SiftWbufHeaderData))
		corrupt(m->index, "Write buffer header block %u is truncated.", s->header_blk);
	hdr = *(SiftWbufHeaderData *) SiftPageGetData(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].head;

		while (BlockNumberIsValid(blk))
		{
			/* mark() rejects a revisit, which also ends a cycle */
			if (blk >= m->hwm)
				m->hwm = high_water(m->index);
			mark(m, blk, 1, "write buffer page", s->gen);
			buf = read_locked(m->index, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(m->index, buf, SIFT_PT_WBUFDATA, s->gen);
			blk = ((SiftWbufPageHeaderData *) SiftPageGetData(BufferGetPage(buf)))->next;
			UnlockReleaseBuffer(buf);
		}
	}
}

Bitmapset *
sift_store_referenced_blocks(Relation index, const SiftDirSnapshot *snap)
{
	BlockMap	m = {index, high_water(index), NULL};
	Buffer		metabuf;

	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			mark_immutable(&m, &snap->segs[i]);
		else
			mark_wbuf(&m, &snap->segs[i]);
	}
	/* Appenders take pages off the free list while a write buffer chain grows, so it is read last */
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	m.hwm = META(BufferGetPage(metabuf))->high_water;
	mark(&m, SIFT_METAPAGE_BLKNO, 2, "metapage and directory", 0);
	sift_freelist_walk(index, metabuf, mark_freelist_page, &m);
	UnlockReleaseBuffer(metabuf);
	return m.set;
}

static bool
in_flight(Page page, const SiftDirSnapshot *snap)
{
	SiftPageOpaque op;

	if (!layout_ok(page))
		return false;
	op = SiftPageGetOpaque(page);
	if (op->type < SIFT_PT_SEGHEADER || op->type > SIFT_PT_WBUFDATA)
		return false;
	if (op->gen >= snap->meta.next_gen)
		return true;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == op->gen)
			return true;
	return false;
}

Bitmapset *
sift_store_orphans(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Bitmapset  *ref;
	Bitmapset  *orphans = NULL;

	sift_dir_read(index, snap);
	ref = sift_store_referenced_blocks(index, snap);
	for (BlockNumber b = FIRST_DATA_BLKNO; b < snap->meta.high_water; b++)
	{
		Buffer		buf;

		if (bms_is_member(b, ref))
			continue;
		buf = read_locked(index, b, BUFFER_LOCK_SHARE);
		if (!in_flight(BufferGetPage(buf), snap))
			orphans = bms_add_member(orphans, b);
		UnlockReleaseBuffer(buf);
	}
	bms_free(ref);
	pfree(snap);
	return orphans;
}

void
sift_store_verify(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	BlockNumber nblocks;

	sift_dir_read(index, snap);
	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks < snap->meta.high_water)
		corrupt(index, "Relation has %u blocks, below the high-water mark %u.", nblocks, snap->meta.high_water);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *s = &snap->segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_WBUF_SEALED || s->gen == 0 ||
			s->gen >= snap->meta.next_gen)
			corrupt(index, "Directory entry %d has kind %u generation %llu (next %llu).",
					i, s->kind, ULL(s->gen), ULL(snap->meta.next_gen));
		for (int j = 0; j < i; j++)
			if (snap->segs[j].gen == s->gen)
				corrupt(index, "Generation %llu appears twice in the directory.", ULL(s->gen));
	}
	bms_free(sift_store_referenced_blocks(index, snap));
	pfree(snap);
}
