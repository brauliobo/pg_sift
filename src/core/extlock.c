/*
 * core/extlock.c - the relation extension lock: see extlock.h
 */
#include "postgres.h"

#include "storage/lmgr.h"
#include "storage/lock.h"

#include "core/errctx.h"
#include "core/extlock.h"
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
sift_extend_zeroed_to(Relation rel, BlockNumber nblocks)
{
	Buffer		buf;

	sift_errctx_freeze(&extend_owner);
	buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, nblocks, RBM_ZERO_AND_LOCK);
	sift_errctx_thaw(&extend_owner);
	return buf;
}
