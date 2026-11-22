/*
 * col/v1/maint/tick.c - what a backend does when its statement has touched a sift_col index: write the reads it noted and, once per naptime,
 * ask the maintenance worker for the promotion or the fold of the log that the reads or the writes have paid for
 *
 * The heap VACUUM is PostgreSQL's: sift_col never starts one (ALTER TABLE .. SET (autovacuum_vacuum_scale_factor = 0.01) keeps the visibility map
 * of a table that is read through a sift_col index close to all-visible).
 */
#include "postgres.h"
#include "access/genam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "agg/stats.h"
#include "col/am.h"
#include "col/scope.h"
#include "col/v1/wbuf.h"
#include "core/worker.h"

typedef struct Look
{
	Oid			index;
	TimestampTz at;
} Look;

#define LOOKS	4

/* The look state of an index: a statement that reads two (the facts and the journal of their pairs) comes by each of them in turn, so one slot would be reset by the other; the oldest slot makes room */
static Look *
look_of(Oid indexoid)
{
	static Look looks[LOOKS];
	Look	   *oldest = &looks[0];

	for (int i = 0; i < LOOKS; i++)
	{
		if (looks[i].index == indexoid)
			return &looks[i];
		if (looks[i].at < oldest->at)
			oldest = &looks[i];
	}
	*oldest = (Look) {.index = indexoid};
	return oldest;
}

static void
flush_reads(Oid indexoid)
{
	Relation	index = index_open(indexoid, AccessShareLock);

	sift_col_reads_flush(index);
	index_close(index, AccessShareLock);
}

/* A backend looks at the index once per naptime, whether it wrote a row version (dirty_rows 0) or read the rows of the index */
void
sift_col_maint_tick(Oid indexoid, int64 dirty_rows)
{
	Look	   *lk;
	TimestampTz now;
	Relation	index;
	bool		marked;

	sift_stat_add(SIFT_STAT_ROWS_DIRTY, dirty_rows);
	if (sift_reads_pending(indexoid))
		flush_reads(indexoid);
	if (!IsUnderPostmaster || IsParallelWorker() || RecoveryInProgress())
		return;
	if (sift_scope_paid_pending(indexoid))
	{
		Relation	scoped = index_open(indexoid, AccessShareLock);

		sift_scope_paid_flush(scoped);
		index_close(scoped, AccessShareLock);
	}
	if (!sift_async_maintenance)
		return;
	marked = sift_col_marks_take();
	lk = look_of(indexoid);
	now = GetCurrentTimestamp();
	if (now - lk->at < (int64) sift_maintenance_naptime * 1000)
		return;
	lk->at = now;
	index = index_open(indexoid, AccessShareLock);
	if (sift_col_promote_tick(index) || (marked && sift_col_fold_tick(index)))
		sift_stat_add(SIFT_STAT_MAINT_ASKS, 1);
	index_close(index, AccessShareLock);
}
