/*
 * table/f2/upgrade.c - sift.upgrade_table(regclass) RETURNS bool: a Format 1 sift_table rewritten as Format 2.0.
 *
 * The rewrite is the one of VACUUM FULL: a new heap is made while the next relations are Format 2.0, the rows of the old one stream into it frozen under the exclusive lock,
 * and finish_heap_swap() swaps the files and rebuilds the indexes. The old format is read by its own scan; this file knows no other file of it. A table that is Format 2.0 already, or has
 * no block yet, is left as it is: the answer is false and a NOTICE says why, so that a script can run it over every table again. The right to run it is that of VACUUM FULL: MAINTAIN.
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#if PG_VERSION_NUM >= 190000
#include "commands/repack.h"
#else
#include "commands/cluster.h"
#endif
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "core/catname.h"
#include "core/meta_peek.h"
#include "core/inspect.h"
#include "core/ops.h"
#include "table/f2/tbl.h"

PG_FUNCTION_INFO_V1(sift_upgrade_table);

static Relation
open_for_upgrade(Oid relid)
{
	Relation	rel;

	sift_require_acl(relid, ACL_MAINTAIN);
	rel = table_open(sift_rel_must_exist(relid), AccessExclusiveLock);
	sift_require_acl(relid, ACL_MAINTAIN);
	sift_tbl_require(rel);
	return rel;
}

static Datum
left_as_it_is(Relation rel, const char *why)
{
	ereport(NOTICE, (errmsg("sift_table \"%s\" %s", RelationGetRelationName(rel), why)));
	table_close(rel, AccessExclusiveLock);
	PG_RETURN_BOOL(false);
}

static char *
size_text(int64 bytes)
{
	return TextDatumGetCString(DirectFunctionCall1(pg_size_pretty, Int64GetDatum(bytes)));
}

/* The rows of the old table in a new heap that swaps with it, made while the next relations are Format 2.0; *rows: the rows copied */
static void
rewrite_as_format2(Relation old, double *rows)
{
	Oid			relid = RelationGetRelid(old);
	char		persistence = old->rd_rel->relpersistence;

	sift_tbl_push_new_format(SIFT_FORMAT2_MAJOR);
	PG_TRY();
	{
		Oid			newid = make_new_heap(relid, old->rd_rel->reltablespace, old->rd_rel->relam, persistence, AccessExclusiveLock);
		Relation	new = table_open(newid, AccessExclusiveLock);

		*rows = sift_tbl_f2_rewrite_rows(old, new);
		table_close(new, NoLock);
		table_close(old, NoLock);
		finish_heap_swap(relid, newid, false, false, false, true, SIFT_HEAP_SWAP_REINDEX_ARG RecentXmin, GetOldestMultiXactId(), persistence);
	}
	PG_FINALLY();
	{
		sift_tbl_pop_new_format();
	}
	PG_END_TRY();
}

static Datum
upgrade(Relation old)
{
	Oid			relid = RelationGetRelid(old);
	char	   *name = pstrdup(RelationGetRelationName(old));
	int64		bytes_before = table_relation_size(old, MAIN_FORKNUM);
	TimestampTz started = GetCurrentTimestamp();
	double		rows = 0;
	Relation	done;
	int64		bytes_after;

	rewrite_as_format2(old, &rows);
	done = table_open(relid, NoLock);
	bytes_after = table_relation_size(done, MAIN_FORKNUM);
	table_close(done, NoLock);
	ereport(NOTICE, (errmsg("sift_table \"%s\" upgraded to Format 2.0 (%lld rows, %s -> %s, %.1f ms)", name, (long long) rows, size_text(bytes_before), size_text(bytes_after),
							(double) (GetCurrentTimestamp() - started) / 1000.0)));
	PG_RETURN_BOOL(true);
}

Datum
sift_upgrade_table(PG_FUNCTION_ARGS)
{
	Relation	rel = open_for_upgrade(PG_GETARG_OID(0));
	SiftMetaPeek peek = sift_meta_peek_format(rel);

	if (peek.status == SIFT_FMT_V2)
		return left_as_it_is(rel, "is already on Format 2.0");
	if (peek.status == SIFT_FMT_EMPTY)
		return left_as_it_is(rel, "is empty; new writes will use Format 2.0");
	if (peek.status == SIFT_FMT_OTHER)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("sift_table \"%s\" has format major %u, which this build (pg_sift %s) does not upgrade", RelationGetRelationName(rel), peek.format_major, SIFT_MODULE_VERSION),
						errhint("Dump and restore the table with the pg_sift release that wrote it.")));
	if (peek.status == SIFT_FMT_BAD)
		SIFT_CORRUPT(rel, "Block 0 is not a metapage.");
	return upgrade(rel);
}
