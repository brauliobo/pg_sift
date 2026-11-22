/*
 * build_ext_gate_pg.c - the two ways f2_build_par.c extends the index file under SiftBuildExtGate (spec 25 2.2): through smgr
 * for a CREATE INDEX that writes a new relfilenode with bulk writes, and through the buffer manager for the first pass of
 * CREATE INDEX CONCURRENTLY, where a checkpoint can run while the workers write. Server build only.
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"

#include "col/f2/build_ext_gate.h"
#include "core/extlock.h"

void
sift_build_extend_smgr(void *ctx, BlockNumber from, BlockNumber to)
{
	smgrzeroextend(RelationGetSmgr((Relation) ctx), MAIN_FORKNUM, from, (int) (to - from), false);
}

void
sift_build_extend_buffered(void *ctx, BlockNumber from, BlockNumber to)
{
	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKWRITE);

	UnlockReleaseBuffer(sift_extend_zeroed_to_bas((Relation) ctx, to, bas));
	FreeAccessStrategy(bas);
}
