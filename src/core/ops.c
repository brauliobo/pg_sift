/*
 * core/ops.c - privilege check of the maintenance functions, LOG lines of maintenance, the checks after ALTER INDEX ... SET (options)
 * (see core/ops.h). Nothing here runs on the query or aminsert paths.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "core/sift.h"
#include "bm25/analyze.h"
#include "col/pg.h"
#include "core/ops.h"
#include "core/store.h"

int			sift_log_min_duration = 1000;

static object_access_hook_type prev_object_access_hook;

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

static void
warn_analyzer_options(Relation index)
{
	if (analyzer_changed(index))
		ereport(WARNING,
				(errmsg("index \"%s\" was built with other analyzer options", RelationGetRelationName(index)),
				 errdetail("Queries and inserts on its table fail until the index is rebuilt with REINDEX."),
				 errhint("To change them without downtime, CREATE INDEX CONCURRENTLY a new index with the options and drop this one.")));
}

/* The reloptions of an index as the catalog holds them now (a copy), NULL when it has none */
static ArrayType *
index_reloptions_copy(HeapTuple tuple)
{
	bool		isnull;
	Datum		d = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

	return isnull ? NULL : DatumGetArrayTypePCopy(d);
}

/*
 * After ALTER INDEX ... SET / RESET (options) of a sift or sift_col index: the new options must fit the index (a name that is not a leaf of it, a type the
 * option does not take), which fails the ALTER and not the queries, inserts and VACUUM that would read them next. An object access hook and not a
 * ProcessUtility hook: the first ALTER of a session that has not used sift loads this library while it runs, after the utility hook of the statement was
 * read, and PostgreSQL reads the object access hook when the catalog row is updated. That is before the command counter moves on: the old row is the one
 * in the cache until then, so the options are compared across the increment, and the check reads the new ones.
 */
static void
sift_object_access(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
	HeapTuple	tuple;
	ArrayType  *before = NULL;
	bool		ours = false;

	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);
	if (access != OAT_POST_ALTER || classId != RelationRelationId || subId != 0)
		return;
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_class cls = (Form_pg_class) GETSTRUCT(tuple);

		ours = cls->relkind == RELKIND_INDEX && (cls->relam == sift_catalog()->am || cls->relam == sift_col_am_oid());
		if (ours)
			before = index_reloptions_copy(tuple);
		ReleaseSysCache(tuple);
	}
	if (!ours)
		return;
	CommandCounterIncrement();
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
	if (HeapTupleIsValid(tuple))
	{
		ArrayType  *after = index_reloptions_copy(tuple);
		bool		same = before == NULL || after == NULL ? before == after : VARSIZE(before) == VARSIZE(after) && memcmp(before, after, VARSIZE(before)) == 0;

		ReleaseSysCache(tuple);
		if (!same)
		{
			Relation	index = relation_open(objectId, NoLock);

			if (sift_col_is_index(index))
				sift_col_alter_check(index);
			else
				warn_analyzer_options(index);
			relation_close(index, NoLock);
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
	prev_object_access_hook = object_access_hook;
	object_access_hook = sift_object_access;
}
