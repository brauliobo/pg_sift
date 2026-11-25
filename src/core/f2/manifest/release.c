/*
 * core/f2/manifest/release.c - who may still read the blocks a commit of the manifest releases.
 */
#include "core/host.h"

#include "access/xact.h"
#include "storage/proc.h"

#include "core/f2/manifest/int.h"

/*
 * Whether the blocks a commit releases wait for the xid of its own transaction. A VACUUM's xid is not in the snapshots of other backends (PROC_IN_VACUUM), and a worker of a parallel
 * query or a maintenance pass has none worth taking (the xid is burned for it): those wait for the next xid as of the moment block 0 is locked, which every snapshot that read the
 * old manifest precedes. An index or table no one else can read has no reader of its own releases: one the transaction created, or an index the build of a concurrent CREATE INDEX has not made ready.
 */
bool
sift_manifest_unshared(Relation rel)
{
	return rel->rd_createSubid != InvalidSubTransactionId || (rel->rd_index != NULL && !rel->rd_index->indisready);
}

bool
sift_manifest_waits_own_xid(Relation rel, int nrel)
{
	return nrel > 0 && !sift_manifest_unshared(rel) && (MyProc->statusFlags & PROC_IN_VACUUM) == 0 && !IsInParallelMode() &&
		FullTransactionIdIsValid(GetTopFullTransactionIdIfAny());
}
