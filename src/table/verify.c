/*
 * table/verify.c - sift.table_verify(table) and the sift_table side of sift.check(): the structural checks of every column segment (chunks, zones, the cube
 * and rollups against the chunks), one segment at a time as sift.col_verify does for an index (col/verify.c)
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_class.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/seg.h"
#include "core/check.h"
#include "core/errctx.h"
#include "core/sql.h"
#include "table/table.h"

typedef struct TblVerify
{
	SiftErrCtx	errctx;
	Relation	rel;
	SiftSegEntry *segs;			/* the column segments of the directory read at the start */
	int			nsegs;
	const SiftSegEntry *seg;
	int64		live;
} TblVerify;

static void
step_directory(void *arg)
{
	TblVerify  *tv = arg;
	SiftTblRel *st = sift_tbl_rel(tv->rel);

	sift_tbl_refresh(tv->rel, st);
	tv->segs = palloc(Max(st->dir.nsegs, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			tv->segs[tv->nsegs++] = st->dir.segs[i];
}

/* One segment under the maintenance lock for its length alone (as col/verify.c); the registered snapshot keeps the blocks of the others */
static void
step_segment(void *arg)
{
	TblVerify  *tv = arg;
	SiftColSegment *seg;

	LockPage(tv->rel, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	seg = sift_cseg_open(tv->rel, tv->seg, CurrentMemoryContext);
	tv->live += sift_cseg_verify(seg);
	sift_cseg_close(seg);
	UnlockPage(tv->rel, SIFT_MAINT_LOCK_BLKNO, ShareLock);
}

static int64
tbl_verify(SiftCheck *ck, Relation rel)
{
	TblVerify  *tv = palloc0(sizeof(TblVerify));
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	bool		sound;

	tv->rel = rel;
	sift_errctx_push_diagnostic(&tv->errctx, RelationGetRelid(rel));
	sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_directory, tv);
	for (int i = 0; sound && i < tv->nsegs; i++)
	{
		tv->seg = &tv->segs[i];
		sift_errctx_segment(&tv->errctx, tv->seg->gen);
		sift_check_step(ck, tv->seg->gen, step_segment, tv);
	}
	sift_errctx_pop(&tv->errctx);
	UnregisterSnapshot(snapshot);
	return tv->live;
}

static Relation
open_verifiable(Oid relid)
{
	Relation	rel = table_open(relid, AccessShareLock);

	sift_tbl_require(rel);
	if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
	return rel;
}

PG_FUNCTION_INFO_V1(sift_table_verify);

/* sift.table_verify(table): the live rows of its column segments; an error on damage */
Datum
sift_table_verify(PG_FUNCTION_ARGS)
{
	Relation	rel = open_verifiable(PG_GETARG_OID(0));
	int64		live = tbl_verify(NULL, rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(live);
}

void
sift_tbl_check(SiftCheck *ck, Oid relid)
{
	Relation	rel = open_verifiable(relid);

	tbl_verify(ck, rel);
	table_close(rel, AccessShareLock);
}
