/*
 * vacuum_int.h - what the files of VACUUM, the maintenance passes and the rewrite (vacuum*.c) share: their types, the pass limits and the functions that cross them.
 */
#ifndef SIFT_TABLE_VACUUM_INT_H
#define SIFT_TABLE_VACUUM_INT_H

#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/multixact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"

#include "col/seg.h"
#include "core/lane.h"
#include "core/ops.h"
#include "bm25/maint.h"
#include "table/table.h"
#include "table/seg_int.h"
#include "core/cmp.h"

/* The cost delay of the pass (autovacuum's, or a worker's sift.table_auto_defrag_delay): a point where the page accesses so far may make it sleep */
#if PG_VERSION_NUM >= 180000
#define COST_DELAY()	vacuum_delay_point(false)
#else
#define COST_DELAY()	vacuum_delay_point()
#endif

/* A growable array of TIDs: the dead rows VACUUM found, the index entries CREATE INDEX CONCURRENTLY already has (sorted to look up) */
typedef struct TidList
{
	ItemPointerData *tid;
	uint64		n;
	uint64		cap;
} TidList;

typedef struct VacState
{
	const SiftTblRel *st;
	TransactionId oldest;
	TransactionId newest_kept;	/* oldest xid still stored: relfrozenxid */
	double		live;			/* as a heap counts them: in-progress insertions left out, in-progress deletions in */
	double		dead;			/* recently dead: deleter committed but not removable yet */
	double		group_dead;		/* of those, rows of the column groups (their liveness bit is still set) */
	TidList	dead_tids;
	TidList	redirects;			/* of the dead TIDs, the roots of HOT chains that go on: their index entries stay, the log keeps the way to the live version */
	SiftTblKeyRuns dead_runs;	/* sorted: the rows of the groups with unfrozen rows whose inserter aborted */
} VacState;

/* Closes the open write buffer segment once it is worth a column segment (pages) */
#define VACUUM_CLOSE_PAGES	8

/* The bytes a worker copies under AccessExclusiveLock at most: the window in which the table is closed to readers stays short */
#define DEFRAG_RELOCATE_BYTES	((uint64) 32 << 20)

extern int pass_max_active;
extern uint64 pass_wal_rate;
extern TimestampTz pass_start;
extern uint64 pass_wal_start;
extern bool maintaining;
extern uint64 space_cap;
extern uint64 merge_cap;
extern bool merge_capped;

extern ItemPointer tid_push(TidList *d);
extern bool tid_has(ItemPointer itemptr, void *state);
extern void relocate_page(Page page, const SiftExtent *old, const SiftExtent *fresh, int n);
extern int merge_steps(Relation rel, SiftTblRel *st, TransactionId oldest);
extern bool tbl_truncates(Relation rel);
extern int64 clear_removable(Relation rel, SiftTblRel *st, VacState *v);

#endif
