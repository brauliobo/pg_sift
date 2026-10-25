/*
 * table/load.c - the rows of a bulk load (COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW) written straight into column groups
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/genam.h"
#include "access/transam.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/seg.h"
#include "core/page.h"
#include "table/table.h"
#include "table/seg_int.h"

bool		sift_tbl_bulk_groups = true;

/* Does the expression call a function (or operator) a user defined? Those can read the table; the built-in ones cannot */
static bool
calls_user_function(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstNormalObjectId)
		return true;
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		set_opfuncid(op);
		if (op->opfuncid >= FirstNormalObjectId)
			return true;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(op);
		if (op->opfuncid >= FirstNormalObjectId)
			return true;
	}
	return expression_tree_walker(node, calls_user_function, context);
}

/* The expressions the executor evaluates for every row of the table (defaults, generated columns, CHECK constraints) */
static bool
row_expressions_call_user_function(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	TupleConstr *constr = desc->constr;

	for (int i = 0; i < desc->natts; i++)
		if (!TupleDescAttr(desc, i)->attisdropped && get_typtype(TupleDescAttr(desc, i)->atttypid) == TYPTYPE_DOMAIN)
			return true;
	if (constr == NULL)
		return false;
	for (int i = 0; i < constr->num_defval; i++)
		if (calls_user_function(stringToNode(constr->defval[i].adbin), NULL))
			return true;
	for (int i = 0; i < constr->num_check; i++)
		if (calls_user_function(stringToNode(constr->check[i].ccbin), NULL))
			return true;
	return false;
}

/*
 * The rows of a load wait in the backend until a group is written, so nothing may read them in the middle of the command: a unique or
 * exclusion index checks the rows inserted before it, a trigger (a foreign key is one) can read the table, and so can a function of the
 * table's own expressions (a default, a generated column, a CHECK, a domain, an index expression or predicate) when the user wrote it: it runs
 * for every row, and a volatile one sees the rows inserted before it by the same command. The load of such a table keeps the write buffers.
 * A published table keeps them too: its changes are logged row by row, as they are inserted.
 */
const char *
sift_tbl_bulk_refusal(Relation rel)
{
	const char *reason = NULL;

	if (rel->trigdesc != NULL)
		return "a trigger or a foreign key can read the table";
	if (sift_tbl_published(rel))
		return "the table is published for logical decoding";
	if (row_expressions_call_user_function(rel))
		return "a default, a check or a domain of the table calls a function of the user";
	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		Form_pg_index ix;

		if (!HeapTupleIsValid(tup))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tup);
		if (ix->indisunique)
			reason = "a unique index checks the rows of the load";
		else if (ix->indisexclusion)
			reason = "an exclusion constraint checks the rows of the load";
		ReleaseSysCache(tup);
		if (reason == NULL)
		{
			Relation	index = index_open(indexoid, AccessShareLock);

			if (calls_user_function((Node *) RelationGetIndexExpressions(index), NULL) ||
				calls_user_function((Node *) RelationGetIndexPredicate(index), NULL))
				reason = "an index expression or predicate calls a function of the user";
			index_close(index, AccessShareLock);
		}
		if (reason)
			break;
	}
	return reason;
}

/*
 * A load takes whole superblocks of TIDs (a group owns its superblocks), LOAD_SBS at a time: its rows are encoded one superblock per unit and
 * the units of a run become one group, published when the run is full, so a load adds a directory entry about every 300,000 rows and the
 * merges of the writers keep the directory as they do for the groups of the promotions. The rows wait in the backend, in the batches of the
 * run (a write buffer's bytes each), until the run is full; the units are then written and the group published in one go, under the lock
 * that serializes the creations of segments (what the orphan sweep of a VACUUM waits for: the blocks written are in no directory until then).
 * The rows past the last full run are written when the command ends (finish_bulk_insert), or at the latest before the transaction commits,
 * so a row never outlives its transaction in memory.
 */
#define LOAD_SBS		4
#define LOAD_SB_ROWS	((uint64) SIFT_TBL_VBLOCKS_SB * SIFT_TBL_MAX_OFF)

typedef struct Load
{
	Oid			relid;
	SubTransactionId subid;		/* the (sub)transaction whose xid the rows carry */
	bool		refused;		/* the table keeps the write buffers (sift_tbl_bulk_refusal) */
	bool		vers;			/* the rows carry their xmin and cmin; false: frozen */
	bool		open;			/* a run of superblocks is reserved */
	MemoryContext cxt;
	MemoryContext rowcx;
	SealWriter	sw;
	SiftColBatch *batch[LOAD_SBS];	/* the rows of each superblock of the run */
	TupleDesc	desc;
	uint64		run;			/* the first vblock of the run */
	int			units;			/* units of the run that are full */
	uint32		n;				/* rows of the current unit */
	Size		bytes;			/* their size in row format: a unit stops at the bytes of a write buffer */
} Load;

static List *loads = NIL;
static bool callbacks_registered = false;

static Load *
find_load(Oid relid)
{
	foreach_ptr(Load, ld, loads)
		if (ld->relid == relid)
			return ld;
	return NULL;
}

static void
drop_load(Load *ld)
{
	loads = list_delete_ptr(loads, ld);
	MemoryContextDelete(ld->cxt);
}

static void
reserve_vblocks(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SIFT_TBL_NEXT_VBLOCK(meta) = *(uint64 *) arg;
}

/* The next run of superblocks, taken under the lock that serializes the write buffers' own reservations */
static void
reserve_run(Load *ld, Relation rel, SiftTblRel *st)
{
	uint64		end;

	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	ld->run = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, Max(SIFT_TBL_NEXT_VBLOCK(&st->dir.meta), (uint64) sift_tbl_vblock_floor));
	end = ld->run + (uint64) LOAD_SBS * SIFT_TBL_VBLOCKS_SB;
	if (end > (uint64) MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" exhausted its TIDs", RelationGetRelationName(rel)),
				 errhint("Run VACUUM FULL on the table.")));
	sift_dir_update(rel, &(SiftDirChange) {.fn = reserve_vblocks,.arg = &end,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	ld->open = true;
	ld->units = 0;
	ld->n = 0;
	ld->bytes = 0;
	for (int i = 0; i < LOAD_SBS; i++)
		sift_cbatch_reset(ld->batch[i], (ld->run + (uint64) i * SIFT_TBL_VBLOCKS_SB) / SIFT_SB_PAGES);
}

static void
publish_group(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const SiftSegEntry *seg = arg;

	dir->segs[dir->nsegs++] = *seg;
	meta->ndocs += seg->ndocs;
	meta->sum_dl += seg->sum_dl;
}

/* How many times a load settles a full directory again before it fails */
#define LOAD_ROOM_TRIES 8

/* A directory that is full (or past sift.table_backpressure) is made room in before the group is written, as a new write buffer does */
static void
make_room(Relation rel, SiftTblRel *st)
{
	sift_tbl_refresh(rel, st);
	if (st->dir.nsegs >= sift_tbl_max_segments - 2 || dir_backpressure(st))
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), true,
							   sift_tbl_backpressure ? SIFT_TBL_ROOM_STEPS : SIFT_TBL_SETTLE_STEPS);
}

/* The unit of the current superblock is full: the rows go into the next one */
static void
end_unit(Load *ld)
{
	ld->units++;
	ld->n = 0;
	ld->bytes = 0;
}

/* The units of the run are written and become a group in the directory */
static void
end_run(Load *ld, Relation rel, SiftTblRel *st)
{
	MemoryContext old;
	SiftSegEntry seg;
	bool		any = false;

	for (int i = 0; i < LOAD_SBS; i++)
		any |= ld->batch[i]->nrows > 0;
	ld->open = false;
	if (!any)
		return;
	/* the room made is taken by the writers that were waiting for it before the lock is: settle again, and give up only when it stays full */
	for (int tries = 0;; tries++)
	{
		make_room(rel, st);
		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		if (st->dir.nsegs < sift_tbl_max_segments - 2 || tries == LOAD_ROOM_TRIES)
			break;
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	if (st->dir.nsegs >= sift_tbl_max_segments - 2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
				 errdetail("The groups of a load cannot be merged as fast as it writes them: rows of running transactions are in the way."),
				 errhint("Run VACUUM on the table, or end the transaction that wrote them.")));
	for (int i = 0; i < LOAD_SBS; i++)
	{
		ld->sw.batch = ld->batch[i];
		seal_flush(&ld->sw);
	}
	old = MemoryContextSwitchTo(ld->cxt);
	sift_colw_finish(ld->sw.w, &seg);
	sift_wt_finish(ld->sw.wt);
	ld->sw.w = NULL;
	MemoryContextSwitchTo(old);
	sift_dir_update(rel, &(SiftDirChange) {.fn = publish_group,.arg = &seg,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_tbl_refresh(rel, st);
	if (sift_tbl_inline_promote)
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
}

/* Writes what the load holds: the rows of the current unit, and the group they end */
static void
flush_load(Load *ld, Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (!ld->open)
		return;
	ld->sw.rel = rel;
	ld->sw.l = layout_of(rel, st);
	if (ld->n > 0)
		end_unit(ld);
	end_run(ld, rel, st);
}

static void
flush_and_drop(Load *ld)
{
	Relation	rel = try_table_open(ld->relid, RowExclusiveLock);

	if (rel != NULL)
	{
		if (!ld->refused)
			flush_load(ld, rel);
		table_close(rel, NoLock);
	}
	drop_load(ld);
}

static void
load_xact_event(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			foreach_ptr(Load, ld, list_copy(loads))
				flush_and_drop(ld);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			foreach_ptr(Load, ld, list_copy(loads))
				drop_load(ld);
			break;
		default:
			break;
	}
}

static void
load_subxact_event(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg)
{
	if (event != SUBXACT_EVENT_PRE_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB)
		return;
	foreach_ptr(Load, ld, list_copy(loads))
	{
		if (ld->subid != mysubid)
			continue;
		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
			flush_and_drop(ld);
		else
			drop_load(ld);
	}
}

bool
sift_tbl_load_wanted(Relation rel, SiftTblRel *st, bool frozen)
{
	Oid			relid = RelationGetRelid(rel);
	Load	   *ld = find_load(relid);
	MemoryContext cxt,
				old;
	const SiftTblLayout *l;

	if (ld != NULL)
		return !ld->refused;
	if (!callbacks_registered)
	{
		RegisterXactCallback(load_xact_event, NULL);
		RegisterSubXactCallback(load_subxact_event, NULL);
		callbacks_registered = true;
	}
	cxt = AllocSetContextCreate(TopMemoryContext, "sift_table bulk load", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	ld = palloc0(sizeof(Load));
	ld->relid = relid;
	ld->subid = GetCurrentSubTransactionId();
	ld->cxt = cxt;
	ld->refused = RecoveryInProgress() || sift_tbl_bulk_refusal(rel) != NULL;
	MemoryContextSwitchTo(TopMemoryContext);
	loads = lappend(loads, ld);
	if (!ld->refused)
	{
		MemoryContextSwitchTo(cxt);
		ld->vers = !frozen;
		l = layout_of(rel, st);
		ld->desc = RelationGetDescr(rel);
		ld->sw = (SealWriter) {rel, l, layout_policy(rel, l), cxt, NULL, NULL, NULL,
			(uint64) LOAD_SBS * sift_tbl_write_buffer_limit * 1024, tbl_col_format(ld->vers)};
		for (int i = 0; i < LOAD_SBS; i++)
			ld->batch[i] = sift_cbatch_create(cxt, l->schema);
		ld->rowcx = AllocSetContextCreate(cxt, "sift_table bulk load row", ALLOCSET_SMALL_SIZES);
	}
	MemoryContextSwitchTo(old);
	return !ld->refused;
}

void
sift_tbl_load_row(Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid)
{
	Load	   *ld = find_load(RelationGetRelid(rel));
	MemoryContext old;
	SiftColRow	row;
	uint64		unit;
	Size		hoff = offsetof(MinimalTupleData, t_bits);

	ld->sw.rel = rel;
	ld->sw.l = layout_of(rel, st);
	if (!ld->open)
		reserve_run(ld, rel, st);
	else if (ld->n >= LOAD_SB_ROWS || ld->bytes >= (Size) sift_tbl_write_buffer_limit * 1024)
	{
		end_unit(ld);
		if (ld->units == LOAD_SBS)
		{
			end_run(ld, rel, st);
			reserve_run(ld, rel, st);
		}
	}
	unit = ld->run + (uint64) ld->units * SIFT_TBL_VBLOCKS_SB;
	ItemPointerSet(tid, unit + ld->n / SIFT_TBL_MAX_OFF, ld->n % SIFT_TBL_MAX_OFF + 1);
	old = MemoryContextSwitchTo(ld->rowcx);
	row_in(ld->sw.l, ld->desc, tid, values, isnull, &row);
	if (ld->vers)
		sift_cbatch_add_ver(ld->batch[ld->units], &row, GetCurrentTransactionId(), cid);
	else
		sift_cbatch_add(ld->batch[ld->units], &row);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ld->rowcx);
	ld->n++;
	/* the bytes the row has as a minimal tuple (the write buffer limit counts them) */
	for (int a = 0; a < ld->desc->natts; a++)
		if (isnull[a])
		{
			hoff += BITMAPLEN(ld->desc->natts);
			break;
		}
	ld->bytes += MAXALIGN(hoff) + heap_compute_data_size(ld->desc, (Datum *) values, (bool *) isnull);
}

void
sift_tbl_load_finish(Relation rel)
{
	Load	   *ld = find_load(RelationGetRelid(rel));

	if (ld == NULL)
		return;
	PG_TRY();
	{
		if (!ld->refused)
			flush_load(ld, rel);
	}
	PG_FINALLY();
	{
		drop_load(ld);
	}
	PG_END_TRY();
}
