/*
 * core/extlock.c - the relation extension lock: see extlock.h
 */
#include "postgres.h"

#include "storage/lmgr.h"

#include "core/errctx.h"
#include "core/extlock.h"
#include "core/sift.h"

static const char extend_owner;

void
sift_extension_lock(Relation rel)
{
	sift_errctx_freeze(rel);
	(void) sift_catalog();
	LockRelationForExtension(rel, ExclusiveLock);
}

void
sift_extension_unlock(Relation rel)
{
	UnlockRelationForExtension(rel, ExclusiveLock);
	sift_errctx_thaw(rel);
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
