/*
 * core/ops.c - privilege check of the maintenance functions, LOG lines of maintenance, the checks after ALTER INDEX ... SET (options)
 * (see core/ops.h). Nothing here runs on the query or aminsert paths.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "commands/defrem.h"
#include "access/reloptions.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "core/sift.h"
#include "bm25/analyze/analyze.h"
#include "col/am.h"
#include "col/pg.h"
#include "col/keystore/keystore.h"
#include "core/errctx.h"
#include "core/format_ux.h"
#include "core/ops.h"
#include "core/store/store.h"

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
	int64		us = GetCurrentTimestamp() - start;
	char		took[SIFT_UX_QUANTITY],
				count[SIFT_UX_QUANTITY],
				size[SIFT_UX_QUANTITY];
	StringInfoData extra;

	if (sift_log_min_duration < 0 || us / 1000 < sift_log_min_duration)
		return;
	initStringInfo(&extra);
	if (docs >= 0)
		appendStringInfo(&extra, ", %s documents", sift_fmt_count(count, sizeof(count), docs));
	if (bytes >= 0)
		appendStringInfo(&extra, ", %s", sift_fmt_bytes(size, sizeof(size), bytes));
	ereport(LOG,
			(errmsg("sift: %s index \"%s\" in %s%s", what, RelationGetRelationName(index), sift_fmt_duration(took, sizeof(took), us), extra.data)));
}

#define WARN_SLOTS	8

bool
sift_warn_due(Relation index, SiftWarnKind kind)
{
	static struct
	{
		Oid			relid;
		TimestampTz at;
	}			seen[SIFT_WARN_KINDS][WARN_SLOTS];
	TimestampTz now = GetCurrentTimestamp();
	int			at = 0;

	for (int i = 0; i < WARN_SLOTS; i++)
	{
		if (seen[kind][i].relid == RelationGetRelid(index))
		{
			at = i;
			break;
		}
		if (seen[kind][i].at < seen[kind][at].at)
			at = i;
	}
	if (seen[kind][at].relid == RelationGetRelid(index) && now - seen[kind][at].at < USECS_PER_MINUTE)
		return false;
	seen[kind][at].relid = RelationGetRelid(index);
	seen[kind][at].at = now;
	return true;
}

void
sift_log_stall(Relation index, TimestampTz start)
{
	static TimestampTz last;
	long		ms = elapsed_ms(start);

	if (sift_log_min_duration < 0 || ms < sift_log_min_duration || GetCurrentTimestamp() - last < USECS_PER_MINUTE)
		return;
	last = GetCurrentTimestamp();
	ereport(LOG,
			(errmsg("sift: an insert waited %ld ms for room in the directory of index \"%s\"", ms, RelationGetRelationName(index)),
			 errhint("The maintenance worker is behind: it promotes and merges the segments, and a seal waits for it when the directory is nearly full.")));
}

static void
maint_errcontext(void *arg)
{
	char	   *name = sift_errctx_name((Oid) (uintptr_t) arg);

	errcontext("sift maintenance worker of index \"%s\"", name);
	pfree(name);
}

void
sift_maint_context(ErrorContextCallback *cb, Oid indexoid)
{
	cb->callback = maint_errcontext;
	cb->arg = (void *) (uintptr_t) indexoid;
	cb->previous = error_context_stack;
	error_context_stack = cb;
}

/* An index that holds documents analyzed with other options than it now has is not left so: the ALTER INDEX fails, and the queries and inserts of its table go on */
static void
check_analyzer_options(Relation index)
{
	SiftDirSnapshot *snap;
	SiftAnalyzerConfig cfg;

	snap = sift_dir_read(index);
	sift_analyzer_config_from_index(index, &cfg);
	if (sift_dir_layout_differs(snap, cfg.hash))
		sift_lane_alter_refuse(index, "analyzer options");
	pfree(snap);
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
	bool		ours = false,
				table = false;

	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);
	if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
		sift_col_track_dropping(objectId), sift_keystore_note_drop(objectId);
	if (access != OAT_POST_ALTER || classId != RelationRelationId || subId != 0)
		return;
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_class cls = (Form_pg_class) GETSTRUCT(tuple);

		table = cls->relkind == RELKIND_RELATION;
		ours = cls->relkind == RELKIND_INDEX && (cls->relam == sift_catalog()->am || cls->relam == sift_col_am_oid());
		if (ours)
			before = index_reloptions_copy(tuple);
		ReleaseSysCache(tuple);
	}
	if (table)
		sift_keystore_follow_owner(objectId);
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
				sift_col_alter_check(index, PointerGetDatum(before));
			else
				check_analyzer_options(index);
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
	sift_col_track_init();
	sift_keystore_init();
}
