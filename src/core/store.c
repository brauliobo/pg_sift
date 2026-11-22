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
#include "core/dirchain.h"
#include "core/dirpart.h"
#include "core/limits.h"
#include "core/ops.h"
#include "core/restrict.h"
#include "core/extlock.h"
#include "core/store.h"
#include "core/store_int.h"
#include "core/u64set.h"
#include "core/visibility.h"


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
bool		sift_test_crash_chain = false;
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
dir_fault_match(bool after)
{
	return sift_test_crash_dir != 0 && (MyBackendType == B_BG_WORKER) == sift_test_crash_worker &&
		sift_test_crash_dir == (after ? -sift_test_dir_updates : sift_test_dir_updates);
}

static bool
dir_fault_here(bool after)
{
	return dir_fault_match(after) && (after || !sift_test_crash_chain);
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

/*
 * With sift.test_crash_chain the fault "before the nth update" is raised after the update has written and logged the pages of a new chain and before the record that
 * flips the metapage to them: the point where a crash leaves pages that nothing refers to (SIGKILL or an ERROR; the wait of kind 2 would hold the locks of the directory)
 */
void
sift_store_dir_test_chain_point(void)
{
	if (!sift_test_crash_chain || !dir_fault_match(false))
		return;
	ereport(LOG, (errmsg("sift test: fault %d after the pages of the chain of directory update %d", sift_test_crash_kind, sift_test_dir_updates)));
	if (!XLogRecPtrIsInvalid(XactLastRecEnd))
		XLogFlush(XactLastRecEnd);
	if (sift_test_crash_kind == 1)
		dir_fault_error(false);
	kill(MyProcPid, SIGKILL);
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
	SiftDirSnapshot *snap = sift_dir_read_segs(index);
	bool		found = false;

	for (int i = 0; i < snap->nsegs && !found; i++)
		found = snap->segs[i].gen == gen;
	pfree(snap);
	return found;
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
	if (likely(found.type == type && found.gen == gen && found.flags == 0 && found.part_id == 0))
		return;
	if (found.type == type && found.gen == gen)
		PAGE_CORRUPT(index, type, "Block %u: the page flags (%u) or partition word (%u) of the special space are not zero.", blkno, found.flags, found.part_id);
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
 * rows (which an older build would show as final), 1.7 for the write buffer of a column index whose pages hold key ranges (an older build reads their head as
 * damage); the other column segments need no more than the base, their format is the column engine's own
 */
static uint16
seg_minor(Relation index, const SiftSegEntry *s)
{
	if (index->rd_rel->relam == sift_catalog()->am)
		return Min(s->format, SIFT_FORMAT_MINOR);
	if (sift_seg_is_wbuf(s) && s->format >= SIFT_COL_FORMAT_WBUFKEY)
		return SIFT_FORMAT_MINOR_WBUFKEY;
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
				 errmsg("sift index \"%s\" uses on-disk format %u.%u, newer than this pg_sift supports (up to %u.%u)",
						RelationGetRelationName(index), meta->format_major, meta->format_minor, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR),
				 errhint("Upgrade the pg_sift library to the release that wrote this index, on every node that reads it.")));
	if (meta->high_water < FIRST_DATA_BLKNO)
		corrupt(index, "High-water mark %u is below the first data block.", meta->high_water);
}

static inline bool
dir_is_chained(const SiftDirectoryData *dir)
{
	return (dir->reserved & SIFT_DIR_CHAINED) != 0;
}

static void
check_dir(Relation index, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	SiftDirectoryData *dir = DIR(page);

	sift_store_check_page(index, buf, SIFT_PT_DIRECTORY, 0);
	if ((dir->reserved & ~SIFT_DIR_CHAINED) != 0 || (dir_is_chained(dir) && dir->nsegs != 0))
		corrupt(index, "Directory holds %u segments with the flags %u.", dir->nsegs, dir->reserved);
	if (dir->nsegs > SIFT_DIR_FLAT_MAX || SiftPageGetUsed(page) != (dir_is_chained(dir) ? DIR_CHAINED_USED : DIR_USED(dir->nsegs)))
		corrupt(index, "Directory holds %u segments in %u bytes.", dir->nsegs, SiftPageGetUsed(page));
	if (dir_is_chained(dir) && (SIFT_DIR_ROOT(dir) <= SIFT_DIRECTORY_BLKNO || SIFT_DIR_ROOT(dir) == InvalidBlockNumber))
		corrupt(index, "The directory names the chain root %u.", SIFT_DIR_ROOT(dir));
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

/* The metapage a snapshot carries: an older one holds fewer fields, and the rest are zero */
static void
copy_meta(SiftDirSnapshot *snap, Page mpage)
{
	snap->meta = *META(mpage);
	if (SiftPageGetUsed(mpage) < sizeof(SiftMetaPageData))
		memset((char *) &snap->meta + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
}

/* The entries of the partitions that may hold a key of the range, read from the chain itself: the memo holds whole chains only */
static SiftDirEntries
range_entries(Relation index, BlockNumber root, uint64 version, const SiftKeyRange *keys, bool *partial)
{
	SiftSegEntry *segs;
	SiftDirChain chain = {0};
	int			n = sift_dirchain_read_range(index, root, version, keys, &segs, &chain);

	pfree(chain.leaf);
	pfree(chain.inner);
	*partial = (uint32) n < chain.nall;
	return (SiftDirEntries) {.segs = segs,.nsegs = n,.blocks = chain.blocks,.nblocks = chain.nblocks};
}

/*
 * The directory under the share locks of the metapage and block 1, as one allocation (meta when asked for: sift_dir_read_segs leaves it zero)
 */
static SiftDirSnapshot *
dir_snapshot(Relation index, bool with_meta, const SiftKeyRange *keys)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	SiftDirectoryData *dir = DIR(BufferGetPage(dirbuf));
	Page		mpage = BufferGetPage(metabuf);
	BlockNumber root = dir_is_chained(dir) ? SIFT_DIR_ROOT(dir) : 0;
	SiftDirSnapshot *snap;
	SiftDirEntries ents = {.segs = dir->segs,.nsegs = dir->nsegs,.kept = true};
	bool		partial = false;

	if (root != 0 && keys != NULL)
		ents = range_entries(index, root, dir->version, keys, &partial);
	else if (root != 0)
		ents = sift_dirchain_entries(index, root, dir->version);
	snap = palloc(offsetof(SiftDirSnapshot, segs) + ents.nsegs * sizeof(SiftSegEntry));
	memset(snap, 0, offsetof(SiftDirSnapshot, segs));
	if (with_meta)
		copy_meta(snap, mpage);
	snap->version = dir->version;
	snap->nsegs = ents.nsegs;
	snap->partial = partial;
	snap->nchain = ents.nblocks;
	if (ents.nblocks > 0)	/* check-memory: allow the pages of one directory chain, 4 bytes each, which the entries it lists bound */
		snap->chain = memcpy(palloc(ents.nblocks * sizeof(BlockNumber)), ents.blocks, ents.nblocks * sizeof(BlockNumber));
	memcpy(snap->segs, ents.segs, ents.nsegs * sizeof(SiftSegEntry));
	sift_dirchain_entries_release(&ents);
	UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
	return snap;
}

SiftDirSnapshot *
sift_dir_read(Relation index)
{
	return dir_snapshot(index, true, NULL);
}

/* The directory less the segments of the partitions that hold no key of the range (sift_range_span): for a reader of those keys, never for a writer or a check of the whole */
SiftDirSnapshot *
sift_dir_read_range(Relation index, const SiftKeyRange *keys)
{
	return dir_snapshot(index, true, keys);
}

/* The directory without the metapage: a caller that reads only the segments (one page where sift_dir_read reads two, while it is block 1 alone); meta is zero, so a copy of it for a scan that opens the index must be of sift_dir_read */
SiftDirSnapshot *
sift_dir_read_segs(Relation index)
{
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	SiftDirectoryData *dir = DIR(BufferGetPage(dirbuf));
	SiftDirSnapshot *snap;

	if (dir_is_chained(dir))
	{
		UnlockReleaseBuffer(dirbuf);
		return dir_snapshot(index, false, NULL);
	}
	snap = palloc0(offsetof(SiftDirSnapshot, segs) + dir->nsegs * sizeof(SiftSegEntry));
	snap->version = dir->version;
	snap->nsegs = dir->nsegs;
	memcpy(snap->segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
	UnlockReleaseBuffer(dirbuf);
	return snap;
}

/* An empty snapshot with room for n entries, for a caller that collects some of another's */
SiftDirSnapshot *
sift_dir_alloc(int n)
{
	return palloc0(offsetof(SiftDirSnapshot, segs) + n * sizeof(SiftSegEntry));
}

SiftDirSnapshot *
sift_dir_copy(const SiftDirSnapshot *snap)
{
	SiftDirSnapshot *copy = palloc(sift_dir_bytes(snap));

	memcpy(copy, snap, sift_dir_bytes(snap));
	copy->chain = NULL;
	copy->nchain = 0;
	return copy;
}

int
sift_dir_cap(const SiftMetaPageData *meta)
{
	return (meta->flags & SIFT_COL_META_FLAG) ? SIFT_DIR_WIDE_MAX : (int) sift_limit(max_segments, SIFT_DIR_POLICY_MAX);
}

/* The snapshot in hand, when the directory is still at its version: its entries are the ones the directory has, and only the metapage can differ */
static bool
reload_meta_only(Relation index, SiftDirSnapshot *snap)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	Buffer		dirbuf = lock_dir(index, BUFFER_LOCK_SHARE);
	bool		same = DIR(BufferGetPage(dirbuf))->version == snap->version;

	if (same)
		copy_meta(snap, BufferGetPage(metabuf));
	UnlockReleaseBuffer(dirbuf);
	UnlockReleaseBuffer(metabuf);
	return same;
}

void
sift_dir_reload(Relation index, SiftDirSnapshot **slot, MemoryContext cxt)
{
	MemoryContext old;
	SiftDirSnapshot *fresh;

	if (*slot != NULL && reload_meta_only(index, *slot))
		return;
	old = MemoryContextSwitchTo(cxt);
	fresh = sift_dir_read(index);

	MemoryContextSwitchTo(old);
	if (fresh->chain != NULL)
		pfree(fresh->chain);
	fresh->chain = NULL;
	fresh->nchain = 0;
	if (*slot != NULL)
		pfree(*slot);
	*slot = fresh;
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
	snap = sift_dir_read(index);
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

/* Is the directory chained, as a pinned block 1 shows it without its lock: a hint for the update that decides whether to take a transaction id before it locks, which a change of shape between the peek and the locks does not break (store_update asks again) */
static bool
dir_chained_peek(Relation index)
{
	Buffer		buf = sift_dir_pin(index);
	bool		chained;

	pg_read_barrier();
	chained = dir_is_chained(DIR(BufferGetPage(buf)));
	ReleaseBuffer(buf);
	return chained;
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

/* Validates the mutator's result (metapage and nsegs entries) inside the record; aborts it on failure */
static void
check_dir_result(Relation index, SiftXlog *state, const SiftMetaPageData *before, const SiftMetaPageData *meta,
				 const SiftSegEntry *segs, int nsegs, int limit)
{
	const char *err = NULL;
	SiftU64Set	seen;

	if (meta->magic != before->magic || meta->high_water != before->high_water ||
		meta->freelist_head != before->freelist_head || meta->next_gen < before->next_gen)
		err = "allocation state changed";
	else if (nsegs > limit)
	{
		sift_xlog_abort(state);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" cannot hold more than %d segments", RelationGetRelationName(index), limit),
				 errhint("Run sift.merge() on the index.")));
	}
	sift_u64set_init(&seen, nsegs);
	for (int i = 0; !err && i < nsegs; i++)
	{
		const SiftSegEntry *s = &segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_VALRUN)
			err = "invalid segment kind";
		else if (s->gen == 0 || s->gen >= meta->next_gen)
			err = "segment generation was not reserved";
		else if (s->format > Max(Max(SIFT_FORMAT_MINOR, SIFT_COL_FORMAT_MAX_READ), SIFT_COL_FORMAT_WIDE_ROWS))	/* the column engine numbers its segment formats on its own */
			err = "unknown segment format";
		else if (!sift_u64set_add(&seen, s->gen))
			err = "duplicate segment generation";
	}
	sift_u64set_free(&seen);
	if (err)
	{
		sift_xlog_abort(state);
		SIFT_INVARIANT(false, "sift: invalid directory update: %s", err);
	}
}

/* An update that works on the directory as a list: the entries the mutator edits, and what the chain it replaces holds */
typedef struct DirEdit
{
	SiftDirParts parts;			/* the partition each entry is in (core/dirpart.c) */
	SiftDirectoryData *dir;		/* the mutator's copy of block 1, with room for the entries a mutator adds (SiftDirChange.nadd) */
	int			cap;
	SiftSegEntry *old;			/* its entries as they were */
	int			nold;
	bool		chained;
	SiftDirChain chain;			/* the chain they came from, when chained */
	SiftExtent *release;		/* the release list of the caller and the blocks of the old chain that the new one does not keep */
	int			nrelease;
} DirEdit;

static void
dir_edit_open(Relation index, Page dp, const SiftDirChange *chg, DirEdit *e)
{
	SiftDirectoryData *page = DIR(dp);
	SiftSegEntry *old;

	e->chained = dir_is_chained(page);
	if (e->chained)
		e->nold = sift_dirchain_load(index, SIFT_DIR_ROOT(page), page->version, &old, &e->chain);
	else
	{
		e->nold = page->nsegs;
		old = page->segs;
	}
	e->cap = e->nold + chg->nadd;
	e->dir = palloc(DIR_USED(e->cap));
	e->dir->version = page->version;
	e->dir->reserved = 0;
	memcpy(e->dir->segs, old, e->nold * sizeof(SiftSegEntry));
	e->dir->nsegs = e->nold;
	e->old = palloc(Max(e->nold, 1) * sizeof(SiftSegEntry));
	memcpy(e->old, old, e->nold * sizeof(SiftSegEntry));
	if (e->chained)
		sift_dirpart_open(&e->parts, &e->chain, e->old);
	sift_dirpart_enter(&e->parts);
}

static void
dir_edit_free(DirEdit *e)
{
	sift_dirpart_leave(&e->parts);
	pfree(e->dir);
	pfree(e->old);
	pfree(e->release);
	if (e->chain.blocks != NULL)
	{
		pfree(e->chain.blocks);
		pfree(e->chain.leaf);
		pfree(e->chain.inner);
	}
}

/*
 * The mutator is done: lays the entries out, in block 1 while they fit and the directory is not chained, else in a chain whose new pages are logged now
 * (before the record that refers to them), and plans the release of the old chain's blocks with the caller's. The chain's blocks come off the free list
 * and the end of the file, which logs the metapage on its own: the working copy of the record takes over those two fields afterwards.
 */
static void
dir_edit_close(Relation index, SiftXlog *state, Buffer metabuf, Page mp, Page dp, const SiftMetaPageData *before,
			   const SiftDirChange *chg, DirEdit *e, FullTransactionId safexid, FreePlan *plan)
{
	SiftDirectoryData *dir = e->dir;
	SiftDirectoryData *page = DIR(dp);
	uint64		version = page->version + 1;

	SiftDirPart *parts = NULL;
	int			nparts = e->parts.n > 0 ? sift_dirpart_arrange(index, &e->parts, dir->segs, dir->nsegs, &parts) : 0;

	check_dir_result(index, state, before, META(mp), dir->segs, dir->nsegs, SIFT_DIR_WIDE_MAX);
	e->release = palloc((chg->nrelease + e->chain.nleaves + e->chain.ninner + 2 + dir->nsegs) * sizeof(SiftExtent));
	if (chg->nrelease > 0)
		memcpy(e->release, chg->release, chg->nrelease * sizeof(SiftExtent));
	e->nrelease = chg->nrelease;
	if (!e->chained && nparts == 0 && dir->nsegs <= sift_dir_flat_max())
	{
		page->nsegs = dir->nsegs;
		memcpy(page->segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
		SiftPageSetUsed(dp, DIR_USED(page->nsegs));
	}
	else
	{
		BlockNumber root = sift_dirchain_write(index, metabuf, version, e->chained ? &e->chain : NULL, e->old, dir->segs, dir->nsegs, parts, nparts,
											   chg->chain_below != 0 ? chg->chain_below : InvalidBlockNumber, e->release, &e->nrelease);

		SIFT_DIR_TEST_CHAIN_POINT();
		page->nsegs = 0;
		page->reserved = SIFT_DIR_CHAINED;
		SIFT_DIR_ROOT(page) = root;
		SiftPageSetUsed(dp, DIR_CHAINED_USED);
		META(mp)->format_minor = Max(META(mp)->format_minor, SIFT_FORMAT_MINOR_DIRCHAIN);
	}
	page->version = version;
	if (e->nrelease > 0)
		prepare_free(index, metabuf, e->release, e->nrelease, safexid, plan);
	META(mp)->high_water = META(BufferGetPage(metabuf))->high_water;
	META(mp)->freelist_head = META(BufferGetPage(metabuf))->freelist_head;
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
	bool		pending = publish && !chg->immediate && !space_exclusive && (chg->nrelease > 0 || dir_chained_peek(index));
	FullTransactionId safexid = pending && xid_safexid() ?
		GetTopFullTransactionId() : InvalidFullTransactionId;
	Buffer		metabuf;
	Buffer		dirbuf;
	FreePlan	plan = {.flbuf = InvalidBuffer, .newhead = InvalidBlockNumber};
	SiftXlog	state;
	SiftMetaPageData before;
	Page		mp;
	Page		dp = NULL;
	Page		fp = NULL;
	Page		xp = NULL;
	DirEdit		edit = {0};

	/* a test lets a reader start between the xid and the locks: its snapshot is later than the xid and reads the directory as it is before this change */
	SIFT_TEST_GATE(pending && sift_test_release_gate, 6);
	SIFT_DIR_TEST_ENTER();
	metabuf = lock_meta(index, BUFFER_LOCK_EXCLUSIVE);
	dirbuf = publish ? lock_dir(index, BUFFER_LOCK_EXCLUSIVE) : InvalidBuffer;
	SIFT_DIR_TEST_POINT(false);
	/* a chain's old blocks wait for the horizon like the extents of a segment, whether or not the caller releases any */
	if ((pending || (publish && dir_is_chained(DIR(BufferGetPage(dirbuf))) && !chg->immediate && !space_exclusive)) &&
		!FullTransactionIdIsValid(safexid))
		safexid = ReadNextFullTransactionId();
	if (!publish && chg->nrelease > 0)
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
		dir_edit_open(index, dp, chg, &edit);
		if (chg->fn)
		{
			chg->fn(META(mp), edit.dir, xp, chg->arg);
			SIFT_INVARIANT(edit.dir->nsegs <= edit.cap, "sift: a directory mutator added %d entries, its bound (SiftDirChange.nadd) is %d", edit.dir->nsegs - edit.nold, chg->nadd);
		}
		dir_edit_close(index, &state, metabuf, mp, dp, &before, chg, &edit, safexid, &plan);
		if (BufferIsValid(plan.flbuf))
			fp = sift_xlog_register(&state, plan.flbuf, 0);
		/* the page ends where the base does until a failure note or a floor is written: pd_lower covers the bytes of a note once it has one */
		if (sift_meta_extended(META(mp)))
			SiftPageSetUsed(mp, sizeof(SiftMetaPageData));
		/* a build that reads less than the newest segment format of the index must refuse it; a column segment's format is its own (col/col.h), its unfrozen rows raise the minor */
		for (int i = 0; i < edit.dir->nsegs; i++)
			META(mp)->format_minor = Max(META(mp)->format_minor, seg_minor(index, &edit.dir->segs[i]));
	}
	apply_free(META(mp), fp, &plan);
	/* Do not accept cancellation between a durable physical mapping change and its immediate invalidation. */
	if (publish && space_exclusive && chg->nrelease > 0)
		HOLD_INTERRUPTS();
	sift_xlog_finish(&state);
	if (publish)
		dir_edit_free(&edit);
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
