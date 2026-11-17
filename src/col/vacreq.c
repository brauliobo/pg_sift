/*
 * col/vacreq.c - the heap VACUUM that churn and scans ask for (sift.col_auto_merge)
 *
 * A sift_col index answers a scan from its units only where the visibility map says that every row of the heap page is visible; a row on any
 * other page goes to the heap, 0.085 us each and a few us for every page. One UPDATE or DELETE per page of rows is enough to
 * turn that on for the whole table: 200,000 rows, 5% changed, a grouped sum 0.1 ms before and 17 ms after, 0.2 ms after the VACUUM that sets
 * the map again, and autovacuum comes for it after 20% of the rows changed.
 *
 * A backend that inserts a row version into the index (an INSERT, or an UPDATE) looks at the statistics of the table once per naptime, and so does
 * a scan that checked many rows in the heap: when the rows changed since the last VACUUM (dead, or new) are CHURN_PCT
 * percent of the table, a worker is started. It vacuums as autovacuum's does (cost delay, SKIP_LOCKED), at most once per five
 * naptimes, or ten times the average VACUUM of the table where PostgreSQL keeps it, and ends after it: a worker that has to wait does not hold its connection
 * (it would keep DROP DATABASE waiting and a slot of max_worker_processes), the next insert or scan that finds the table changed asks again. The VACUUM calls the index back: the dead rows are marked and the index rewritten.
 * A DELETE alone calls nothing and waits for a scan or the next insert.
 *
 * A table that was just loaded has no statistics to read and no scan yet: the transaction that builds the index (CREATE INDEX, REINDEX) over pages the map does not hold, or that writes
 * CHURN_PCT percent of the rows of the table (at least MIN_VETTED) to an index, asks for the worker when it commits, so that the first reads after a load do not
 * pay for the heap check of every row until the next scan asks for it, or autovacuum comes.
 */
#include "postgres.h"
#include "access/relation.h"
#include "access/xact.h"
#include "access/table.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "agg/stats.h"
#include "col/am.h"
#include "col/scope.h"
#include "core/eval.h"
#include "core/worker.h"
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
/* Pages the map does not hold from which a build asks, and the worker takes a table for unclean: a table of a few pages is read in the heap in less than the worker takes to start */
#define MIN_UNCLEAN		16
/* The least time between two VACUUMs of a table on this account, in naptimes, and in times the average one where it is known */
#define NAPTIMES		5
#define DURATIONS		10
/* The advisory lock (in the space of col/maint/maint.c's, 0x51F7) a worker holds for the table it is at */
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
/*
 * Does the map lack more than a hundredth of the pages of the table (MIN_UNCLEAN at least): what the worker takes for a table that is not clean when the statistics say it is. A VACUUM
 * resets the counts of dead and new rows to what it saw, so one that began before a transaction committed (the worker of another request, a scan's) wipes that transaction's rows
 * from them, and the table would be left with the pages that transaction wrote out of the map until the next change.
 */
static bool
heap_unclean(Oid heapoid)
{
	Relation	heap = try_table_open(heapoid, AccessShareLock);
	BlockNumber nblocks,
				allvisible;

	if (heap == NULL)
		return false;
	nblocks = RelationGetNumberOfBlocks(heap);
	visibilitymap_count(heap, &allvisible, NULL);
	table_close(heap, AccessShareLock);
	return nblocks - Min(allvisible, nblocks) >= Max(MIN_UNCLEAN, nblocks / 100);
}

static TableState
table_state(Oid heapoid, bool asked, bool worker)
{
	const PgStat_StatTabEntry *tab;
	TimestampTz last;
	int64		wait_ms = (int64) NAPTIMES * sift_maintenance_naptime;
	int64		changed,
				threshold;

	/* the look is once per naptime: a statement that runs for longer keeps its snapshot of the statistics, and would ask for a VACUUM the worker finds done */
	pgstat_clear_snapshot();
	tab = pgstat_fetch_stat_tabentry(heapoid);
	if (tab == NULL)
		return asked || (worker && heap_unclean(heapoid)) ? TABLE_DUE : TABLE_CLEAN;
	churn_of(tab, &changed, &threshold);
	if (!asked && changed < threshold && !(worker && heap_unclean(heapoid)))
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

static Oid	vacuum_heap_of(Oid indexoid);

/* The reloptions of another table AM are not StdRdOptions, and a sift_table has its own pass */
static bool
heap_vacuumable(Relation heap)
{
	return heap->rd_rel->relam == HEAP_TABLE_AM_OID && heap->rd_rel->relkind == RELKIND_RELATION && sift_col_autovacuum_enabled(heap);
}

/*
 * What a read pays for a row that maintenance would spare it, in instructions (records on 300,000 rows, 5% of them updated in a covered column, the heap table with its index, no VACUUM since):
 * a row checked in the heap 250 (the aggregate scan 120 to 250, an index-only scan 600), a row decided by the tombstone log nothing measurable (the same reads cost the same with track_changes on), a row of a unit read as a unit because it holds a dead row
 * 30 (a point read of 60 keys: 0.90 M instructions before the VACUUM, 1.75 M after it, until the rewrite). The scans report the rows, the tick keeps the sum for the index in this backend
 * (sift.stats read_penalty_kinstr).
 */
#define PEN_VET_ROW		250
#define PEN_DIRTY_ROW	30

/*
 * The price of the VACUUM a read would ask for, in instructions: 340 for each row of the table (its index and its map, whatever the churn), 41,000 for each page that is not all-visible,
 * and the WAL of the page, 10.5 kB measured (5% of the rows updated, 2,943 pages: 31 MB; clustered, 281 pages: 2.8 MB) at 0.6 G instructions a MB. A maintenance step counts at half (the weighted formula), so the worker is asked when the reads owe half of this.
 */
#define VAC_ROW			340
#define VAC_PAGE		41000
#define VAC_WAL_PAGE	6.3e6

/* unclean: false when every page is all-visible: a VACUUM spares no read anything then (the rows of the write buffer are checked in the heap whatever the map says, the merge spares those) */
static double
vacuum_cost(Oid heapoid, bool *unclean)
{
	Relation	heap = table_open(heapoid, AccessShareLock);
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap),
				allvisible;
	double		rows = Max(heap->rd_rel->reltuples, 0);

	visibilitymap_count(heap, &allvisible, NULL);
	table_close(heap, AccessShareLock);
	allvisible = Min(allvisible, nblocks);
	*unclean = nblocks > allvisible;
	return rows * VAC_ROW + (double) (nblocks - allvisible) * (VAC_PAGE + VAC_WAL_PAGE);
}

/* What a backend keeps of its looks at an index: when it looked last, and what its reads owe since */
typedef struct Look
{
	Oid			index;
	TimestampTz at;
	double		owed_vacuum;
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

/*
 * A backend looks at the table of the index: once per naptime for each index, whether it inserted a row version (everything 0) or read the rows of the
 * index in the heap (vetted, which must be many to be worth a look).
 *
 * With the method penalty_maintenance (sift.eval_methods, core/eval.h) the VACUUM is asked for the same way: when the rows the reads checked in the heap come to half of what that VACUUM costs, and not when a fixed share of the rows has changed or one scan checked a thousand rows.
 * The worker vacuums once for the request. A table that churns and is not read is left to autovacuum; the reads stay correct either way.
 */
void
sift_col_vacuum_tick(Oid indexoid, Oid heapoid, int64 vetted, int64 dirty_rows)
{
	Look	   *lk;
	bool		priced = sift_eval_on(SIFT_EVAL_PENALTY_MAINTENANCE);
	const char *worker = NULL;
	TimestampTz now;
	Relation	index;
	bool		small,
				decoded,
				marked;

	/* counted whether or not maintenance may run: this is how a stopped maintenance shows */
	sift_stat_add(SIFT_STAT_ROWS_DIRTY, dirty_rows);
	sift_stat_add(SIFT_STAT_READ_PENALTY_KINSTR, (vetted * PEN_VET_ROW + dirty_rows * PEN_DIRTY_ROW) / 1000);
	if (sift_reads_pending(indexoid))
		flush_reads(indexoid);
	if (!sift_col_auto_merge || !sift_async_maintenance || !IsUnderPostmaster || IsParallelWorker() || RecoveryInProgress())
		return;
	if (sift_scope_paid_pending(indexoid))
	{
		Relation	scoped = index_open(indexoid, AccessShareLock);

		sift_scope_paid_flush(scoped);
		index_close(scoped, AccessShareLock);
	}
	lk = look_of(indexoid);
	lk->owed_vacuum += (double) vetted * PEN_VET_ROW;
	/* a scan that checked a few rows in the heap saved nothing worth a VACUUM, but if it decoded the write buffer the reads may have paid for its promotion */
	decoded = sift_cwbuf_decoded_take();
	marked = sift_col_marks_take();
	small = !priced && vetted > 0 && vetted < MIN_VETTED;
	if (small && !decoded)
		return;
	now = GetCurrentTimestamp();
	if (now - lk->at < (int64) sift_maintenance_naptime * 1000)
		return;
	lk->at = now;
	index = index_open(indexoid, AccessShareLock);
	if (small)
	{
		if (sift_col_promote_tick(index))
			sift_stat_add(SIFT_STAT_MAINT_ASKS, 1);
		index_close(index, AccessShareLock);
		return;
	}
	if (priced)
	{
		bool		unclean;

		if (lk->owed_vacuum > 0 && OidIsValid(vacuum_heap_of(indexoid)))
		{
			double		cost = vacuum_cost(heapoid, &unclean);

			if (!unclean)
				lk->owed_vacuum = 0;
			else if (lk->owed_vacuum >= MAINT_HALF * cost && table_state(heapoid, true, false) == TABLE_DUE)
				worker = "sift_col_vacuum_once_worker_main";
		}
	}
	else if (table_state(heapoid, vetted > 0, false) == TABLE_DUE)
		worker = vetted > 0 ? "sift_col_vacuum_scan_worker_main" : "sift_col_vacuum_worker_main";
	if (worker != NULL && !worker_at(heapoid))
	{
		if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
			sift_maint_register_worker(index, worker);
		sift_stat_add(SIFT_STAT_MAINT_ASKS, 1);
		lk->owed_vacuum = 0;
	}
	else if (sift_col_promote_tick(index) || (marked && sift_col_fold_tick(index)))
	{
		sift_stat_add(SIFT_STAT_MAINT_ASKS, 1);
	}
	index_close(index, AccessShareLock);
}

/* The reloptions of a heap table are StdRdOptions */
bool
sift_col_autovacuum_enabled(Relation heap)
{
	return heap->rd_options == NULL || ((StdRdOptions *) heap->rd_options)->autovacuum.enabled;
}

/*
 * The VACUUMs a transaction asks for, started when it commits (the index and the rows are not there for a worker before): one per index it built, or wrote CHURN_PCT percent of the table to.
 * The first rows of a table, and the pages that no VACUUM has marked, are what the reads of a table that was just loaded pay for.
 */
#define ASKS			4

typedef struct VacAsk
{
	Oid			index;
	Oid			heap;
	int64		rows;
	int64		threshold;
	bool		due;
	char		subject[2 * NAMEDATALEN];	/* the name of the index, for its worker: a commit callback reads no catalog */
} VacAsk;

static VacAsk asks[ASKS];
static int	nasks;
static bool asks_hooked;

static void
asks_xact(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	for (int i = 0; i < nasks; i++)
	{
		bool		again = false;

		/* one worker for a table whatever the indexes that asked */
		for (int j = 0; j < i; j++)
			again |= asks[j].due && asks[j].heap == asks[i].heap;
		if (event == XACT_EVENT_COMMIT && asks[i].due && !again)
			(void) sift_maint_register_worker_oid(asks[i].index, "sift_col_vacuum_scan_worker_main", asks[i].subject);
	}
	nasks = 0;
}

static bool
asking(Relation index)
{
	return sift_col_auto_merge && sift_async_maintenance && IsUnderPostmaster && !IsParallelWorker() && !RecoveryInProgress() &&
		index->rd_rel->relpersistence != RELPERSISTENCE_TEMP;
}

static VacAsk *
ask_for(Relation index, Relation heap)
{
	VacAsk	   *a;

	for (int i = 0; i < nasks; i++)
		if (asks[i].index == RelationGetRelid(index))
			return &asks[i];
	if (nasks == ASKS)
		return NULL;
	if (!asks_hooked)
	{
		RegisterXactCallback(asks_xact, NULL);
		asks_hooked = true;
	}
	a = &asks[nasks++];
	a->index = RelationGetRelid(index);
	a->heap = RelationGetRelid(heap);
	a->rows = 0;
	a->threshold = Max(MIN_VETTED, Min(CHURN_PCT / 100.0 * Max(heap->rd_rel->reltuples, 0), CHURN_ROWS));
	a->due = false;
	return a;
}

static void
ask_due(VacAsk *a, Relation index)
{
	a->due = true;
	strlcpy(a->subject, sift_worker_subject(index), sizeof(a->subject));
}

/* A row version written to the index (its INSERT or UPDATE): the transaction that writes many asks for the VACUUM at its commit */
void
sift_col_vacuum_note_insert(Relation index, Relation heap)
{
	VacAsk	   *a;

	if (!asking(index) || (a = ask_for(index, heap)) == NULL || a->due)
		return;
	if (++a->rows >= a->threshold && heap_vacuumable(heap))
		ask_due(a, index);
}

/* A build over a table whose map holds few pages asks for the VACUUM that would make the first reads of the index cheap */
static void
ask_build(Relation heap, Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap),
				allvisible;
	VacAsk	   *a;

	if (!asking(index) || !heap_vacuumable(heap))
		return;
	visibilitymap_count(heap, &allvisible, NULL);
	if (nblocks < Min(allvisible, nblocks) + MIN_UNCLEAN || (a = ask_for(index, heap)) == NULL)
		return;
	ask_due(a, index);
}

/*
 * CREATE INDEX and REINDEX ask for the VACUUM of a table the map does not hold, and say that sift.col_auto_merge does nothing for a table that opted out of autovacuum: with
 * autovacuum on, the default needs no word
 */
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
	ask_build(heap, index);
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
	if (!heap_vacuumable(heap))
		heapoid = InvalidOid;
	relation_close(heap, AccessShareLock);
	return heapoid;
}

/*
 * What autovacuum asks of a table: the options of its reloptions (vacuum_index_cleanup and vacuum_truncate unspecified), so that PostgreSQL skips the
 * indexes when few pages hold a dead row. vacuum() ends the transaction it is called in and starts the next, as the VACUUM command does for the one of
 * its backend, so what it is given lives in a context of its own, as autovacuum's does.
 */
/* The context of the VACUUM at work: a worker that is ended in the middle of it (the server stops) leaves it for the exit to free */
static MemoryContext vacuum_cx;

static void
vacuum_cx_free(int code, Datum arg)
{
	if (vacuum_cx != NULL)
		MemoryContextDelete(vacuum_cx);
	vacuum_cx = NULL;
}

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
	/*
	 * With synchronous_commit off the commit records of the churn are not on disk yet, and PostgreSQL sets no hint bit for a transaction whose commit
	 * record is not flushed: the pages that hold only its new row versions stay out of the visibility map, and the index checks their rows in the heap
	 * until the next VACUUM (a fifth of the pages after 3% of the rows changed). The flush costs the WAL writer's next one.
	 */
	sift_wal_flush_inserted();
	vacuum_cx = cx;
	vacuum(rels, &params, strategy, cx, true);
	FreeAccessStrategy(strategy);
	vacuum_cx = NULL;
	MemoryContextDelete(cx);
}

/* A VACUUM of the table ended well: the index the worker was asked for says so (sift.workers) */
static void
vacuum_ok(Oid indexoid, TimestampTz start)
{
	Relation	index = sift_try_open_index(indexoid, RowExclusiveLock);

	if (index == NULL)
		return;
	sift_worker_ok(index, SIFT_JOB_VACUUM, start);
	index_close(index, RowExclusiveLock);
}

/* The one VACUUM of the worker, when the table is due for it (the worker leaves otherwise: it does not wait for the table to change again) */
static void
vacuum_once(Oid indexoid, bool asked)
{
	Oid			heapoid;
	LOCKTAG		tag;
	TimestampTz start = GetCurrentTimestamp();

	StartTransactionCommand();
	heapoid = vacuum_heap_of(indexoid);
	CommitTransactionCommand();
	table_lock_tag(&tag, heapoid);
	if (!OidIsValid(heapoid) || LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
		return;
	before_shmem_exit(vacuum_cx_free, (Datum) 0);
	SetConfigOption("vacuum_cost_delay", psprintf("%d", Max(sift_maintenance_cost_delay, 2)), PGC_SUSET, PGC_S_OVERRIDE);
	/* vacuum() ends the transaction it is called in and starts the next */
	StartTransactionCommand();
	if (!RecoveryInProgress() && table_state(heapoid, asked, true) == TABLE_DUE)
	{
		SIFT_TEST_WORKER_FAIL(SIFT_JOB_VACUUM);
		VacuumUpdateCosts();
		vacuum_heap(heapoid);
		vacuum_ok(indexoid, start);
	}
	CommitTransactionCommand();
}

/* asked: a scan started the worker, which takes its look as a scan would (table_state). A failure is recorded for the index like any worker's */
static void
vacuum_worker(Datum main_arg, bool asked)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;

	PG_TRY();
	{
		vacuum_once(indexoid, asked);
	}
	PG_CATCH();
	{
		sift_maint_worker_failed("sift_col", indexoid, sift_worker_target(), cx);
	}
	PG_END_TRY();
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

/* The VACUUM a request of penalty_maintenance asks for: one, then the worker leaves */
void
sift_col_vacuum_once_worker_main(Datum main_arg)
{
	vacuum_worker(main_arg, true);
}
