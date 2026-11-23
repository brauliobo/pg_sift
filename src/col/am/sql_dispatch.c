/*
 * col/am/sql_dispatch.c - the SQL functions that read or drive the file of an index (sift.col_space, sift.col_extents, sift.col_verify ...): each is answered by the table of
 * the format of its index (col/am/format_ops.h), so no format reads the blocks of another. An index of no sift_col format (a BM25 index, a sift_table) is the directory formats' to answer.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "fmgr.h"

#include "col/am/format_ops.h"
#include "col/am.h"
#include "col/pg.h"
#include "core/check.h"
#include "core/errctx.h"
#include "core/meta_peek.h"
#include "core/sql.h"
#include "table/dispatch.h"

PG_FUNCTION_INFO_V1(sift_col_tracking);
PG_FUNCTION_INFO_V1(sift_col_refresh);

static bool
f2_table_oid(Oid relid)
{
	Relation	rel = try_relation_open(relid, AccessShareLock);
	bool		f2;

	if (rel == NULL)
		return false;
	f2 = rel->rd_rel->relkind != RELKIND_INDEX && sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;
	relation_close(rel, AccessShareLock);
	return f2;
}

static const SiftColFormatOps *
ops_of_index(Oid indexoid)
{
	return sift_col_index_oid_format(indexoid) == SIFT_FORMAT2_MAJOR || f2_table_oid(indexoid) ? &sift_col_v2_ops : &sift_col_v1_ops;
}

static Datum
dispatch(SiftColSqlFn fn, FunctionCallInfo fcinfo)
{
	return ops_of_index(PG_GETARG_OID(0))->sql(fn, fcinfo);
}

typedef struct OpsOf
{
	Oid			indexoid;
	const SiftColFormatOps *ops;
} OpsOf;

static void
resolve_ops(void *arg)
{
	OpsOf	   *o = arg;

	o->ops = ops_of_index(o->indexoid);
}

/* The format is read from block 0 inside a step of the check: a block 0 that is not a metapage is the problem found, and this report of damage takes no index out of service */
void
sift_col_check(SiftCheck *ck, Oid indexoid, bool heapallindexed)
{
	SiftErrCtx	ctx;
	OpsOf		o = {.indexoid = indexoid};
	bool		resolved;

	sift_errctx_push_diagnostic(&ctx, indexoid);
	resolved = sift_check_step(ck, SIFT_CHECK_WHOLE, resolve_ops, &o);
	sift_errctx_pop(&ctx);
	if (resolved)
		o.ops->check(ck, indexoid, heapallindexed);
}

#define SIFT_SQL_ENTRY(name, fn) \
	extern PGDLLEXPORT Datum name(PG_FUNCTION_ARGS); \
	Datum name(PG_FUNCTION_ARGS) \
	{ \
		return dispatch(fn, fcinfo); \
	}

SIFT_SQL_ENTRY(sift_col_space, SIFT_SQL_SPACE)
SIFT_SQL_ENTRY(sift_col_extents, SIFT_SQL_EXTENTS)
SIFT_SQL_ENTRY(sift_col_shrink_history, SIFT_SQL_SHRINK_HISTORY)
SIFT_SQL_ENTRY(sift_col_index_info, SIFT_SQL_INDEX_INFO)
SIFT_SQL_ENTRY(sift_col_segments, SIFT_SQL_SEGMENTS)
SIFT_SQL_ENTRY(sift_col_leaves, SIFT_SQL_LEAVES)
SIFT_SQL_ENTRY(sift_col_sections, SIFT_SQL_SECTIONS)
SIFT_SQL_ENTRY(sift_col_verify, SIFT_SQL_VERIFY)
SIFT_SQL_ENTRY(sift_col_maintain, SIFT_SQL_MAINTAIN)
SIFT_SQL_ENTRY(sift_col_maintenance_due, SIFT_SQL_MAINTENANCE_DUE)
SIFT_SQL_ENTRY(sift_col_maintenance_status, SIFT_SQL_MAINTENANCE_STATUS)
SIFT_SQL_ENTRY(sift_col_tracking, SIFT_SQL_TRACKING)
SIFT_SQL_ENTRY(sift_worker_state, SIFT_SQL_WORKER_STATE)
SIFT_SQL_ENTRY(sift_worker_history, SIFT_SQL_WORKER_HISTORY)

/* not STRICT: a NULL index gives NULL, as it did before the format chose the answer */
Datum
sift_col_refresh(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	return dispatch(SIFT_SQL_REFRESH, fcinfo);
}
