/*
 * test/unit/table/f2/mvcc_any.c - what a snapshot that takes any row (CLUSTER, VACUUM FULL, CREATE INDEX) and a dirty one see of a sift_table (table/f2/mvcc.c): the rows and column deltas
 * of committed writers ask the transaction log alone, never the process array (every TransactionIdIsInProgress takes ProcArrayLock and walks every backend), and the rows of a writer
 * that rolled back are gone while those of one still running are there.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "table/f2/mvcc.c"

#define WRITERS 4
#define RUNNING_XID 1100
#define ABORTED_XID 1101

static int	in_progress_calls;

bool
TransactionIdIsInProgress(TransactionId xid)
{
	in_progress_calls++;
	return xid == RUNNING_XID;
}

bool
TransactionIdDidCommit(TransactionId xid)
{
	return xid != RUNNING_XID && xid != ABORTED_XID;
}

bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	return false;
}

static void
check_snapshot(SnapshotType type, const char *what)
{
	static SiftRowVer vers[SIFT_GRANULE_ROWS];
	SnapshotData snap = {.snapshot_type = type};
	SiftTblVis	v = {.snap = &snap};
	SiftF2GranuleBuf gb = {.nrows = SIFT_GRANULE_ROWS,.vers = vers};
	uint64		sel[SIFT_DV_MASK_WORDS];
	bool		deltas = true;

	for (uint32 r = 0; r < SIFT_GRANULE_ROWS; r++)
		vers[r] = (SiftRowVer) {.xmin = 1000 + r % WRITERS};
	in_progress_calls = 0;
	if (type == SNAPSHOT_ANY)
		SIFT_CHECKF(sift_tbl_f2_vis_granule(&v, &gb, NULL, sel) == SIFT_GRANULE_ROWS, "%s: a row of a committed writer is not seen", what);
	for (uint32 r = 0; r < SIFT_GRANULE_ROWS; r++)
		deltas &= sift_tbl_f2_delta_visible(&v, vers[r].xmin, 0);
	SIFT_CHECKF(deltas, "%s: a column delta of a committed writer is not seen", what);
	SIFT_CHECKF(in_progress_calls == 0, "%s: a granule of %d committed writers asked the process array %d times", what, WRITERS, in_progress_calls);
	SIFT_CHECKF(!sift_tbl_f2_delta_visible(&v, RUNNING_XID, 0) && !sift_tbl_f2_delta_visible(&v, ABORTED_XID, 0), "%s: a column delta of a writer that did not commit is seen", what);
	if (type != SNAPSHOT_ANY)
		return;
	vers[0].xmin = RUNNING_XID;
	vers[1].xmin = ABORTED_XID;
	SIFT_CHECKF(sift_tbl_f2_vis_granule(&v, &gb, NULL, sel) == SIFT_GRANULE_ROWS - 1 && (sel[0] & 3) == 1, "%s: the row of a running writer is gone or the aborted one is there", what);
}

int
main(void)
{
	check_snapshot(SNAPSHOT_ANY, "any");
	check_snapshot(SNAPSHOT_DIRTY, "dirty");
	return sift_check_summary();
}
