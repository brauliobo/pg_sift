/*
 * core/extlock.c - the relation extension lock: see extlock.h
 */
#include "postgres.h"

#include "storage/lmgr.h"
#include "storage/lock.h"

#include "core/errctx.h"
#include "core/extlock.h"
#include "core/f2/meta.h"
#include "core/sift.h"
#include "core/store.h"

static const char extend_owner;
static LockRelId last_locked;

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

	sift_errctx_freeze(&extend_owner);
	buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, bas, 0, nblocks, RBM_ZERO_AND_LOCK);
	sift_errctx_thaw(&extend_owner);
	return buf;
}

Buffer
sift_extend_zeroed_to(Relation rel, BlockNumber nblocks)
{
	return sift_extend_zeroed_to_bas(rel, nblocks, NULL);
}

struct SiftBulkRun
{
	Relation	rel;
	BulkWriteState *state;
	BlockNumber eof;
};

SiftBulkRun *
sift_bulk_run_begin(Relation rel)
{
	SiftBulkRun *run;

	if (RelationUsesLocalBuffers(rel))
		return NULL;
	run = palloc(sizeof(SiftBulkRun));
	run->rel = rel;
	sift_extension_lock(rel);
	run->eof = RelationGetNumberOfBlocks(rel);
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
	smgr_bulk_write(run->state, blk, (BulkWriteBuffer) page, false);
}

void
sift_bulk_run_end(SiftBulkRun *run)
{
	smgr_bulk_finish(run->state);
	sift_extension_unlock(run->rel);
	pfree(run);
}
