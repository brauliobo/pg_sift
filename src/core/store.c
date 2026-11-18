/*
 * core/store.c - page checks and logging, metapage and directory: the reads and the one record that updates them.
 * The crash-safety protocol (P1-P6) is documented in core/store.h.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "storage/sinval.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"

#include "col/col.h"
#include "core/ops.h"
#include "core/restrict.h"
#include "core/extlock.h"
#include "core/store.h"
#include "core/store_int.h"
#include "core/visibility.h"

bool		sift_standby_conflicts = true;

/* Everything inserted into the WAL so far is on disk. GetXLogInsertRecPtr is the position of the next record: when the last one ended with a page it is the one after the header of the next page, which nothing has reached and XLogFlush refuses */
void
sift_wal_flush_inserted(void)
{
	XLogFlush(GetXLogInsertEndRecPtr());
}

#ifdef SIFT_TESTING
int			sift_test_space_crash = 0;
bool		sift_test_space_error = false;
int			sift_test_fail_pages = 0;
int			sift_test_max_extents = 0;
bool		sift_test_run_gap = false;
int			sift_test_crash_dir = 0;
int			sift_test_crash_kind = 0;
bool		sift_test_crash_worker = false;
bool		sift_test_cut_with_standby = false;
int			sift_test_dir_updates = 0;
int			sift_test_crash_clear = 0;
int			sift_test_clears = 0;
#endif

#ifdef SIFT_TESTING
void
sift_store_space_test_point(int phase)
{
	if (sift_test_space_crash == phase)
	{
		ereport(LOG, (errmsg("sift test: space crash phase %d", phase)));
		sift_wal_flush_inserted();
		kill(MyProcPid, SIGKILL);
	}
}

/*
 * sift.test_crash_dir = n faults the process, its log flushed, before (n > 0) or after (n < 0) the |n|-th directory update it makes
 * since the setting (every build, promotion, merge, VACUUM and table sealing publishes through store_update); sift.test_crash_kind picks the fault: 0 SIGKILL, 1 an ERROR, 2 a wait for
 * advisory lock (0x51F7, 13), where a test cancels or terminates the process, stops the server or lets it go on. The update holds interrupts while it has the buffers of the metapage and
 * the directory locked: the wait is outside of that, before the update takes its locks (n > 0) and after it released them (n < 0), where a cancel and a terminate act at once; a SIGKILL
 * and an ERROR before the update are inside it; an ERROR or a wait after it comes after the update has handed its blocks to the directory and released its buffers, and only after one that publishes: a caller of the others updates its own books once it returns. An update under the relation extension lock (the appends of a value store) waits where the lock is released: PostgreSQL forbids a wait for a heavyweight lock under it. sift.test_crash_worker picks background workers instead of the other processes. sift._test_dir_updates() is the count.
 */
static bool
dir_fault_here(bool after)
{
	return sift_test_crash_dir != 0 && (MyBackendType == B_BG_WORKER) == sift_test_crash_worker &&
		sift_test_crash_dir == (after ? -sift_test_dir_updates : sift_test_dir_updates);
}

static bool dir_books_pending;
static bool dir_wait_pending;

/* A wait is a heavyweight lock, which PostgreSQL forbids under the relation extension lock: a directory update under it waits when the lock is released */
static void
dir_wait(void)
{
	if (sift_extension_held())
		dir_wait_pending = true;
	else
		sift_test_wait(13);
}

void
sift_store_dir_test_unlocked(void)
{
	if (!dir_wait_pending)
		return;
	dir_wait_pending = false;
	sift_test_wait(13);
}

void
sift_store_dir_test_enter(void)
{
	sift_test_dir_updates++;
	dir_books_pending = false;
	if (sift_test_crash_kind == 2 && dir_fault_here(false))
		dir_wait();
}

static void
dir_fault_error(bool after)
{
	ereport(ERROR, (errcode(ERRCODE_OPERATOR_INTERVENTION), errmsg("sift test: error %s directory update %d", after ? "after" : "before", sift_test_dir_updates)));
}

static void
dir_fault_after(void)
{
	if (sift_test_crash_kind == 2)
		dir_wait();
	else if (sift_test_crash_kind == 1)
		dir_fault_error(true);
}

/* after an update that publishes the directory: its caller has nothing left to book; after one that does not, the fault waits for the caller's books (sift_store_dir_test_books) */
void
sift_store_dir_test_leave(bool publish)
{
	if (!dir_fault_here(true))
		return;
	if (publish)
		dir_fault_after();
	else
		dir_books_pending = true;
}

/* the caller of an update that does not publish has updated its own books: the fault of "after update n" goes here */
void
sift_store_dir_test_books(void)
{
	if (!dir_books_pending)
		return;
	dir_books_pending = false;
	dir_fault_after();
}

void
sift_store_dir_test_point(bool after)
{
	if (sift_test_crash_kind == 2 || (sift_test_crash_kind == 1 && after) || !dir_fault_here(after))
		return;
	ereport(LOG, (errmsg("sift test: fault %d %s directory update %d", sift_test_crash_kind, after ? "after" : "before", sift_test_dir_updates)));
	if (!XLogRecPtrIsInvalid(XactLastRecEnd))
		XLogFlush(XactLastRecEnd);
	if (sift_test_crash_kind == 1)
		dir_fault_error(false);
	kill(MyProcPid, SIGKILL);
}

/* sift.test_crash_clear = n kills the process with SIGKILL, its WAL flushed, when a pass that clears liveness bits (P8) has visited its nth segment */
void
sift_store_clear_test_point(void)
{
	if (sift_test_crash_clear == 0 || (MyBackendType == B_BG_WORKER) != sift_test_crash_worker || ++sift_test_clears != sift_test_crash_clear)
		return;
	ereport(LOG, (errmsg("sift test: crash after the segment %d of a pass that clears liveness bits", sift_test_clears)));
	sift_wal_flush_inserted();
	kill(MyProcPid, SIGKILL);
}
#endif

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
	for (int i = 0; i < n; i++)
		SIFT_INVARIANT(sift_page_layout_ok(srcs[i]), "sift: block %u is logged as a page that is not laid out as one of the index", BufferGetBlockNumber(bufs[i]));
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

#define LIVE_CORRUPT(index, ...) \
	SIFT_DAMAGE_INDEX(index, \
					  errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
					  errdetail(__VA_ARGS__), \
					  errhint("A sift_col index that a VACUUM of pg_sift 4.2.1 to 4.3.5 damaged is restored in place by sift.col_repair; REINDEX rebuilds any index."))

#define PAGE_CORRUPT(index, type, ...) \
	do { \
		if ((type) == SIFT_PT_LIVE) \
			LIVE_CORRUPT(index, __VA_ARGS__); \
		else \
			corrupt(index, __VA_ARGS__); \
	} while (0)

void
sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen)
{
	Page		page = BufferGetPage(buf);
	PageHeader	ph = (PageHeader) page;
	BlockNumber blkno = BufferGetBlockNumber(buf);
	SiftPageOpaqueData found;

	if (!sift_page_layout_ok(page))
		PAGE_CORRUPT(index, type, "Block %u has an invalid page layout (lower %u, upper %u, special %u).",
					 blkno, ph->pd_lower, ph->pd_upper, ph->pd_special);
	found = *SiftPageGetOpaque(page);
	if (likely(found.type == type && found.gen == gen && found.flags == 0 && found.reserved == 0))
		return;
	if (found.type == type && found.gen == gen)
		PAGE_CORRUPT(index, type, "Block %u: the page flags (%u) or reserved word (%u) of the special space are not zero.", blkno, found.flags, found.reserved);
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
	PAGE_CORRUPT(index, type, "Block %u: expected page type %d generation %llu, found type %d generation %llu.",
				 blkno, type, ULL(gen), found.type, ULL(found.gen));
}

/*
 * The SQL functions and views that look inside an index read its metapage, which an index of CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY has not got until its
 * build ends (the file is empty, or written past shared buffers with the metapage last, and a standby replays the build from the same WAL): while it is not ready, or when the
 * build failed and left it so, the read is no corruption but a question the index cannot answer yet.
 */
void
sift_store_require_ready(Relation index)
{
	if (!index->rd_index->indisready)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" is not ready to be read", RelationGetRelationName(index)),
				 errdetail("Its CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY has not finished building it."),
				 errhint("Wait for the build to finish, or DROP INDEX the one a failed build left.")));
}

/*
 * The metapage minor a build must read to understand the segment: the segment format of a BM25 segment, 1.3 for a column segment with unfrozen
 * rows (which an older build would show as final); the other column segments need no more than the base, their format is the column engine's own
 */
static uint16
seg_minor(Relation index, const SiftSegEntry *s)
{
	if (index->rd_rel->relam == sift_catalog()->am)
		return Min(s->format, SIFT_FORMAT_MINOR);
	return (s->flags & SIFT_SEG_VERS) ? SIFT_FORMAT_MINOR_VERS : SIFT_FORMAT_MINOR_BASE;
}

static void
check_meta(Relation index, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	SiftMetaPageData *meta = META(page);

	sift_store_check_page(index, buf, SIFT_PT_META, 0);
	if (meta->magic != SIFT_MAGIC || SiftPageGetUsed(page) < SIFT_META_BASE_SIZE || SiftPageGetUsed(page) > sizeof(SiftMetaPageData))
		corrupt(index, "Bad metapage (magic 0x%08x, %u bytes).", meta->magic, SiftPageGetUsed(page));
	if (meta->format_major != SIFT_FORMAT_MAJOR || meta->format_minor > SIFT_FORMAT_MINOR)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" has on-disk format %u.%u, this build (pg_sift %s) reads formats %u.0 to %u.%u",
						RelationGetRelationName(index), meta->format_major, meta->format_minor, SIFT_MODULE_VERSION,
						SIFT_FORMAT_MAJOR, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR),
				 errhint("Install the pg_sift release that wrote the index on every node that reads it, or REINDEX it.")));
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

uint32
sift_store_meta_flags(Relation index)
{
	Buffer		buf;
	uint32		flags;

	if (RelationGetNumberOfBlocks(index) == 0)
		return 0;
	buf = lock_meta(index, BUFFER_LOCK_SHARE);
	flags = META(BufferGetPage(buf))->flags;
	UnlockReleaseBuffer(buf);
	return flags;
}

bool
sift_store_meta_has_flag(Relation index, uint32 flag)
{
	Buffer		buf;
	SiftMetaPageData *meta;
	bool		on;

	if (RelationGetNumberOfBlocks(index) == 0)
		return false;
	buf = read_locked(index, SIFT_METAPAGE_BLKNO, BUFFER_LOCK_SHARE);
	meta = META(BufferGetPage(buf));
	on = meta->magic == SIFT_MAGIC && (meta->flags & flag) != 0;
	UnlockReleaseBuffer(buf);
	return on;
}

void
sift_dir_read(Relation index, SiftDirSnapshot *snap)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	SiftDirectoryData *dir = DIR(BufferGetPage(dirbuf));

	snap->meta = *META(BufferGetPage(metabuf));
	if (SiftPageGetUsed(BufferGetPage(metabuf)) < sizeof(SiftMetaPageData))
		memset((char *) &snap->meta + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
	snap->version = dir->version;
	snap->nsegs = dir->nsegs;
	memcpy(snap->segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
	UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
}

/* The directory without the metapage: a caller that reads only the segments (one page where sift_dir_read reads two); snap->meta is zero, so a copy of it for a scan that opens the index must be of sift_dir_read */
void
sift_dir_read_segs(Relation index, SiftDirSnapshot *snap)
{
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	SiftDirectoryData *dir = DIR(BufferGetPage(dirbuf));

	memset(&snap->meta, 0, sizeof(snap->meta));
	snap->version = dir->version;
	snap->nsegs = dir->nsegs;
	memcpy(snap->segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
	UnlockReleaseBuffer(dirbuf);
}

/* The schema hash and the flags the segments of a column index were written with; false while the index has no segment (CREATE INDEX has not started to write, or nothing was
 * written yet: sift_dir_layout_differs) and for a partitioned index, which has no storage (PostgreSQL 19 asks it for amcanreturn while it plans a query on the partitioned table) */
bool
sift_store_meta_peek(Relation index, uint32 *hash, uint32 *flags)
{
	SiftDirSnapshot *snap;
	bool		written;

	if (index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX || RelationGetNumberOfBlocks(index) <= SIFT_DIRECTORY_BLKNO)
		return false;
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	*hash = snap->meta.analyzer_hash;
	*flags = snap->meta.flags;
	written = sift_segs_have_layout(snap->segs, snap->nsegs);
	pfree(snap);
	return written;
}

static void
set_layout_hash(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->analyzer_hash = *(uint32 *) arg;
}

/* The hash the segments of the index are written for, once the layout has been given other names (a renamed column): the leaves are the same, so no segment is written again */
void
sift_store_rekey_layout(Relation index, uint32 hash)
{
	sift_dir_update(index, &(SiftDirChange) {.fn = set_layout_hash,.arg = &hash,.extra = InvalidBuffer});
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
	SiftXlog state = sift_xlog_start(index);
	uint64		gen = META(sift_xlog_register(&state, metabuf, 0))->next_gen++;

	sift_xlog_finish(&state);
	UnlockReleaseBuffer(metabuf);
	return gen;
}

void
sift_meta_update(Relation index, SiftMetaMutator fn, void *arg)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog state = sift_xlog_start(index);
	Page		mp = sift_xlog_register(&state, metabuf, 0);

	if (SiftPageGetUsed(mp) < sizeof(SiftMetaPageData))
		memset((char *) META(mp) + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
	if (!fn(META(mp), arg))
	{
		sift_xlog_abort(&state);
		UnlockReleaseBuffer(metabuf);
		return;
	}
	if (sift_meta_extended(META(mp)))
		SiftPageSetUsed(mp, sizeof(SiftMetaPageData));
	sift_xlog_finish(&state);
	UnlockReleaseBuffer(metabuf);
}

/* Validates the mutator's result inside the record; aborts it on failure */
static void
finish_dir(Relation index, SiftXlog *state, const SiftMetaPageData *before, const SiftMetaPageData *meta,
		   Page dirpage)
{
	SiftDirectoryData *dir = DIR(dirpage);
	const char *err = NULL;

	if (meta->magic != before->magic || meta->high_water != before->high_water ||
		meta->freelist_head != before->freelist_head || meta->next_gen < before->next_gen)
		err = "allocation state changed";
	else if (dir->nsegs > SIFT_MAX_SEGMENTS)
	{
		sift_xlog_abort(state);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" cannot hold more than %d segments", RelationGetRelationName(index), SIFT_MAX_SEGMENTS),
				 errhint("Run sift.merge() on the index.")));
	}
	for (int i = 0; !err && i < dir->nsegs; i++)
	{
		const SiftSegEntry *s = &dir->segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_VALRUN)
			err = "invalid segment kind";
		else if (s->gen == 0 || s->gen >= meta->next_gen)
			err = "segment generation was not reserved";
		else if (s->format > Max(Max(SIFT_FORMAT_MINOR, SIFT_COL_FORMAT_MAX_READ), SIFT_COL_FORMAT_WIDE_ROWS))	/* the column engine numbers its segment formats on its own */
			err = "unknown segment format";
		for (int j = 0; !err && j < i; j++)
			if (dir->segs[j].gen == s->gen)
				err = "duplicate segment generation";
	}
	if (err)
	{
		sift_xlog_abort(state);
		SIFT_INVARIANT(false, "sift: invalid directory update: %s", err);
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
	return !sift_in_vacuum() &&
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
	Buffer		metabuf;
	Buffer		dirbuf;
	FreePlan	plan = {.flbuf = InvalidBuffer, .newhead = InvalidBlockNumber};
	SiftXlog state;
	SiftMetaPageData before;
	Page		mp;
	Page		dp = NULL;
	Page		fp = NULL;
	Page		xp = NULL;

	/* a test lets a reader start between the xid and the locks: its snapshot is later than the xid and reads the directory as it is before this change */
	SIFT_TEST_GATE(pending && sift_test_release_gate, 6);
	SIFT_DIR_TEST_ENTER();
	metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	dirbuf = publish ? lock_dir(index, BUFFER_LOCK_EXCLUSIVE) : InvalidBuffer;
	SIFT_DIR_TEST_POINT(false);
	if (pending && !FullTransactionIdIsValid(safexid))
		safexid = ReadNextFullTransactionId();
	if (chg->nrelease > 0)
		prepare_free(index, metabuf, chg->release, chg->nrelease, safexid, &plan);
	state = sift_xlog_start(index);
	mp = sift_xlog_register(&state, metabuf, 0);
	if (publish)
		dp = sift_xlog_register(&state, dirbuf, 0);
	if (BufferIsValid(plan.flbuf))
		fp = sift_xlog_register(&state, plan.flbuf, 0);
	if (BufferIsValid(chg->extra))
		xp = sift_xlog_register(&state, chg->extra, 0);
	if (SiftPageGetUsed(mp) < sizeof(SiftMetaPageData))
		memset((char *) META(mp) + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
	before = *META(mp);
	if (publish)
	{
		if (chg->fn)
			chg->fn(META(mp), DIR(dp), xp, chg->arg);
		/* the page ends where the base does until a failure note or a floor is written: pd_lower covers the bytes of a note once it has one */
		if (sift_meta_extended(META(mp)))
			SiftPageSetUsed(mp, sizeof(SiftMetaPageData));
		/* a build that reads less than the newest segment format of the index must refuse it; a column segment's format is its own (col/col.h), its unfrozen rows raise the minor */
		for (int i = 0; i < DIR(dp)->nsegs; i++)
			META(mp)->format_minor = Max(META(mp)->format_minor, seg_minor(index, &DIR(dp)->segs[i]));
		finish_dir(index, &state, &before, META(mp), dp);
	}
	apply_free(META(mp), fp, &plan);
	/* Do not accept cancellation between a durable physical mapping change and its immediate invalidation. */
	if (publish && space_exclusive && chg->nrelease > 0)
		HOLD_INTERRUPTS();
	sift_xlog_finish(&state);
	SIFT_DIR_TEST_POINT(true);
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
	SIFT_DIR_TEST_LEAVE(publish);
}

void
sift_dir_update(Relation index, const SiftDirChange *chg)
{
	store_update(index, chg, true, NULL);
}

void
sift_dir_publish(Relation index, const SiftDirChange *chg, SiftWriteTarget *wt)
{
	store_update(index, chg, true, wt);
}
