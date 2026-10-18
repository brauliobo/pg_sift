/*
 * sift_store.c - metapage, directory, free list, extent allocation, write
 * targets, byte streams and WAL helpers. The crash-safety protocol (P1-P6)
 * is documented in sift_store.h.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "catalog/pg_control.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bulk_write.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "sift_store.h"

#define SPECIAL_SIZE		MAXALIGN(sizeof(SiftPageOpaqueData))
#define FIRST_DATA_BLKNO	(SIFT_DIRECTORY_BLKNO + 1)
#define RUN_MIN				16
#define RUN_MAX				1024

#define META(page)	((SiftMetaPageData *) SiftPageGetData(page))
#define DIR(page)	((SiftDirectoryData *) SiftPageGetData(page))
#define FL(page)	((SiftFreeListData *) SiftPageGetData(page))
#define DIR_USED(n)	(offsetof(SiftDirectoryData, segs) + (n) * sizeof(SiftSegEntry))
#define FL_USED(n)	(offsetof(SiftFreeListData, ext) + (n) * sizeof(SiftFreeExtent))
#define ULL(x)		((unsigned long long) (x))

#define corrupt(index, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
			 errdetail(__VA_ARGS__)))

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
	SiftExtentList ext;
};

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

static Buffer
read_freelist(Relation index, BlockNumber blk, BlockNumber hwm, uint32 hops, int mode,
			  SiftFreeListData **fl)
{
	Buffer		buf;

	if (blk < FIRST_DATA_BLKNO || blk >= hwm || hops >= hwm)
		corrupt(index, "Free list chain reaches block %u after %u pages.", blk, hops);
	buf = read_locked(index, blk, mode);
	*fl = check_freelist(index, buf, hwm);
	return buf;
}

/* The GlobalVis horizon is the heap's: it asserts a table, not the index. A sift_table is its own table */
bool
sift_store_removable(Relation index, FullTransactionId xid)
{
	Relation	heap;
	bool		removable;

	if (index->rd_rel->relkind != RELKIND_INDEX)
		return GlobalVisCheckRemovableFullXid(index, xid);
	heap = RelationIdGetRelation(index->rd_index->indrelid);
	removable = GlobalVisCheckRemovableFullXid(heap, xid);
	RelationClose(heap);
	return removable;
}

static bool
reusable(Relation index, FullTransactionId safexid)
{
	return !FullTransactionIdIsValid(safexid) || sift_store_removable(index, safexid);
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

/*
 * Takes up to want contiguous blocks (at least min): the first reusable
 * free-list record holding want, else the largest holding min, else extends
 * by want. metabuf is locked exclusively, which also guards the free list.
 */
static BlockNumber
alloc_blocks(Relation index, Buffer metabuf, uint32 want, uint32 min, uint32 *got)
{
	SiftMetaPageData *meta = META(BufferGetPage(metabuf));
	BlockNumber blk = meta->freelist_head;
	BlockNumber pick_blk = InvalidBlockNumber;
	int			pick = -1;
	uint32		pick_n = 0;

	for (uint32 hops = 0; BlockNumberIsValid(blk) && pick_n < want; hops++)
	{
		SiftFreeListData *fl;
		Buffer		buf = read_freelist(index, blk, meta->high_water, hops, BUFFER_LOCK_SHARE, &fl);

		for (uint32 i = 0; i < fl->n && pick_n < want; i++)
		{
			SiftFreeExtent *e = &fl->ext[i];

			if (e->nblocks >= min && e->nblocks > pick_n && reusable(index, e->safexid))
			{
				pick_blk = blk;
				pick = i;
				pick_n = e->nblocks;
			}
		}
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
	*got = pick >= 0 ? Min(want, pick_n) : want;
	return pick >= 0 ? take_free(index, pick_blk, pick, *got) : extend(index, metabuf, want);
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
	SiftFreeExtent *have = NULL;
	int			nhave = 0;
	int			maxhave = 0;
	BlockNumber blk = meta->freelist_head;
	BlockNumber room = InvalidBlockNumber;
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

	for (uint32 hops = 0; BlockNumberIsValid(blk); hops++)
	{
		SiftFreeListData *fl;
		Buffer		buf = read_freelist(index, blk, meta->high_water, hops, BUFFER_LOCK_SHARE, &fl);

		if (!BlockNumberIsValid(room) && SIFT_FREELIST_CAP - fl->n >= (uint32) n)
			room = blk;
		if (nhave + fl->n + 1 > maxhave)
		{
			maxhave = Max(64, 2 * (nhave + fl->n + 1));
			have = have ? repalloc(have, maxhave * sizeof(SiftFreeExtent)) :
				palloc(maxhave * sizeof(SiftFreeExtent));
		}
		memcpy(have + nhave, fl->ext, fl->n * sizeof(SiftFreeExtent));
		nhave += fl->n;
		have[nhave++] = (SiftFreeExtent) {blk, 1};
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
	if (nhave > 0)
		qsort(have, nhave, sizeof(SiftFreeExtent), cmp_free);
	for (int i = 0, j = 0; i < n && j < nhave;)
	{
		if (plan->ext[i].start + plan->ext[i].nblocks <= have[j].start)
			i++;
		else if (have[j].start + have[j].nblocks <= plan->ext[i].start)
			j++;
		else
			corrupt(index, "Block %u is released twice.", Max(plan->ext[i].start, have[j].start));
	}
	if (BlockNumberIsValid(room))
		plan->flbuf = read_locked(index, room, BUFFER_LOCK_EXCLUSIVE);
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

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_MUTABLE_SEALED)
			err = "invalid segment kind";
		else if (s->gen == 0 || s->gen >= meta->next_gen)
			err = "segment generation was not reserved";
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
 * as safe but is only passed once some later xid completes.
 */
static bool
xid_safexid(void)
{
	return FullTransactionIdIsValid(GetTopFullTransactionIdIfAny()) ||
		!(IsInParallelMode() || IsParallelWorker());
}

static void
store_update(Relation index, const SiftDirChange *chg, bool publish)
{
	bool		pending = publish && chg->nrelease > 0;
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
		finish_dir(state, &before, META(mp), dp);
	}
	apply_free(META(mp), fp, &plan);
	GenericXLogFinish(state);
	if (BufferIsValid(plan.flbuf))
		UnlockReleaseBuffer(plan.flbuf);
	if (publish)
		UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
}

void
sift_dir_update(Relation index, const SiftDirChange *chg)
{
	store_update(index, chg, true);
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

/*
 * Rewrites the free list as one sorted, coalesced chain in blocks that are
 * free now, then switches the head in one record; the old chain pages become
 * free records. A crash before the switch leaves the old chain intact.
 */
int
sift_recycle(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	SiftMetaPageData *meta = META(BufferGetPage(metabuf));
	BlockNumber blk = meta->freelist_head;
	SiftFreeExtent *ext = NULL;
	BlockNumber *old = NULL;
	BlockNumber *blks;
	int			n = 0;
	int			npages = 0;
	int			cleared = 0;
	int			k;
	GenericXLogState *state;

	for (uint32 hops = 0; BlockNumberIsValid(blk); hops++)
	{
		SiftFreeListData *fl;
		Buffer		buf = read_freelist(index, blk, meta->high_water, hops, BUFFER_LOCK_SHARE, &fl);

		ext = ext ? repalloc(ext, (n + fl->n + npages + 1) * sizeof(SiftFreeExtent)) :
			palloc((fl->n + 1) * sizeof(SiftFreeExtent));
		old = old ? repalloc(old, (npages + 1) * sizeof(BlockNumber)) : palloc(sizeof(BlockNumber));
		for (uint32 i = 0; i < fl->n; i++)
		{
			ext[n] = fl->ext[i];
			if (FullTransactionIdIsValid(ext[n].safexid) &&
				sift_store_removable(index, ext[n].safexid))
			{
				ext[n].safexid = InvalidFullTransactionId;
				cleared++;
			}
			n++;
		}
		old[npages++] = blk;
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
	if (npages == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}
	k = n;
	n = coalesce(ext, n);
	if (cleared == 0 && n == k && npages == 1)
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}
	k = (n + npages + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
	blks = palloc(k * sizeof(BlockNumber));
	/* New pages come from free records, never from the live old chain */
	for (int i = 0, j = 0; i < k; i++)
	{
		while (j < n && FullTransactionIdIsValid(ext[j].safexid))
			j++;
		if (j == n)
			blks[i] = extend(index, metabuf, 1);
		else
		{
			blks[i] = ext[j].start++;
			ext[j].nblocks--;
			if (ext[j].nblocks == 0)
				memmove(&ext[j], &ext[j + 1], (--n - j) * sizeof(SiftFreeExtent));
		}
	}
	for (int i = 0; i < npages; i++)
		ext[n++] = (SiftFreeExtent) {old[i], 1};
	n = coalesce(ext, n);
	write_chain(index, blks, k, ext, n, InvalidBlockNumber);
	state = GenericXLogStart(index);
	META(GenericXLogRegisterBuffer(state, metabuf, 0))->freelist_head = blks[0];
	GenericXLogFinish(state);
	UnlockReleaseBuffer(metabuf);
	return cleared;
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
sift_store_init_meta(SiftWriteTarget *wt, const SiftOptions *opts, uint32 analyzer_hash)
{
	SiftMetaPageData *meta = &wt->meta;

	if (wt->bulk == NULL)
		elog(ERROR, "sift: the metapage is written by BULK or INIT write targets only");
	memset(meta, 0, sizeof(SiftMetaPageData));
	meta->magic = SIFT_MAGIC;
	meta->format_major = SIFT_FORMAT_MAJOR;
	meta->format_minor = SIFT_FORMAT_MINOR;
	meta->k1 = (float4) opts->k1;
	meta->b = (float4) opts->b;
	meta->analyzer_hash = analyzer_hash;
	meta->next_gen = 1;
	meta->freelist_head = InvalidBlockNumber;
	wt->have_meta = true;
}

/* Takes want blocks (at least min): spare runs first, then fresh ones */
static BlockNumber
wt_take(SiftWriteTarget *wt, uint32 want, uint32 min, uint32 *got)
{
	Buffer		metabuf;
	BlockNumber start;

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
	metabuf = lock_meta(wt->index, BUFFER_LOCK_EXCLUSIVE);
	start = alloc_blocks(wt->index, metabuf, want, min, got);
	UnlockReleaseBuffer(metabuf);
	return start;
}

static void
wt_spare(SiftWriteTarget *wt, BlockNumber start, uint32 n)
{
	if (n == 0)
		return;
	if (wt->nspare == wt->maxspare)
	{
		wt->maxspare = Max(8, 2 * wt->maxspare);
		/* lives as long as wt, whatever context the stream writer ran in */
		wt->spare = wt->spare ? repalloc(wt->spare, wt->maxspare * sizeof(SiftExtent)) :
			MemoryContextAlloc(GetMemoryChunkContext(wt), wt->maxspare * sizeof(SiftExtent));
	}
	wt->spare[wt->nspare++] = (SiftExtent) {start, n};
}

/* Writes a complete page; BULK takes ownership of a smgr_bulk_get_buf page */
static void
wt_put(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	Buffer		buf;

	if (wt->bulk)
	{
		smgr_bulk_write(wt->bulk, blkno, (BulkWriteBuffer) page, true);
		return;
	}
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
			store_update(wt->index, &(SiftDirChange) {.release = wt->spare, .nrelease = wt->nspare,
					.extra = InvalidBuffer}, false);
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

/* ---------- stream writer ---------- */

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->grow = RUN_MIN;
	sift_extlist_init(&sw->ext);
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

static void
sw_start_page(SiftStreamWriter *sw)
{
	if (sw->run_next == sw->run_end)
	{
		uint32		got;

		sw->run_next = wt_take(sw->wt, sw->grow, Max(RUN_MIN, sw->grow / 4), &got);
		sw->run_end = sw->run_next + got;
		sw->grow = Min(sw->grow * 2, RUN_MAX);
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

static void
mark_freelist(BlockMap *m, BlockNumber blk)
{
	for (uint32 hops = 0; BlockNumberIsValid(blk); hops++)
	{
		SiftFreeListData *fl;
		Buffer		buf = read_freelist(m->index, blk, m->hwm, hops, BUFFER_LOCK_SHARE, &fl);

		mark(m, blk, 1, "free list page", 0);
		for (uint32 i = 0; i < fl->n; i++)
			mark(m, fl->ext[i].start, fl->ext[i].nblocks, "free extent", 0);
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
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

static void
mark_mutable(BlockMap *m, const SiftSegEntry *s)
{
	Buffer		buf;
	SiftMutHeaderData hdr;

	mark(m, s->header_blk, 1, "mutable header", s->gen);
	buf = read_locked(m->index, s->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(m->index, buf, SIFT_PT_MUTHEADER, s->gen);
	if (SiftPageGetUsed(BufferGetPage(buf)) < sizeof(SiftMutHeaderData))
		corrupt(m->index, "Mutable header block %u is truncated.", s->header_blk);
	hdr = *(SiftMutHeaderData *) SiftPageGetData(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].head;

		while (BlockNumberIsValid(blk))
		{
			/* mark() rejects a revisit, which also ends a cycle */
			if (blk >= m->hwm)
				m->hwm = high_water(m->index);
			mark(m, blk, 1, "mutable page", s->gen);
			buf = read_locked(m->index, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(m->index, buf, SIFT_PT_MUTDATA, s->gen);
			blk = ((SiftMutPageHeaderData *) SiftPageGetData(BufferGetPage(buf)))->next;
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
			mark_mutable(&m, &snap->segs[i]);
	}
	/* Appenders take pages off the free list while a mutable chain grows, so it is read last */
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	m.hwm = META(BufferGetPage(metabuf))->high_water;
	mark(&m, SIFT_METAPAGE_BLKNO, 2, "metapage and directory", 0);
	mark_freelist(&m, META(BufferGetPage(metabuf))->freelist_head);
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
	if (op->type < SIFT_PT_SEGHEADER || op->type > SIFT_PT_MUTDATA)
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

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_MUTABLE_SEALED || s->gen == 0 ||
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
