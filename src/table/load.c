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
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "optimizer/paths.h"
#include "port/atomics.h"
#include "utils/wait_event.h"
#include "storage/condition_variable.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/am.h"
#include "col/seg.h"
#include "core/page.h"
#include "core/parbuild.h"
#include "table/table.h"
#include "table/seg_int.h"

bool		sift_tbl_bulk_groups = true;
bool		sift_tbl_load_from_copy = false;
#ifdef SIFT_TESTING
int			sift_test_load_publish = 0;
#endif


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

/* Does the expression call a function that must not run in parallel mode (nextval is one)? */
static bool
calls_parallel_unsafe(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && func_parallel(((FuncExpr *) node)->funcid) == PROPARALLEL_UNSAFE)
		return true;
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		set_opfuncid(op);
		if (func_parallel(op->opfuncid) == PROPARALLEL_UNSAFE)
			return true;
	}
	return expression_tree_walker(node, calls_parallel_unsafe, context);
}

/*
 * The backend that reads the rows of a COPY is in parallel mode while the workers of the ring write, so what it runs for each row must not start a
 * command, a subtransaction or a new xid: a default, a generated column or an identity column that calls nextval, a trigger (the checks of a
 * foreign key are triggers), an index that is not a plain b-tree (the maintenance of a sift index merges in a subtransaction, which failed here and
 * left the index half written). Such a table has its groups written one run at a time, without the ring.
 */
static bool
row_work_unsafe_in_parallel_mode(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	TupleConstr *constr = desc->constr;

	if (rel->rd_rel->relhastriggers)
		return true;
	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		Relation	index = index_open(indexoid, NoLock);
		bool		plain = index->rd_rel->relam == BTREE_AM_OID && RelationGetIndexExpressions(index) == NIL && RelationGetIndexPredicate(index) == NIL;

		index_close(index, NoLock);
		if (!plain)
			return true;
	}
	for (int i = 0; i < desc->natts; i++)
		if (TupleDescAttr(desc, i)->attidentity != '\0')
			return true;
	if (constr == NULL)
		return false;
	for (int i = 0; i < constr->num_defval; i++)
		if (calls_parallel_unsafe(stringToNode(constr->defval[i].adbin), NULL))
			return true;
	for (int i = 0; i < constr->num_check; i++)
		if (calls_parallel_unsafe(stringToNode(constr->check[i].ccbin), NULL))
			return true;
	return false;
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
#define LOAD_RING_UNITS	4
/*
 * The ring writes its groups without the lock of the active segment, so the writers of the table can take the room they need in the directory before
 * the segments are published: the ring runs while the directory has this many entries free besides the segments it holds, else the load goes on in the
 * way that keeps the lock for the groups of a run
 */
#define LOAD_RING_ROOM	32
#define ring_room(st, held)		((st)->dir.nsegs + (held) + LOAD_RING_ROOM <= sift_tbl_max_segments - 2)
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
	bool		ring_ok;		/* the rows come from COPY: the workers may outlive a run */
	bool		noring;
	struct LoadRingLeader *rl;	/* the ring of the workers that write the groups while the command reads on */
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

static void ring_take_slot(Load *ld, Relation rel);


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

/* Does the load of this table hand its groups to workers that stay for the command? */
static bool
ring_wanted(Load *ld, Relation rel)
{
	return ld->ring_ok && !ld->noring && load_groups(rel) > 1 && ring_room(sift_tbl_rel(rel), 0);
}

/* The next run of superblocks, taken under the lock that serializes the write buffers' own reservations */
static void
reserve_run(Load *ld, Relation rel, SiftTblRel *st)
{
	uint64		end;

	ld->maxunits = ld->rl || ring_wanted(ld, rel) ? LOAD_RING_UNITS : LOAD_SBS * load_groups(rel);
	if (ld->rl)
		ring_take_slot(ld, rel);
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
	if (!ld->rl)
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

/*
 * The lock of the active segment, held with room in the directory for n segments or, when the room stays short (rows of running transactions fill it),
 * for the fewest the directory has: returns how many segments may be published under this hold, at least one (room made by settling, as often as
 * writers that waited for it take it)
 */
static int
lock_with_room(Relation rel, SiftTblRel *st, int n)
{
	int			room;

	/* the room made is taken by the writers that were waiting for it before the lock is: settle again, and take what there is only when it stays short */
	for (int tries = 0;; tries++)
	{
		make_room(rel, st);
		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		room = sift_tbl_max_segments - 2 - st->dir.nsegs;
		if (room >= n || tries == LOAD_ROOM_TRIES)
			break;
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	if (room < 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
				 errdetail("The groups of a load cannot be merged as fast as it writes them: rows of running transactions are in the way."),
				 errhint("Run VACUUM on the table, or end the transaction that wrote them.")));
	return Min(n, room);
}

/* The segments of groups already written (the caller keeps the orphan sweep from them), published in as few changes as the room of the directory allows */
static void
publish_all(Relation rel, SiftTblRel *st, Published *pub)
{
	for (int done = 0; done < pub->n;)
	{
		Published	part = {pub->segs + done, lock_with_room(rel, st, pub->n - done)};

		sift_dir_update(rel, &(SiftDirChange) {.fn = publish_groups,.arg = &part,.extra = InvalidBuffer});
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		done += part.n;
		sift_tbl_refresh(rel, st);
		if (sift_tbl_inline_promote)
			(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
	}
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

#ifdef SIFT_TESTING
/* the phase a participant waits in for the advisory lock of sift.test_phase_gate + this, so that a test finds a worker that is writing a group */
#define SIFT_TBL_PHASE_LOAD	4
#endif

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

#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_LOAD);
#endif
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

/*
 * The ring: the workers of a COPY that stay for the command. A run of one group (LOAD_SBS units) is written by the backend straight into a slot
 * of the ring in shared memory; when the group is full the slot is handed over and a worker writes its segment while the backend goes on
 * reading the input. A slot is claimed group by group with one atomic word (sequence, groups, next group): a late claim of an earlier use of the
 * slot fails on the sequence.
 *
 * The blocks a group takes are in no directory until its segment is published, so for as long as the ring runs the backend holds
 * SIFT_LOAD_LOCK_BLKNO in ShareLock, which the orphan sweep of a VACUUM only tries for (and leaves the sweep for the next VACUUM when it is
 * not free). The segments are published in order every LOAD_PUBLISH groups and at the end, each time in one change of the directory, under
 * the lock of the active segment only for that change: the load blocks neither the writers of the table nor its maintenance.
 */
#define RING_SLOTS_MAX	8
#ifdef SIFT_TESTING
/* the phases the leader of the ring waits in for the advisory lock of sift.test_phase_gate + this: before it publishes segments, and before it publishes the last ones */
#define SIFT_TBL_PHASE_PUBLISH	5
#define SIFT_TBL_PHASE_STOP		6
#define load_publish()			(sift_test_load_publish > 0 ? sift_test_load_publish : LOAD_PUBLISH)
#else
#define load_publish()			LOAD_PUBLISH
#endif
#define LOAD_PUBLISH	8		/* segments from which the backend publishes the ones written */

typedef struct LoadSlot
{
	pg_atomic_uint64 claim;		/* seq << 16 | ngroups << 8 | next group; next == ngroups: nothing to take */
	pg_atomic_uint32 done;		/* groups written */
	LoadRun		run;
	SiftSegEntry segs[LOAD_MAX_GROUPS];
} LoadSlot;

typedef struct LoadRing
{
	SiftParCommon common;
	ConditionVariable cv;		/* a group is ready, or a group is written */
	pg_atomic_uint32 closed;	/* the command has no more groups */
	int			nslots;
	Size		cap;			/* bytes of the rows of a slot */
	Size		data_off;
	LoadSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} LoadRing;

typedef struct LoadRingLeader
{
	SiftParLeader *ldr;
	LoadRing   *rg;
	int			participants;
	uint64		head;			/* slots handed over; the next to fill is head % nslots */
	uint64		tail;			/* slots whose segments are taken */
	uint32		seq;
	int			nsegs;
	SiftSegEntry *segs;			/* the segments of the groups taken and not yet published, in the order of their superblocks */
	int			maxsegs;
} LoadRingLeader;

static char *
slot_rows(LoadRing *rg, int i)
{
	return (char *) rg + rg->data_off + (Size) i * rg->cap;
}

/* The next group nobody has taken, or NULL */
static LoadSlot *
ring_claim(LoadRing *rg, int *g)
{
	for (int i = 0; i < rg->nslots; i++)
	{
		LoadSlot   *sl = &rg->slots[i];
		uint64		c = pg_atomic_read_u64(&sl->claim);

		while ((c & 0xff) < ((c >> 8) & 0xff))
			if (pg_atomic_compare_exchange_u64(&sl->claim, &c, c + 1))
			{
				*g = (int) (c & 0xff);
				return sl;
			}
	}
	return NULL;
}

static void
ring_write(LoadRing *rg, LoadSlot *sl, int g, Relation rel)
{
	int			i = (int) (sl - rg->slots);

	write_group(&sl->run, slot_rows(rg, i), rel, g, &sl->segs[g]);
	if (pg_atomic_add_fetch_u32(&sl->done, 1) == ((pg_atomic_read_u64(&sl->claim) >> 8) & 0xff))
		ConditionVariableBroadcast(&rg->cv);
}

PGDLLEXPORT void sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc);

void
sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	LoadRing   *rg;

	sift_par_worker_begin(&w, toc);
	rg = w.shared;
	for (;;)
	{
		LoadSlot   *sl = NULL;
		int			g = 0;

		ConditionVariablePrepareToSleep(&rg->cv);
		while ((sl = ring_claim(rg, &g)) == NULL && !pg_atomic_read_u32(&rg->closed))
			ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
		ConditionVariableCancelSleep();
		if (sl == NULL)
			break;
		ring_write(rg, sl, g, w.heap);
	}
	sift_par_worker_end(&w);
}

static bool
slot_done(LoadSlot *sl)
{
	uint64		c = pg_atomic_read_u64(&sl->claim);

	return pg_atomic_read_u32(&sl->done) == ((c >> 8) & 0xff);
}

/* Takes the segments of the oldest slot once it is written (waits for it, writing groups meanwhile, when wait); false if it is not */
static bool
ring_reap(Load *ld, Relation rel, bool wait)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->tail % rg->nslots];
	int			g;
	int			ng;

	if (!wait && !slot_done(sl))
		return false;
	ConditionVariablePrepareToSleep(&rg->cv);
	while (!slot_done(sl))
	{
		LoadSlot   *other = ring_claim(rg, &g);

		if (other != NULL)
		{
			ConditionVariableCancelSleep();
			ring_write(rg, other, g, rel);
			ConditionVariablePrepareToSleep(&rg->cv);
			continue;
		}
		ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	pg_read_barrier();
	ng = (int) ((pg_atomic_read_u64(&sl->claim) >> 8) & 0xff);
	if (rl->nsegs + ng > rl->maxsegs)
	{
		rl->maxsegs = Max(rl->maxsegs * 2, rl->nsegs + ng);
		rl->segs = repalloc(rl->segs, rl->maxsegs * sizeof(SiftSegEntry));
	}
	memcpy(rl->segs + rl->nsegs, sl->segs, ng * sizeof(SiftSegEntry));
	rl->nsegs += ng;
	rl->tail++;
	return true;
}

/* The slot the backend fills next: waits for a free one */
static void
ring_take_slot(Load *ld, Relation rel)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;

	while (rl->head - rl->tail >= (uint64) rg->nslots)
		(void) ring_reap(ld, rel, true);
	ld->rows.data = slot_rows(rg, (int) (rl->head % rg->nslots));
	ld->rows.maxlen = (int) Min(rg->cap, (Size) INT_MAX);
	ld->rows.len = 0;
}

/* The segments taken become part of the directory in one change; false, with nothing published, when it has no room for them (the ring is then stopped) */
static bool
ring_publish(Load *ld, Relation rel, SiftTblRel *st)
{
	LoadRingLeader *rl = ld->rl;
	Published	pub = {rl->segs, rl->nsegs};

	if (rl->nsegs == 0)
		return true;
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_PUBLISH);
#endif
	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	if (st->dir.nsegs + pub.n > sift_tbl_max_segments - 2 || dir_backpressure(st))
	{
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		return false;
	}
	sift_dir_update(rel, &(SiftDirChange) {.fn = publish_groups,.arg = &pub,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	rl->nsegs = 0;
	return true;
}

static void ring_stop(Load *ld, Relation rel, SiftTblRel *st);

/* The group the backend filled is handed over to the workers; last: the groups of the rest of the load are shared out between the participants */
static void
ring_hand_over(Load *ld, Relation rel, SiftTblRel *st, bool last)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->head % rg->nslots];
	int			ngroups = 1;

	sl->run = ld->run;
	sl->run.xid = sl->run.vers ? GetCurrentTransactionId() : InvalidTransactionId;
	sl->run.per = LOAD_RING_UNITS;
	if (last)
		ngroups = share_units(&sl->run, ld->rows.len, rl->participants);
	pg_atomic_write_u32(&sl->done, 0);
	pg_write_barrier();
	pg_atomic_write_u64(&sl->claim, ((uint64) ++rl->seq << 16) | ((uint64) ngroups << 8));
	rl->head++;
	ld->open = false;
	ConditionVariableBroadcast(&rg->cv);
	if (last)
		return;
	while (ring_reap(ld, rel, false))
		;
	sift_dir_read(rel, &st->dir);
	if (!ring_room(st, rl->nsegs + (int) (rl->head - rl->tail)) || (rl->nsegs >= load_publish() && !ring_publish(ld, rel, st)))
	{
		/* the directory has little room: the rest of the load goes the way of a load without workers, which makes room between its runs */
		ld->noring = true;
		ring_stop(ld, rel, st);
	}
}

/*
 * Starts the workers and hands them the first group of the load (the rows of the run already read): false, with nothing changed, when they cannot
 * be had.
 */
static bool
ring_start(Load *ld, Relation rel, SiftTblRel *st)
{
	int			nworkers = Min(max_parallel_maintenance_workers, LOAD_MAX_GROUPS - 1);
	Size		cap = (Size) LOAD_RING_UNITS * sift_tbl_write_buffer_limit * 1024 / 4 * 5;
	int			nslots = Max(2, Min(Min(nworkers + 1, RING_SLOTS_MAX), (int) (((Size) maintenance_work_mem * 1024) / cap)));
	Size		head = MAXALIGN(offsetof(LoadRing, slots) + nslots * sizeof(LoadSlot));
	SiftParLeader *ldr;
	LoadRingLeader *rl;
	LoadRing   *rg;
	MemoryContext old;

	sift_dir_read(rel, &st->dir);
	if (!ring_room(st, 0) || !ConditionalLockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock))
		return false;
	(void) GetCurrentTransactionId();
	old = MemoryContextSwitchTo(ld->cxt);
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_tbl_load_ring_worker", nworkers, head + nslots * cap, sizeof(uint64), false))
	{
		MemoryContextSwitchTo(old);
		UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
		return false;
	}
	rg = ldr->shared;
	rg->common = (SiftParCommon) {RelationGetRelid(rel), InvalidOid, false};
	ConditionVariableInit(&rg->cv);
	pg_atomic_init_u32(&rg->closed, 0);
	rg->nslots = nslots;
	rg->cap = cap;
	rg->data_off = head;
	for (int i = 0; i < nslots; i++)
	{
		pg_atomic_init_u64(&rg->slots[i].claim, 0);
		pg_atomic_init_u32(&rg->slots[i].done, 0);
	}
	LaunchParallelWorkers(ldr->pcxt);
#ifdef SIFT_TESTING
	sift_test_par_workers = ldr->pcxt->nworkers_launched;
#endif
	if (ldr->pcxt->nworkers_launched == 0)
	{
		sift_par_end(ldr);
		MemoryContextSwitchTo(old);
		UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
		return false;
	}
	rl = palloc0(sizeof(LoadRingLeader));
	rl->ldr = ldr;
	rl->rg = rg;
	rl->participants = 1 + ldr->pcxt->nworkers_launched;
	rl->maxsegs = 16;
	rl->segs = palloc(rl->maxsegs * sizeof(SiftSegEntry));
	MemoryContextSwitchTo(old);
	/* the rows already read are the first group */
	memcpy(slot_rows(rg, 0), ld->rows.data, ld->rows.len);
	pfree(ld->rows.data);
	ld->rows.data = slot_rows(rg, 0);
	ld->rows.maxlen = (int) Min(cap, (Size) INT_MAX);
	ld->rl = rl;
	ring_hand_over(ld, rel, st, false);
	return true;
}

/*
 * The end of the ring, at the end of the load or when the rest of it goes without the workers: the rows of the open run are shared out, the backend
 * writes with the workers, the workers end, and the segments not yet published are, with the room the directory needs made when it has none
 */
static void
ring_stop(Load *ld, Relation rel, SiftTblRel *st)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	int			g;
	LoadSlot   *sl;
	Published	pub;

	if (ld->open && (ld->n > 0 || ld->run.units > 0))
	{
		if (ld->n > 0)
			end_unit(ld);
		if (ld->run.units > 0)
			ring_hand_over(ld, rel, st, true);
	}
	ld->open = false;
	pg_atomic_write_u32(&rg->closed, 1);
	ConditionVariableBroadcast(&rg->cv);
	while ((sl = ring_claim(rg, &g)) != NULL)
		ring_write(rg, sl, g, rel);
	while (rl->tail < rl->head)
		(void) ring_reap(ld, rel, true);
	sift_par_wait(rl->ldr);
	sift_par_end(rl->ldr);
	ld->rl = NULL;
	{
		MemoryContext old = MemoryContextSwitchTo(ld->cxt);

		initStringInfo(&ld->rows);
		MemoryContextSwitchTo(old);
	}
	pub = (Published) {rl->segs, rl->nsegs};
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_STOP);
#endif
	if (pub.n > 0)
		publish_all(rel, st, &pub);
	UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
}

/* The groups of the run are written and become segments in the directory. parallel: the command is still running, so a parallel context may be used */
static void
end_run(Load *ld, Relation rel, SiftTblRel *st, bool parallel, bool more)
{
	LoadRun    *r = &ld->run;
	int			participants = parallel ? load_groups(rel) : 1;
	int			ngroups;
	int			room;
	SiftSegEntry segs[LOAD_MAX_GROUPS];
	Published	pub = {segs};

	if (ld->rl)
	{
		ring_hand_over(ld, rel, st, false);
		return;
	}
	ld->open = false;
	if (r->units == 0)
		return;
	if (more && parallel && ring_wanted(ld, rel) && participants > 1 && r->units == LOAD_RING_UNITS && ld->rows.len >= LOAD_PARALLEL_BYTES * LOAD_RING_UNITS / LOAD_SBS)
	{
		ld->open = true;
		if (ring_start(ld, rel, st))
			return;
		ld->open = false;
		ld->noring = true;
	}
	pub.n = ngroups = share_units(r, ld->rows.len, participants);
	room = lock_with_room(rel, st, ngroups);
	r->xid = r->vers ? GetCurrentTransactionId() : InvalidTransactionId;
	if (room >= ngroups)
	{
		if (!(participants > 1 && ngroups > 1 && write_parallel(r, ld->rows.data, rel, ngroups, segs)))
			for (int g = 0; g < ngroups; g++)
				write_group(r, ld->rows.data, rel, g, &segs[g]);
		sift_dir_update(rel, &(SiftDirChange) {.fn = publish_groups,.arg = &pub,.extra = InvalidBuffer});
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	else
	{
		/* less room than groups: each is written and published under its own hold of the lock, the way a load without workers goes */
		for (int g = 0; g < ngroups; g++)
		{
			Published	one = {&segs[g], 1};

			if (g > 0)
				(void) lock_with_room(rel, st, 1);
			write_group(r, ld->rows.data, rel, g, &segs[g]);
			sift_dir_update(rel, &(SiftDirChange) {.fn = publish_groups,.arg = &one,.extra = InvalidBuffer});
			UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		}
	}
	sift_tbl_refresh(rel, st);
	if (sift_tbl_inline_promote)
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
}

/* Writes what the load holds: the rows of the current unit, and the run they end */
static void
flush_load(Load *ld, Relation rel, bool parallel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (ld->rl)
	{
		ring_stop(ld, rel, st);
		return;
	}
	if (!ld->open)
		return;
	if (ld->n > 0)
		end_unit(ld);
	end_run(ld, rel, st, parallel, false);
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
	sift_tbl_load_from_copy = false;
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
		ld->ring_ok = sift_tbl_load_from_copy && !row_work_unsafe_in_parallel_mode(rel);
		(void) layout_of(rel, st);
		ld->desc = RelationGetDescr(rel);
		ld->flat = palloc(ld->desc->natts * sizeof(Datum));
		initStringInfo(&ld->rows);
	}
	MemoryContextSwitchTo(old);
	return !ld->refused;
}

/* The row as a record at the end of the buffer of the run; false, with nothing written, when the buffer is a slot of the ring and the row does not fit */
static bool
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
	{
		if (ld->rl)
			return false;
		enlargeStringInfo(&ld->rows, MAXALIGN(len));
	}
	rec = (LoadRec *) (ld->rows.data + ld->rows.len);
	rec->len = len;
	rec->cid = cid;
	th = (HeapTupleHeader) (rec + 1);
	memset(th, 0, len - sizeof(LoadRec));	/* the padding must be zero for heap_deform_tuple to find the next value */
	HeapTupleHeaderSetNatts(th, desc->natts);
	th->t_hoff = hoff;
	heap_fill_tuple(desc, vals, isnull, (char *) th + hoff, len - sizeof(LoadRec) - hoff, &th->t_infomask, hasnull ? th->t_bits : NULL);
	ld->rows.len += MAXALIGN(len);
	return true;
}

void
sift_tbl_load_row(Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid)
{
	Load	   *ld = find_load(RelationGetRelid(rel));
	Size		hoff = offsetof(MinimalTupleData, t_bits);
	Size		datasize = heap_compute_data_size(ld->desc, (Datum *) values, (bool *) isnull);

	for (;;)
	{
		if (!ld->open)
			reserve_run(ld, rel, st);
		else if (ld->n >= LOAD_SB_ROWS || ld->bytes >= (Size) sift_tbl_write_buffer_limit * 1024)
		{
			end_unit(ld);
			if (ld->run.units == ld->maxunits)
			{
				end_run(ld, rel, st, true, true);
				reserve_run(ld, rel, st);
			}
		}
		if (put_row(ld, values, isnull, cid, datasize))
			break;
		/* the slot of the ring is full: the group is handed over with the units it has, and a row that no slot holds goes the way of a load without workers */
		if (ld->n == 0 && ld->run.units == 0)
		{
			ld->noring = true;
			ring_stop(ld, rel, st);
		}
		else
		{
			if (ld->n > 0)
				end_unit(ld);
			end_run(ld, rel, st, true, true);
		}
	}
	ItemPointerSet(tid, ld->run.first + (uint64) ld->run.units * SIFT_TBL_VBLOCKS_SB + ld->n / SIFT_TBL_MAX_OFF, ld->n % SIFT_TBL_MAX_OFF + 1);
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
