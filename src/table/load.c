/*
 * table/load.c - the rows of a bulk load (COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW) written straight into column groups
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/genam.h"
#include "access/transam.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "optimizer/paths.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/seg.h"
#include "core/page.h"
#include "core/parbuild.h"
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
 * the units of a group become one segment. The rows wait in the backend as records in a buffer (a write buffer's bytes per unit) until a run is
 * full, then the segments of its groups are written and published in one go, under the lock that serializes the creations of segments (what
 * the orphan sweep of a VACUUM waits for: the blocks written are in no directory until then), so a load adds a directory entry every few
 * hundred thousand rows and the merges of the writers keep the directory as they do for the groups of the promotions. A run is one group,
 * or one for each participant of a parallel context (the backend and the workers max_parallel_maintenance_workers allows) that write the
 * groups at the same time: the transposition of the rows and the encoding are most of what a load costs besides the parse of its input.
 * The rows past the last full run are written when the command ends (finish_bulk_insert), or at the latest before the transaction commits,
 * so a row never outlives its transaction in memory.
 */
#define LOAD_SBS		8
#define LOAD_SB_ROWS	((uint64) SIFT_TBL_VBLOCKS_SB * SIFT_TBL_MAX_OFF)
/* The backend and three workers: the buffer of a run is LOAD_MAX_GROUPS * LOAD_SBS write buffers */
#define LOAD_MAX_GROUPS	4
#define LOAD_MAX_UNITS	(LOAD_SBS * LOAD_MAX_GROUPS)
/*
 * A run is shared between the participants of a parallel context from this many bytes of rows (the start of the workers, about 15 ms, then
 * costs a fifth of the encoding they take over), and every one gets two units at least
 */
#define LOAD_PARALLEL_BYTES	((Size) 16 << 20)
#define LOAD_MIN_UNITS	2

/* A row in the buffer of a run: this header, then the tuple in heap format (the fields of the transaction unset), each record at a MAXALIGN */
typedef struct LoadRec
{
	uint32		len;			/* of the header and the tuple */
	CommandId	cid;
} LoadRec;

/* A run as the writers of its groups read it: group g is the units from g * per, unit u is urows[u] records from uoff[u] of the buffer of the rows, of the superblock of vblock first + u * SIFT_TBL_VBLOCKS_SB */
typedef struct LoadRun
{
	SiftParCommon common;		/* in shared memory: the table */
	bool		vers;			/* the rows carry their xmin (xid) and cmin; false: frozen */
	TransactionId xid;
	uint64		first;
	int			units;
	int			per;			/* units of a group: LOAD_SBS, fewer when the units are shared out between the participants */
	uint32		urows[LOAD_MAX_UNITS];
	Size		uoff[LOAD_MAX_UNITS + 1];
} LoadRun;

typedef struct Load
{
	Oid			relid;
	SubTransactionId subid;		/* the (sub)transaction whose xid the rows carry */
	bool		refused;		/* the table keeps the write buffers (sift_tbl_bulk_refusal) */
	bool		open;			/* a run of superblocks is reserved */
	MemoryContext cxt;
	TupleDesc	desc;
	Datum	   *flat;			/* the values of a row with its external values read in */
	StringInfoData rows;		/* the records of the run */
	LoadRun		run;
	int			maxunits;		/* units of a run: LOAD_SBS for each group written at once */
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

/* The groups a run holds: a parallel context writes one for each participant (the rows of a run wait in the backend: they fit maintenance_work_mem), the backend alone one */
static int
load_groups(Relation rel)
{
	if (!IsUnderPostmaster || IsInParallelMode() || !ActiveSnapshotSet() || RelationUsesLocalBuffers(rel))
		return 1;
	return Max(1, Min(Min(1 + max_parallel_maintenance_workers, LOAD_MAX_GROUPS), maintenance_work_mem / (LOAD_SBS * sift_tbl_write_buffer_limit)));
}

/* The next run of superblocks, taken under the lock that serializes the write buffers' own reservations */
static void
reserve_run(Load *ld, Relation rel, SiftTblRel *st)
{
	uint64		end;

	ld->maxunits = LOAD_SBS * load_groups(rel);
	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	ld->run.first = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, Max(SIFT_TBL_NEXT_VBLOCK(&st->dir.meta), (uint64) sift_tbl_vblock_floor));
	end = ld->run.first + (uint64) ld->maxunits * SIFT_TBL_VBLOCKS_SB;
	if (end > (uint64) MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" exhausted its TIDs", RelationGetRelationName(rel)),
				 errhint("Run VACUUM FULL on the table.")));
	sift_dir_update(rel, &(SiftDirChange) {.fn = reserve_vblocks,.arg = &end,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	ld->open = true;
	ld->run.units = 0;
	ld->run.uoff[0] = 0;
	ld->n = 0;
	ld->bytes = 0;
	resetStringInfo(&ld->rows);
}

/* The segments of the groups of a run, in the order of their superblocks */
typedef struct Published
{
	const SiftSegEntry *segs;
	int			n;
} Published;

static void
publish_groups(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const Published *p = arg;

	for (int i = 0; i < p->n; i++)
	{
		dir->segs[dir->nsegs++] = p->segs[i];
		meta->ndocs += p->segs[i].ndocs;
		meta->sum_dl += p->segs[i].sum_dl;
	}
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
	LoadRun    *r = &ld->run;

	r->urows[r->units] = ld->n;
	r->uoff[++r->units] = ld->rows.len;
	ld->n = 0;
	ld->bytes = 0;
}

/* The units of group g of the run as one segment: the rows of a unit are one batch, written as the unit of its superblock */
static void
write_group(const LoadRun *r, const char *rows, Relation rel, int g, SiftSegEntry *seg)
{
	const SiftTblLayout *l = layout_of(rel, sift_tbl_rel(rel));
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table load group", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table load row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL,
		(uint64) LOAD_SBS * sift_tbl_write_buffer_limit * 1024, tbl_col_format(r->vers)};
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));

	for (int u = g * r->per; u < Min(r->units, (g + 1) * r->per); u++)
	{
		uint64		vblock = r->first + (uint64) u * SIFT_TBL_VBLOCKS_SB;
		const char *p = rows + r->uoff[u];

		CHECK_FOR_INTERRUPTS();
		sift_cbatch_reset(sw.batch, vblock / SIFT_SB_PAGES);
		for (uint32 n = 0; n < r->urows[u]; n++)
		{
			const LoadRec *rec = (const LoadRec *) p;
			HeapTupleData tup = {.t_len = rec->len - sizeof(LoadRec),.t_data = (HeapTupleHeader) (p + sizeof(LoadRec))};
			ItemPointerData tid;
			SiftColRow	row;

			ItemPointerSet(&tid, vblock + n / SIFT_TBL_MAX_OFF, n % SIFT_TBL_MAX_OFF + 1);
			MemoryContextSwitchTo(rowcx);
			heap_deform_tuple(&tup, desc, values, isnull);
			row_in(l, desc, &tid, values, isnull, &row);
			if (r->vers)
				sift_cbatch_add_ver(sw.batch, &row, r->xid, rec->cid);
			else
				sift_cbatch_add(sw.batch, &row);
			MemoryContextSwitchTo(cx);
			MemoryContextReset(rowcx);
			p += MAXALIGN(rec->len);
		}
		seal_flush(&sw);
	}
	sift_colw_finish(sw.w, seg);
	sift_wt_finish(sw.wt);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* What the participants of a parallel write share: the run with the buffer of its rows after it, and the next group nobody has taken */
typedef struct LoadShared
{
	LoadRun		run;
	int			ngroups;
	pg_atomic_uint32 next;
} LoadShared;

static void
take_groups(LoadShared *sh, SiftSegEntry *segs, Relation rel)
{
	uint32		g;

	while ((g = pg_atomic_fetch_add_u32(&sh->next, 1)) < sh->ngroups)
		write_group(&sh->run, (const char *) (sh + 1), rel, g, &segs[g]);
}

/* False, with nothing written, when no worker could be had: the backend then writes the groups alone */
static bool
write_parallel(const LoadRun *r, const char *rows, Relation rel, int ngroups, SiftSegEntry *segs)
{
	int			nworkers = Min(ngroups - 1, max_parallel_maintenance_workers);
	Size		rowsz = r->uoff[r->units];
	SiftParLeader *ldr;
	LoadShared *sh;

	(void) GetCurrentTransactionId();	/* parallel mode assigns none */
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_tbl_load_worker", nworkers, sizeof(LoadShared) + rowsz, ngroups * sizeof(SiftSegEntry), false))
		return false;
	sh = ldr->shared;
	sh->run = *r;
	sh->run.common = (SiftParCommon) {RelationGetRelid(rel), InvalidOid, false};
	memcpy(sh + 1, rows, rowsz);
	sh->ngroups = ngroups;
	pg_atomic_init_u32(&sh->next, 0);
	if (!sift_par_launch(ldr, "sift_table", rel, nworkers))
		return false;
	take_groups(sh, ldr->parts, rel);
	sift_par_wait(ldr);
	memcpy(segs, ldr->parts, ngroups * sizeof(SiftSegEntry));
	sift_par_end(ldr);
	return true;
}

PGDLLEXPORT void sift_tbl_load_worker(dsm_segment *seg, shm_toc *toc);

void
sift_tbl_load_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;

	sift_par_worker_begin(&w, toc);
	take_groups(w.shared, w.parts, w.heap);
	sift_par_worker_end(&w);
}

/* The groups of a run: LOAD_SBS units each; a run the participants share is cut into groups of the same size, one for each of them */
static int
share_units(LoadRun *r, Size bytes, int participants)
{
	r->per = LOAD_SBS;
	if (participants > 1 && bytes >= LOAD_PARALLEL_BYTES)
	{
		int			groups = Max((r->units + LOAD_SBS - 1) / LOAD_SBS, Min(participants, r->units / LOAD_MIN_UNITS));

		r->per = (r->units + groups - 1) / groups;
	}
	return (r->units + r->per - 1) / r->per;
}

/* The groups of the run are written and become segments in the directory. parallel: the command is still running, so a parallel context may be used */
static void
end_run(Load *ld, Relation rel, SiftTblRel *st, bool parallel)
{
	LoadRun    *r = &ld->run;
	int			participants = parallel ? load_groups(rel) : 1;
	int			ngroups;
	SiftSegEntry segs[LOAD_MAX_GROUPS];
	Published	pub = {segs};

	ld->open = false;
	if (r->units == 0)
		return;
	pub.n = ngroups = share_units(r, ld->rows.len, participants);
	/* the room made is taken by the writers that were waiting for it before the lock is: settle again, and give up only when it stays full */
	for (int tries = 0;; tries++)
	{
		make_room(rel, st);
		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		if (st->dir.nsegs + ngroups <= sift_tbl_max_segments - 2 || tries == LOAD_ROOM_TRIES)
			break;
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	if (st->dir.nsegs + ngroups > sift_tbl_max_segments - 2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
				 errdetail("The groups of a load cannot be merged as fast as it writes them: rows of running transactions are in the way."),
				 errhint("Run VACUUM on the table, or end the transaction that wrote them.")));
	r->xid = r->vers ? GetCurrentTransactionId() : InvalidTransactionId;
	if (!(participants > 1 && ngroups > 1 && write_parallel(r, ld->rows.data, rel, ngroups, segs)))
		for (int g = 0; g < ngroups; g++)
			write_group(r, ld->rows.data, rel, g, &segs[g]);
	sift_dir_update(rel, &(SiftDirChange) {.fn = publish_groups,.arg = &pub,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_tbl_refresh(rel, st);
	if (sift_tbl_inline_promote)
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
}

/* Writes what the load holds: the rows of the current unit, and the run they end */
static void
flush_load(Load *ld, Relation rel, bool parallel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (!ld->open)
		return;
	if (ld->n > 0)
		end_unit(ld);
	end_run(ld, rel, st, parallel);
}

static void
flush_and_drop(Load *ld)
{
	Relation	rel = try_table_open(ld->relid, RowExclusiveLock);

	if (rel != NULL)
	{
		if (!ld->refused)
			flush_load(ld, rel, false);
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
		ld->run.vers = !frozen;
		(void) layout_of(rel, st);
		ld->desc = RelationGetDescr(rel);
		ld->flat = palloc(ld->desc->natts * sizeof(Datum));
		initStringInfo(&ld->rows);
	}
	MemoryContextSwitchTo(old);
	return !ld->refused;
}

/* The row as a record at the end of the buffer of the run */
static void
put_row(Load *ld, const Datum *values, const bool *isnull, CommandId cid, Size datasize)
{
	TupleDesc	desc = ld->desc;
	const Datum *vals = values;
	bool		hasnull = false;
	Size		hoff,
				len;
	LoadRec    *rec;
	HeapTupleHeader th;

	for (int a = 0; a < desc->natts; a++)
	{
		if (isnull[a])
			hasnull = true;
		else if (TupleDescAttr(desc, a)->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(values[a])) &&
				 !VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(values[a])))
		{
			/* the workers have no snapshot to read a toasted value with */
			if (vals == values)
			{
				memcpy(ld->flat, values, desc->natts * sizeof(Datum));
				vals = ld->flat;
			}
			ld->flat[a] = PointerGetDatum(detoast_attr((struct varlena *) DatumGetPointer(values[a])));
		}
	}
	hoff = MAXALIGN(SizeofHeapTupleHeader + (hasnull ? BITMAPLEN(desc->natts) : 0));
	if (vals != values)
		datasize = heap_compute_data_size(desc, (Datum *) vals, (bool *) isnull);
	len = sizeof(LoadRec) + hoff + datasize;
	if (ld->rows.maxlen - ld->rows.len <= MAXALIGN(len))
		enlargeStringInfo(&ld->rows, MAXALIGN(len));
	rec = (LoadRec *) (ld->rows.data + ld->rows.len);
	rec->len = len;
	rec->cid = cid;
	th = (HeapTupleHeader) (rec + 1);
	memset(th, 0, len - sizeof(LoadRec));	/* the padding must be zero for heap_deform_tuple to find the next value */
	HeapTupleHeaderSetNatts(th, desc->natts);
	th->t_hoff = hoff;
	heap_fill_tuple(desc, vals, isnull, (char *) th + hoff, len - sizeof(LoadRec) - hoff, &th->t_infomask, hasnull ? th->t_bits : NULL);
	ld->rows.len += MAXALIGN(len);
}

void
sift_tbl_load_row(Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid)
{
	Load	   *ld = find_load(RelationGetRelid(rel));
	Size		hoff = offsetof(MinimalTupleData, t_bits);
	Size		datasize = heap_compute_data_size(ld->desc, (Datum *) values, (bool *) isnull);

	if (!ld->open)
		reserve_run(ld, rel, st);
	else if (ld->n >= LOAD_SB_ROWS || ld->bytes >= (Size) sift_tbl_write_buffer_limit * 1024)
	{
		end_unit(ld);
		if (ld->run.units == ld->maxunits)
		{
			end_run(ld, rel, st, true);
			reserve_run(ld, rel, st);
		}
	}
	ItemPointerSet(tid, ld->run.first + (uint64) ld->run.units * SIFT_TBL_VBLOCKS_SB + ld->n / SIFT_TBL_MAX_OFF, ld->n % SIFT_TBL_MAX_OFF + 1);
	put_row(ld, values, isnull, cid, datasize);
	ld->n++;
	/* the bytes the row has as a minimal tuple (the write buffer limit counts them) */
	for (int a = 0; a < ld->desc->natts; a++)
		if (isnull[a])
		{
			hoff += BITMAPLEN(ld->desc->natts);
			break;
		}
	ld->bytes += MAXALIGN(hoff) + datasize;
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
			flush_load(ld, rel, ActiveSnapshotSet());
	}
	PG_FINALLY();
	{
		drop_load(ld);
	}
	PG_END_TRY();
}
