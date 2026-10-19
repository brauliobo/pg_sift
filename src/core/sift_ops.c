/*
 * sift_ops.c - privilege check of the maintenance functions, LOG lines of maintenance, ALTER INDEX warning
 * (see sift_ops.h). Nothing here runs on the query or aminsert paths.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "sift.h"
#include "sift_analyze.h"
#include "sift_ops.h"
#include "sift_store.h"

int			sift_log_min_duration = 1000;

static ProcessUtility_hook_type prev_utility_hook;

void
sift_check_maintain(Relation index)
{
	Oid			heap = index->rd_index->indrelid;

	if (pg_class_aclcheck(heap, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for index \"%s\"", RelationGetRelationName(index)),
				 errdetail("Only the owner of table \"%s\" or a role with the MAINTAIN privilege on it can do this.",
						   get_rel_name(heap))));
}

static long
elapsed_ms(TimestampTz start)
{
	long		secs;
	int			usecs;

	TimestampDifference(start, GetCurrentTimestamp(), &secs, &usecs);
	return secs * 1000 + usecs / 1000;
}

void
sift_log_maint(Relation index, const char *what, TimestampTz start, int64 docs, int64 bytes)
{
	long		ms = elapsed_ms(start);
	StringInfoData size;

	if (sift_log_min_duration < 0 || ms < sift_log_min_duration)
		return;
	initStringInfo(&size);
	if (docs >= 0)
		appendStringInfo(&size, ", %lld documents", (long long) docs);
	if (bytes >= 0)
		appendStringInfo(&size, ", %lld bytes", (long long) bytes);
	ereport(LOG,
			(errmsg("sift: %s index \"%s\" in %ld ms%s", what, RelationGetRelationName(index), ms, size.data)));
}

void
sift_log_stall(Relation index, TimestampTz start)
{
	long		ms = elapsed_ms(start);

	if (sift_log_min_duration < 0 || ms < sift_log_min_duration)
		return;
	ereport(LOG,
			(errmsg("sift: an insert waited %ld ms for room in the directory of index \"%s\"", ms, RelationGetRelationName(index)),
			 errhint("The maintenance worker is behind: it promotes and merges the segments, and a seal waits for it when the directory is nearly full.")));
}

void
sift_log_refused(Relation index)
{
	static TimestampTz last;
	TimestampTz now = GetCurrentTimestamp();

	if (now - last < USECS_PER_MINUTE)
		return;
	last = now;
	ereport(LOG,
			(errmsg("sift: could not start the maintenance worker of index \"%s\"", RelationGetRelationName(index)),
			 errhint("All max_worker_processes (%d) are in use; the inserting sessions promote and merge instead.", max_worker_processes)));
}

static void
maint_errcontext(void *arg)
{
	errcontext("sift maintenance worker of index %u", (Oid) (uintptr_t) arg);
}

void
sift_maint_context(ErrorContextCallback *cb, Oid indexoid)
{
	cb->callback = maint_errcontext;
	cb->arg = (void *) (uintptr_t) indexoid;
	cb->previous = error_context_stack;
	error_context_stack = cb;
}

/* Does the index hold documents analyzed with other options than it now has? */
static bool
analyzer_changed(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftAnalyzerConfig cfg;

	sift_dir_read(index, snap);
	sift_analyzer_config_from_index(index, &cfg);
	return snap->meta.analyzer_hash != cfg.hash;
}

/* The index an ALTER INDEX ... SET / RESET (options) changed, InvalidOid for any other command */
static Oid
altered_options_index(AlterTableStmt *stmt)
{
	bool		set = false;

	if (stmt->objtype != OBJECT_INDEX)
		return InvalidOid;
	foreach_node(AlterTableCmd, cmd, stmt->cmds)
		set |= cmd->subtype == AT_SetRelOptions || cmd->subtype == AT_ResetRelOptions;
	return set ? RangeVarGetRelid(stmt->relation, NoLock, true) : InvalidOid;
}

static void
warn_analyzer_options(Oid relid)
{
	Relation	index = relation_open(relid, NoLock);

	if (index->rd_rel->relam == sift_catalog()->am && analyzer_changed(index))
		ereport(WARNING,
				(errmsg("index \"%s\" was built with other analyzer options", RelationGetRelationName(index)),
				 errdetail("Queries and inserts on its table fail until the index is rebuilt with REINDEX."),
				 errhint("To change them without downtime, CREATE INDEX CONCURRENTLY a new index with the options and drop this one.")));
	relation_close(index, NoLock);
}

static void
sift_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree, ProcessUtilityContext context,
			 ParamListInfo params, QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *stmt = pstmt->utilityStmt;

	if (prev_utility_hook)
		prev_utility_hook(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	if (IsA(stmt, AlterTableStmt))
	{
		Oid			relid = altered_options_index((AlterTableStmt *) stmt);

		if (relid != InvalidOid)
		{
			Relation	index = relation_open(relid, NoLock);

			/* an option that does not fit the index (a name that is not a leaf) fails the ALTER, not the next use of the index */
			sift_col_alter_check(index);
			relation_close(index, NoLock);
			if (sift_catalog()->am != InvalidOid)
				warn_analyzer_options(relid);
		}
	}
}

void
sift_ops_init(void)
{
	DefineCustomIntVariable("sift.log_min_duration",
							"Logs the promotions, merges and maintenance worker runs of an index that take at least this long (ms), -1 to log none.",
							NULL, &sift_log_min_duration, 1000, -1, INT_MAX,
							PGC_SUSET, GUC_UNIT_MS, NULL, NULL, NULL);
	prev_utility_hook = ProcessUtility_hook;
	ProcessUtility_hook = sift_utility;
}
