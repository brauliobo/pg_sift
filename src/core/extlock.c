/*
 * core/extlock.c - the relation extension lock: see extlock.h
 */
#include "postgres.h"

#include "storage/lmgr.h"

#include "core/errctx.h"
#include "core/extlock.h"
#include "core/sift.h"

static void
warm_reports(Relation rel)
{
	(void) sift_catalog();
	sift_errctx_warm();
	pfree(sift_errctx_name(RelationGetRelid(rel)));
}

void
sift_extension_lock(Relation rel)
{
	warm_reports(rel);
	LockRelationForExtension(rel, ExclusiveLock);
}

void
sift_extension_unlock(Relation rel)
{
	UnlockRelationForExtension(rel, ExclusiveLock);
}

Buffer
sift_extend_zeroed_to(Relation rel, BlockNumber nblocks)
{
	warm_reports(rel);
	return ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, nblocks, RBM_ZERO_AND_LOCK);
}
