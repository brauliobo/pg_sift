/*
 * core/extlock.c - the relation extension lock: see extlock.h
 */
#include "postgres.h"

#include "storage/lmgr.h"
#include "storage/lock.h"

#include "core/errctx.h"
#include "core/extlock.h"
#include "core/f2/meta.h"
#include "core/maint/maint.h"
#include "core/maint/pacer.h"
#include "core/sift.h"
#include "core/store/store.h"

static const char extend_owner;
static LockRelId last_locked;

#ifdef SIFT_TESTING
int			sift_test_disk_blocks = 0;

/* A full disk: the file of a relation of sift may not grow past sift.test_disk_blocks blocks */
static void
test_disk_full(BlockNumber nblocks)
{
	if (sift_test_disk_blocks > 0 && nblocks > (BlockNumber) sift_test_disk_blocks)
		ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("sift test: no space left on device")));	/* check-errors: allow */
}
#else
#define test_disk_full(nblocks) ((void) 0)
#endif

void
sift_extension_lock(Relation rel)
{
	sift_errctx_freeze(rel);
	(void) sift_catalog();
	LockRelationForExtension(rel, ExclusiveLock);
	last_locked = rel->rd_lockInfo.lockRelId;
}

bool
sift_extension_held(void)
{
	LOCKTAG		tag;

	if (!OidIsValid(last_locked.relId))
		return false;
	SET_LOCKTAG_RELATION_EXTEND(tag, last_locked.dbId, last_locked.relId);
	return LockHeldByMe(&tag, ExclusiveLock, false);
}

void
sift_extension_unlock(Relation rel)
{
	UnlockRelationForExtension(rel, ExclusiveLock);
	sift_errctx_thaw(rel);
	SIFT_DIR_TEST_UNLOCKED();
}

Buffer
sift_extend_zeroed_to_bas(Relation rel, BlockNumber nblocks, BufferAccessStrategy bas)
{
	Buffer		buf;

	test_disk_full(nblocks);
	sift_errctx_freeze(&extend_owner);
	buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, bas, 0, nblocks, RBM_ZERO_AND_LOCK);
	sift_errctx_thaw(&extend_owner);
	return buf;
}

Buffer
sift_extend_zeroed_to(Relation rel, BlockNumber nblocks)
{
	return sift_extend_zeroed_to_bas(rel, nblocks, sift_maint_ring);
}

struct SiftBulkRun
{
	Relation	rel;
	BulkWriteState *state;
	BlockNumber eof;
	bool		written;
};

/* The file made long enough for the blocks of the extents, the lock held for that alone: the length it had */
static BlockNumber
extend_file_to(Relation rel, BlockNumber nblocks)
{
	SMgrRelation smgr = RelationGetSmgr(rel);
	BlockNumber eof;

	test_disk_full(nblocks);
	sift_extension_lock(rel);
	eof = smgrnblocks(smgr, MAIN_FORKNUM);
	if (eof < nblocks)
		smgrzeroextend(smgr, MAIN_FORKNUM, eof, nblocks - eof, false);
	sift_extension_unlock(rel);
	return eof;
}

SiftBulkRun *
sift_bulk_run_begin(Relation rel, const SiftExtent *ext, int next)
{
	SiftBulkRun *run;
	BlockNumber end = 0;

	if (RelationUsesLocalBuffers(rel) || !sift_maint_file_is_new(rel))
		return NULL;
	for (int e = 0; e < next; e++)
		end = Max(end, ext[e].start + ext[e].nblocks);
	run = palloc(sizeof(SiftBulkRun));
	run->rel = rel;
	run->eof = extend_file_to(rel, end);
	run->state = smgr_bulk_start_rel(rel, MAIN_FORKNUM);
	return run;
}

bool
sift_bulk_run_fresh(const SiftBulkRun *run, BlockNumber blk)
{
	return blk >= run->eof;
}

Page
sift_bulk_run_page(SiftBulkRun *run)
{
	BulkWriteBuffer buf = smgr_bulk_get_buf(run->state);

	memset(buf, 0, BLCKSZ);
	return (Page) buf;
}

void
sift_bulk_run_write(SiftBulkRun *run, BlockNumber blk, Page page)
{
	test_disk_full(blk + 1);
	/* STOR[STOR-WAL-01]: smgr_bulk_write page_std=true (omits 3-7 KB empty hole from WAL) */
	smgr_bulk_write(run->state, blk, (BulkWriteBuffer) page, true);
	run->written = true;
}

void
sift_bulk_run_abort(SiftBulkRun *run)
{
	if (run->written)
		smgr_bulk_finish(run->state);
	pfree(run);
}

void
sift_bulk_run_end(SiftBulkRun *run)
{
	smgr_bulk_finish(run->state);
	pfree(run);
}
