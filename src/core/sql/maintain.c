/*
 * core/sql/maintain.c - sift.maintain(rel): one maintenance step of a sift index, a sift_col index or a sift_table, now and in this session: the buffers promoted, the parts merged, the dead rows
 * cleared, the free end of the file returned. busy: the lock of the relation is held (a VACUUM, a merge, a worker) and nothing was done.
 *
 * Each kind runs the steps it has: sift and sift_col of Format 1 the promotion and the merges of core/maint/maint.h (what sift.promote and sift.merge call), the rewrite of the segments that churn left dirty (ops->rewrite_due) and the cut of the free end; a sift_table of
 * Format 1 sift_tbl_maintain (VACUUM's work on the segments and the file, without the indexes); sift_col of Format 2.0 the cleanup of its VACUUM (the slabs, the compaction, the summaries and the
 * end of the file); sift_table of Format 2.0 the relation_vacuum of its table access method, which is the same steps for the table. For Format 2.0 promoted and merged are read from the manifest
 * before and after: the slabs that are gone, and the parts that are gone net of the ones the slabs became; dead_rows is the dead rows that are gone.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type_d.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/rel.h"

#include "col/f2/meta.h"
#include "col/f2/f2.h"
#include "core/catname.h"
#include "core/explain.h"
#include "core/f2/meta.h"
#include "core/maint/maint.h"
#include "core/maint/int.h"
#include "core/meta_peek.h"
#include "core/ops.h"
#include "core/sql/api.h"
#include "table/dispatch.h"
#include "table/v1/table.h"

PG_FUNCTION_INFO_V1(sift_maintain);

/* columns of sift.maintain, in the order of sql/6.0/15-unified-api.sql */
enum MaintainCol
{
	MT_BUSY,
	MT_DEAD_ROWS,
	MT_PROMOTED,
	MT_MERGED,
	MT_BLOCKS_BEFORE,
	MT_BLOCKS_AFTER,
	MT_NCOLS
};

static const Oid maintain_types[MT_NCOLS] = {BOOLOID, INT8OID, INT4OID, INT4OID, INT8OID, INT8OID};

typedef struct MaintainResult
{
	bool		busy;
	int64		dead_rows;
	int32		promoted;
	int32		merged;
	int64		blocks_before;
	int64		blocks_after;
} MaintainResult;

/* sift and sift_col of Format 1: seal and promote the buffers, merge what the policy picks, cut the free end of the file */
static void
maintain_directory(const SiftApiRel *r, MaintainResult *res)
{
	const SiftMaintOps *ops = sift_mops_of(r->rel);
	Relation	index = sift_mops_open_maintain(ops, RelationGetRelid(r->rel));
	SiftIndexStats before = {0},
				after = {0};
	uint64		busy_gen = 0;

	res->blocks_before = RelationGetNumberOfBlocks(index);
	if (!sift_maint_lock(index, false))
		res->busy = true;
	else
	{
		if (r->am == SIFT_API_BM25)
			sift_bm25_stats(index, &before);
		else
			sift_col_stats(index, &before);
		if (ops->rewrite_due != NULL && ops->rewrite_due(index) != NULL)
		{
			int			rewritten;

			sift_maint_unlock(index);
			rewritten = sift_mops_rewrite_ex(ops, index, &res->promoted);
			res->merged = Max(0, rewritten);
		}
		else
		{
			res->promoted = promote_locked(ops, index, true, true);
			while (ops->merge_next(index, false, SIFT_GOAL_ANY, true, &busy_gen) == SIFT_STEP_DONE)
			{
				res->merged++;
				CHECK_FOR_INTERRUPTS();
			}
			sift_maint_unlock(index);
		}
		sift_mops_cut_free_end(ops, index);
		if (r->am == SIFT_API_BM25)
			sift_bm25_stats(index, &after);
		else
			sift_col_stats(index, &after);
		res->dead_rows = Max(0, before.dead_rows - after.dead_rows);
	}
	res->blocks_after = RelationGetNumberOfBlocks(index);
	index_close(index, RowExclusiveLock);
}

/* a sift_table of Format 1: the steps of VACUUM on its segments and its file, without the indexes */
static void
maintain_table_v1(const SiftApiRel *r, MaintainResult *res)
{
	Oid			relid = RelationGetRelid(r->rel);
	SiftTblMaintain t = {0};

	if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(r->rel->rd_rel->relkind), RelationGetRelationName(r->rel));
	if (ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock))
	{
		sift_tbl_maintain(r->rel, false, true, &t);
		UnlockRelationOid(relid, ShareUpdateExclusiveLock);
	}
	else
		t.busy = true;
	*res = (MaintainResult) {t.busy, t.dead_rows, t.promoted, t.merged, t.blocks_before, t.blocks_after};
}

/* What a step of Format 2.0 changed in the manifest: the dead rows and the parts counted by the metapage and the manifest */
typedef struct F2Shape
{
	uint64		dead;
	SiftApiF2Count count;
	int64		blocks;
} F2Shape;

static F2Shape
f2_shape(Relation rel)
{
	return (F2Shape) {sift_col_meta_read(rel).total_dead, sift_api_f2_count(rel), RelationGetNumberOfBlocks(rel)};
}

static void
f2_result(const F2Shape *before, const F2Shape *after, MaintainResult *res)
{
	res->blocks_before = before->blocks;
	res->blocks_after = after->blocks;
	res->dead_rows = before->dead > after->dead ? (int64) (before->dead - after->dead) : 0;
	res->promoted = (int32) Max(0, before->count.slabs - after->count.slabs);
	res->merged = (int32) Max(0, before->count.parts + res->promoted - after->count.parts);
}

/* Whether another session holds the maintenance of the relation: probed, never kept */
static bool
f2_maintenance_held(Relation rel)
{
	if (!ConditionalLockPage(rel, SIFT_F2_MAINT_LOCK_BLKNO, ExclusiveLock))
		return true;
	UnlockPage(rel, SIFT_F2_MAINT_LOCK_BLKNO, ExclusiveLock);
	return false;
}

/* sift_col of Format 2.0: what the VACUUM of its table does for the index, the heap held as VACUUM holds it */
static void
maintain_col_f2(const SiftApiRel *r, MaintainResult *res)
{
	Oid			heapoid = r->rel->rd_index->indrelid;
	IndexVacuumInfo info = {.index = r->rel};
	F2Shape		before,
				after;

	sift_check_maintain(r->rel);
	if (f2_maintenance_held(r->rel) || !ConditionalLockRelationOid(heapoid, ShareUpdateExclusiveLock))
	{
		res->busy = true;
		res->blocks_before = res->blocks_after = RelationGetNumberOfBlocks(r->rel);
		return;
	}
	before = f2_shape(r->rel);
	(void) sift_f2_amvacuumcleanup(&info, NULL);
	after = f2_shape(r->rel);
	UnlockRelationOid(heapoid, ShareUpdateExclusiveLock);
	f2_result(&before, &after, res);
}

/* sift_table of Format 2.0: the vacuum of its table access method, which keeps the table's steps (the slabs, the compaction, the end of the file) in one place */
static void
maintain_table_f2(const SiftApiRel *r, MaintainResult *res)
{
	Oid			relid = RelationGetRelid(r->rel);
	VacuumParams params = {
		.options = VACOPT_VACUUM,
		.freeze_min_age = -1,
		.freeze_table_age = -1,
		.multixact_freeze_min_age = -1,
		.multixact_freeze_table_age = -1,
		.SIFT_VACPARAM_LOG_MIN_DURATION = -1,
		.index_cleanup = VACOPTVALUE_DISABLED,
		.truncate = VACOPTVALUE_ENABLED
	};
	F2Shape		before,
				after;

	if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(r->rel->rd_rel->relkind), RelationGetRelationName(r->rel));
	if (f2_maintenance_held(r->rel) || !ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock))
	{
		res->busy = true;
		res->blocks_before = res->blocks_after = RelationGetNumberOfBlocks(r->rel);
		return;
	}
	before = f2_shape(r->rel);
	sift_tbl_ops(r->rel)->tam->relation_vacuum(r->rel, &params, GetAccessStrategy(BAS_VACUUM));
	after = f2_shape(r->rel);
	UnlockRelationOid(relid, ShareUpdateExclusiveLock);
	f2_result(&before, &after, res);
}

/* sift.promote and sift.merge of a relation of Format 2.0, which has no directory of segments to seal or merge one by one: the step of sift.maintain, and the slabs and the parts it took away. False for any other relation */
bool
sift_maintain_f2(Oid relid, const char *what, int32 *promoted, int32 *merged)
{
	MaintainResult res = {0};
	SiftApiRel	r;
	Relation	rel = relation_open(sift_rel_must_exist(relid), AccessShareLock);
	SiftApiAm	am;
	bool		f2 = sift_api_am_of(rel, &am) && (rel->rd_rel->relkind == RELKIND_INDEX || rel->rd_rel->relkind == RELKIND_RELATION) &&
		sift_meta_peek_format(rel).status == SIFT_FMT_V2;

	relation_close(rel, AccessShareLock);
	if (!f2)
		return false;
	PreventCommandIfReadOnly(what);
	PreventCommandDuringRecovery(what);
	r = sift_api_open(relid, AccessShareLock);
	if (r.am == SIFT_API_COL)
		maintain_col_f2(&r, &res);
	else
		maintain_table_f2(&r, &res);
	sift_api_close(&r, AccessShareLock);
	*promoted = res.promoted;
	*merged = res.merged;
	return true;
}

Datum
sift_maintain(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	MaintainResult res = {0};
	SiftApiRel	r;
	Datum		v[MT_NCOLS];
	bool		nulls[MT_NCOLS] = {false};

	PreventCommandIfReadOnly("sift.maintain()");
	PreventCommandDuringRecovery("sift.maintain()");
	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, maintain_types, MT_NCOLS);
	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	if (r.f2 && r.am == SIFT_API_COL)
		maintain_col_f2(&r, &res);
	else if (r.f2)
		maintain_table_f2(&r, &res);
	else if (r.am == SIFT_API_TABLE && r.peek.status == SIFT_FMT_V1)
		maintain_table_v1(&r, &res);
	else if (r.peek.status == SIFT_FMT_V1)
		maintain_directory(&r, &res);
	v[MT_BUSY] = BoolGetDatum(res.busy);
	v[MT_DEAD_ROWS] = Int64GetDatum(res.dead_rows);
	v[MT_PROMOTED] = Int32GetDatum(res.promoted);
	v[MT_MERGED] = Int32GetDatum(res.merged);
	v[MT_BLOCKS_BEFORE] = Int64GetDatum(res.blocks_before);
	v[MT_BLOCKS_AFTER] = Int64GetDatum(res.blocks_after);
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}
