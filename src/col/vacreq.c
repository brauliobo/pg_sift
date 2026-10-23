/*
 * col/vacreq.c - the heap VACUUM that churn and scans ask for (sift.col_auto_merge)
 *
 * A sift_col index answers a scan from its units only where the visibility map says that every row of the heap page is visible; a row on any
 * other page goes to the heap, 0.085 us each and a few us for every page (docs/COL.md 3). One UPDATE or DELETE per page of rows is enough to
 * turn that on for the whole table: 200,000 rows, 5% changed, a grouped sum 0.1 ms before and 17 ms after, 0.2 ms after the VACUUM that sets
 * the map again, and autovacuum comes for it after 20% of the rows changed.
 *
 * A backend that inserts a row version into the index (an INSERT, or an UPDATE) looks at the statistics of the table once per naptime, and so does
 * a scan that checked many rows in the heap: when the rows changed since the last VACUUM (dead, or new) are CHURN_PCT
 * percent of the table, a worker is started. It vacuums as autovacuum's does (cost delay, SKIP_LOCKED), at most once per five
 * naptimes, or ten times the average VACUUM of the table where PostgreSQL keeps it, and stays until the table is clean, so that the end of a burst of
 * changes is vacuumed too. The VACUUM calls the index back (docs/COL.md 2.6): the dead rows are marked and the index rewritten.
 * A DELETE alone calls nothing and waits for a scan or the next insert.
 */
#include "postgres.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
#include "col/am.h"
#include "bm25/maint.h"

/*
 * The rows of a table, dead or written since its last VACUUM, from which it is vacuumed: CHURN_PCT percent of it (a row changed per page of a thousand rows
 * is enough to send every row to the heap check), and never more than CHURN_ROWS, because a scan checks a row in the heap in 0.085 us: 100,000 stale rows
 * are 8.5 ms a query whatever the size of the table. autovacuum waits for 20% of it: 20 million rows of 100 million.
 */
#define CHURN_PCT		0.1
#define CHURN_ROWS		100000
/* A scan that checked fewer rows than this in the heap saved nothing worth a look */
#define MIN_VETTED		1000
/* Rows changed under which a table is clean whatever its size (autovacuum_vacuum_threshold) */
#define MIN_CHANGED		50
/* The least time between two VACUUMs of a table on this account, in naptimes, and in times the average one where it is known */
#define NAPTIMES		5
#define DURATIONS		10
/* A worker looks once per naptime, leaves after this many looks at a clean table, or this many in all */
#define CLEAN_LOOKS		3
#define MAX_LOOKS		60
/* The advisory lock (in the space of col/maint.c's, 0x51F7) a worker holds for the table it is at */
#define TABLE_LOCK_KEY	0x51F8

typedef enum TableState
{
	TABLE_CLEAN,				/* little changed, or not a table to vacuum */
	TABLE_WAIT,					/* changed, but it was vacuumed lately */
	TABLE_DUE
} TableState;

static void
table_lock_tag(LOCKTAG *tag, Oid heapoid)
{
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, TABLE_LOCK_KEY, heapoid, 3);
}

/* The rows changed since the last VACUUM (dead, or new), and the number from which the table is vacuumed */
static void
churn_of(const PgStat_StatTabEntry *tab, int64 *changed, int64 *threshold)
{
	*changed = (int64) Max(tab->dead_tuples, 0) + Max(tab->ins_since_vacuum, 0);
	*threshold = (int64) Max(MIN_CHANGED, Min(CHURN_PCT / 100.0 * (double) Max(tab->live_tuples, 0), CHURN_ROWS));
}

/*
 * What the statistics say of the table; they lag the changes by a second or so, and have no entry for a table nothing has touched. A scan that
 * checked many rows in the heap (asked) is the better witness: the table is due unless it was vacuumed lately.
 */
static TableState
table_state(Oid heapoid, bool asked)
{
	const PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(heapoid);
	TimestampTz last;
	int64		wait_ms = (int64) NAPTIMES * sift_maintenance_naptime;
	int64		changed,
				threshold;

	if (tab == NULL)
		return asked ? TABLE_DUE : TABLE_CLEAN;
	churn_of(tab, &changed, &threshold);
	if (!asked && changed < threshold)
		return TABLE_CLEAN;
#if PG_VERSION_NUM >= 180000
	if (tab->vacuum_count + tab->autovacuum_count > 0)
		wait_ms = Max(wait_ms, DURATIONS * (tab->total_vacuum_time + tab->total_autovacuum_time) / (tab->vacuum_count + tab->autovacuum_count));
#endif
	last = Max(tab->last_vacuum_time, tab->last_autovacuum_time);
	return last != 0 && GetCurrentTimestamp() - last < wait_ms * 1000 ? TABLE_WAIT : TABLE_DUE;
}

/* What the statistics of the table say (sift.col_maintenance_status): false when it has no entry */
bool
sift_col_table_churn(Oid heapoid, int64 *changed, int64 *threshold, TimestampTz *last)
{
	const PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(heapoid);

	if (tab == NULL)
		return false;
	churn_of(tab, changed, threshold);
	*last = Max(tab->last_vacuum_time, tab->last_autovacuum_time);
	return true;
}

/* Is a worker at the table (it holds the lock for as long as it lives)? */
static bool
worker_at(Oid heapoid)
{
	LOCKTAG		tag;

	table_lock_tag(&tag, heapoid);
	if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	LockRelease(&tag, ExclusiveLock, true);
	return false;
}

/*
 * A backend looks at the table of the index: once per naptime for each index, whether it inserted a row version (vetted 0) or read the rows of the
 * index in the heap (vetted, which must be many to be worth a look).
 */
void
sift_col_vacuum_tick(Oid indexoid, Oid heapoid, int64 vetted)
{
	static Oid	looked = InvalidOid;
	static TimestampTz looked_at = 0;
	TimestampTz now;
	Relation	index;

	if (!sift_col_auto_merge || (vetted > 0 && vetted < MIN_VETTED) || !sift_async_maintenance || !IsUnderPostmaster || IsParallelWorker() ||
		RecoveryInProgress())
		return;
	now = GetCurrentTimestamp();
	if (looked == indexoid && now - looked_at < (int64) sift_maintenance_naptime * 1000)
		return;
	looked = indexoid;
	looked_at = now;
	if (table_state(heapoid, vetted > 0) != TABLE_DUE || worker_at(heapoid))
		return;
	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		sift_maint_register_worker(index, vetted > 0 ? "sift_col_vacuum_scan_worker_main" : "sift_col_vacuum_worker_main");
	index_close(index, AccessShareLock);
}

/* The reloptions of a heap table are StdRdOptions */
bool
sift_col_autovacuum_enabled(Relation heap)
{
	return heap->rd_options == NULL || ((StdRdOptions *) heap->rd_options)->autovacuum.enabled;
}

/* CREATE INDEX and REINDEX say what sift.col_auto_merge does with the index, and that it does nothing for a table that opted out of autovacuum */
void
sift_col_vacuum_notice(Relation heap, Relation index)
{
	if (!sift_col_auto_merge || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (!sift_col_autovacuum_enabled(heap))
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": sift.col_auto_merge does not vacuum \"%s\", whose autovacuum_enabled is false",
						RelationGetRelationName(index), RelationGetRelationName(heap)),
				 errdetail("The units answer from their pre-aggregates again only after a VACUUM of the table, and the merge of the index follows that VACUUM."),
				 errhint("Run VACUUM on it from a job, or RESET autovacuum_enabled.")));
	else
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": sift.col_auto_merge keeps it answerable after churn", RelationGetRelationName(index)),
				 errdetail("A background worker vacuums \"%s\" when a thousandth of its rows changed and rewrites the index after the VACUUM, within sift.col_auto_merge_wal_pct of the WAL the server writes.",
						   RelationGetRelationName(heap)),
				 errhint("SET sift.col_auto_merge = off for the server or the database to stop it; sift.col_maintenance_status(index) shows where it stands.")));
}

/* The heap of the index when it is one to vacuum, else InvalidOid: not another table AM's, and not one that opted out of vacuum */
static Oid
vacuum_heap_of(Oid indexoid)
{
	Relation	index = try_index_open(indexoid, AccessShareLock);
	Oid			heapoid = InvalidOid;
	Relation	heap;

	if (index == NULL)
		return InvalidOid;
	if (sift_col_is_index(index))
		heapoid = index->rd_index->indrelid;
	index_close(index, AccessShareLock);
	if (!OidIsValid(heapoid) || (heap = try_relation_open(heapoid, AccessShareLock)) == NULL)
		return InvalidOid;
	/* the reloptions of another table AM are not StdRdOptions, and a sift_table has its own pass */
	if (heap->rd_rel->relam != HEAP_TABLE_AM_OID || heap->rd_rel->relkind != RELKIND_RELATION || !sift_col_autovacuum_enabled(heap))
		heapoid = InvalidOid;
	relation_close(heap, AccessShareLock);
	return heapoid;
}

/*
 * What autovacuum asks of a table: the options of its reloptions (vacuum_index_cleanup and vacuum_truncate unspecified), so that PostgreSQL skips the
 * indexes when few pages hold a dead row. vacuum() ends the transaction it is called in and starts the next, as the VACUUM command does for the one of
 * its backend, so what it is given lives in a context of its own, as autovacuum's does.
 */
static void
vacuum_heap(Oid heapoid)
{
	VacuumParams params = {0};
	MemoryContext cx = AllocSetContextCreate(TopMemoryContext, "sift_col vacuum", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_VACUUM);
	List	   *rels = list_make1(makeVacuumRelation(NULL, heapoid, NIL));

	MemoryContextSwitchTo(old);
	params.options = VACOPT_VACUUM | VACOPT_SKIP_LOCKED | VACOPT_PROCESS_MAIN | VACOPT_PROCESS_TOAST;
	params.freeze_min_age = params.freeze_table_age = params.multixact_freeze_min_age = params.multixact_freeze_table_age = -1;
#if PG_VERSION_NUM >= 190000
	params.log_vacuum_min_duration = -1;
#else
	params.log_min_duration = -1;
#endif
	params.nworkers = -1;
	vacuum(rels, &params, strategy, cx, true);
	FreeAccessStrategy(strategy);
	MemoryContextDelete(cx);
}

/* asked: a scan started the worker, which takes its first look as a scan would (table_state) */
static void
vacuum_worker(Datum main_arg, bool asked)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	Oid			heapoid;
	LOCKTAG		tag;
	int			clean = 0;

	StartTransactionCommand();
	heapoid = vacuum_heap_of(indexoid);
	CommitTransactionCommand();
	table_lock_tag(&tag, heapoid);
	if (!OidIsValid(heapoid) || LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
		return;
	SetConfigOption("vacuum_cost_delay", psprintf("%d", Max(sift_maintenance_cost_delay, 2)), PGC_SUSET, PGC_S_OVERRIDE);
	for (int looks = 0; looks < MAX_LOOKS && clean < CLEAN_LOOKS; looks++)
	{
		TableState	state;

		/* vacuum() ends the transaction it is called in and starts the next */
		StartTransactionCommand();
		state = RecoveryInProgress() ? TABLE_CLEAN : table_state(heapoid, asked && looks == 0);
		clean = state == TABLE_CLEAN ? clean + 1 : 0;
		if (state == TABLE_DUE)
		{
			VacuumUpdateCosts();
			vacuum_heap(heapoid);
		}
		CommitTransactionCommand();
		if (state != TABLE_DUE)
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, sift_maintenance_naptime, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

void
sift_col_vacuum_worker_main(Datum main_arg)
{
	vacuum_worker(main_arg, false);
}

void
sift_col_vacuum_scan_worker_main(Datum main_arg)
{
	vacuum_worker(main_arg, true);
}
