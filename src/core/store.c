/*
 * core/store.c - page checks and logging, metapage and directory: the reads and the one record that updates them.
 * The crash-safety protocol (P1-P6) is documented in core/store.h.
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

#include "col/col.h"
#include "bm25/estimate.h"
#include "core/lane.h"
#include "core/store.h"
#include "core/store_int.h"

bool		sift_standby_conflicts = false;

#ifdef SIFT_TESTING
int			sift_test_space_crash = 0;
bool		sift_test_space_error = false;
int			sift_test_fail_pages = 0;
int			sift_test_max_extents = 0;
bool		sift_test_run_gap = false;
#endif

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

/* ---------- pages ---------- */

/*
 * Copies srcs into the exclusively locked bufs and logs them as full images
 * in one record, inside one critical section (P4: content enters the buffer
 * only together with its WAL).
 */
void
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

Buffer
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
void
invalidate_mapping(Relation index)
{
	SharedInvalidationMessage msg = {.rc = {SHAREDINVALRELCACHE_ID, MyDatabaseId, RelationGetRelid(index)}};

	SendSharedInvalidMessages(&msg, 1);
	LocalExecuteInvalidationMessage(&msg);
}

void
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
		/* a build that reads less than the newest segment format of the index must refuse it; a column segment's format is its own (col/col.h), its unfrozen rows raise the minor */
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
