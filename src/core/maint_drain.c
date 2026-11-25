/*
 * core/maint_drain.c - see maint_drain.h. A worker stays attached to its database through its naps, and the servers before 19 end only the autovacuum workers of a database that is dropped or
 * copied, so the statement would fail with "being accessed by other users" for as long as a worker naps. The hook holds the database (the launcher starts no worker for it), ends the workers of sift
 * that are attached to it with SIGTERM and waits for them to leave, then runs the statement. DROP EXTENSION pg_sift drains the database the same way: a worker that napped on through it would
 * read the catalogs of an extension that is gone. The jobs queued for the database are forgotten once the statement ran (a database that is copied keeps them).
 */
#include "core/maint_sched.h"
#include "core/maint_drain.h"

#include <signal.h>

#include "catalog/namespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "tcop/utility.h"
#include "utils/backend_status.h"
#include "utils/timestamp.h"

#include "core/waits.h"

#define SIFT_EXTENSION	"pg_sift"
#define DRAIN_WAIT_US	(5 * USECS_PER_SEC)
#define DRAIN_POLL_MS	10

static ProcessUtility_hook_type prev_utility;

static bool
drops_sift(DropStmt *stmt)
{
	if (stmt->removeType == OBJECT_EXTENSION)
		foreach_node(String, name, stmt->objects)
			if (strcmp(strVal(name), SIFT_EXTENSION) == 0)
				return true;
	return false;
}

/* The schema of the extension holds the names that its functions and the library look up: it keeps its name */
static void
refuse_schema_rename(Node *stmt)
{
	RenameStmt *rename;
	Oid			ext,
				nsp;

	if (!IsA(stmt, RenameStmt) || (rename = castNode(RenameStmt, stmt))->renameType != OBJECT_SCHEMA)
		return;
	ext = get_extension_oid(SIFT_EXTENSION, true);
	nsp = get_namespace_oid(rename->subname, true);
	if (OidIsValid(ext) && OidIsValid(nsp) && get_extension_schema(ext) == nsp)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot rename schema \"%s\": it holds the objects of extension \"%s\"", rename->subname, SIFT_EXTENSION),
						errdetail("The functions and the library of the extension look their objects up by the name of the schema.")));
}

/* The database whose workers must leave before the statement runs: the one dropped, the one copied, the one that loses the extension */
static Oid
target_db(Node *stmt)
{
	const char *name = NULL;

	if (IsA(stmt, DropStmt))
		return drops_sift(castNode(DropStmt, stmt)) ? MyDatabaseId : InvalidOid;
	if (IsA(stmt, DropdbStmt))
		name = castNode(DropdbStmt, stmt)->dbname;
	else if (IsA(stmt, CreatedbStmt))
	{
		name = "template1";
		foreach_node(DefElem, opt, castNode(CreatedbStmt, stmt)->options)
			if (strcmp(opt->defname, "template") == 0 && opt->arg != NULL)
				name = defGetString(opt);
	}
	return name == NULL ? InvalidOid : get_database_oid(name, true);
}

/* The workers of sift that are attached to db now: each gets SIGTERM */
static int
terminate_workers(Oid db)
{
	int			n = 0;

	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *be = &pgstat_get_local_beentry_by_index(i)->backendStatus;
		const char *type;

		if (be->st_backendType != B_BG_WORKER || be->st_databaseid != db || (type = GetBackgroundWorkerTypeByPid(be->st_procpid)) == NULL || strncmp(type, "sift ", 5) != 0)
			continue;
		(void) kill(be->st_procpid, SIGTERM);
		n++;
	}
	return n;
}

static void
drain(SiftMaintShmem *m, Oid db)
{
	TimestampTz until = GetCurrentTimestamp() + DRAIN_WAIT_US;

	while (terminate_workers(db) + sift_maint_db_workers(m, db) > 0 && GetCurrentTimestamp() < until)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, DRAIN_POLL_MS, sift_wait_event(SIFT_WAIT_NAP));
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

static void
drain_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree, ProcessUtilityContext context, ParamListInfo params, QueryEnvironment *queryEnv, DestReceiver *dest,
			  QueryCompletion *qc)
{
	Oid			db = target_db(pstmt->utilityStmt);
	SiftMaintShmem *m = OidIsValid(db) ? sift_maint_table() : NULL;

	refuse_schema_rename(pstmt->utilityStmt);
	if (m == NULL)
	{
		(prev_utility ? prev_utility : standard_ProcessUtility) (pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
		return;
	}
	sift_maint_db_hold(m, GetCurrentTimestamp(), db, true);
	PG_TRY();
	{
		drain(m, db);
		(prev_utility ? prev_utility : standard_ProcessUtility) (pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		sift_maint_db_hold(m, GetCurrentTimestamp(), db, false);
	}
	PG_END_TRY();
	if (!IsA(pstmt->utilityStmt, CreatedbStmt))
		sift_maint_db_purge(m, db);
}

void
sift_maint_drain_install(void)
{
	prev_utility = ProcessUtility_hook;
	ProcessUtility_hook = drain_utility;
}
