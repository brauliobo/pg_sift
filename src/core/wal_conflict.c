/*
 * core/wal_conflict.c - the recovery conflict of a change a standby's snapshots could not tell from the primary's (core/wal_conflict.h).
 */
#include "core/host.h"

#include "access/nbtxlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"

#include "core/page_io.h"
#include "core/wal_conflict.h"

bool		sift_standby_conflicts = true;

bool
sift_wal_snapshot_conflict(Relation index, BlockNumber blk, FullTransactionId horizon)
{
	xl_btree_reuse_page xlrec;

	if (!sift_standby_conflicts || !FullTransactionIdIsValid(horizon) || !XLogStandbyInfoActive() || !RelationNeedsWAL(index))
		return false;
	xlrec = (xl_btree_reuse_page) {.locator = index->rd_locator,.block = blk,.snapshotConflictHorizon = horizon};
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfBtreeReusePage);
	XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE);
	return true;
}
