/*
 * col/am/am.c - the column access method: the handler, the insert of a row, the batch
 * of rows a statement collects and the row a statement makes
 * reloptions, the relcache and the caches are in the files next to it
 */
#include "postgres.h"
#include "access/relation.h"
#include "bm25/am.h"
#include "col/am/am_int.h"

int			sift_col_format_minor = SIFT_COL_FORMAT_MINOR;
#ifdef SIFT_TESTING
bool		sift_test_col_rel_inval = false;
bool		sift_test_col_net = true;
#endif

/*
 * The planner asks one index for its statistics, its zones and its pre-aggregates several times (the cost estimate alone three, the source another) and what the
 * first call validates, by reading the metapage and the directory under share locks, holds for the whole planning: the planner hook opens a planning
 * (sift_col_plan_begin) and an entry validated in it is returned as it is. A build or a maintenance call outside a planning always validates.
 */
uint64 am_leaf_plan;

void
sift_col_make_row_rc(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt,
					 struct SiftShredRecheck *rc)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	/* detoasting a value locks the toast table and can reload the relcache entry, which frees r when it is the cached state: what the loop reads is taken first
	 * (the schema and the arrays are apart in rd_indexcxt and stay) */
	const SiftColSchema *schema = r->schema;
	const int  *first = r->first,
			   *nleaves = r->nleaves;
	const bool *json = r->json,
			   *name = r->name;
	SiftColShredAdapter *shred = r->shred;
	int			natts = r->natts,
				ncols = schema->ncols;

	row->tid = *tid;
	row->ncols = ncols;
	row->val = palloc0(ncols * sizeof(SiftColValue));
	row->elem = palloc0(ncols * sizeof(SiftColValue *));
	for (int a = 1; a <= natts; a++)
	{
		const SiftColDef *def = &schema->cols[first[a]];

		if (json[a])
			continue;
		if (isnull[a - 1])
			for (int k = 0; k < nleaves[a]; k++)
				row->val[def->id + k].isnull = true;
		else
			sift_col_datum_values(def, values[a - 1], name[a], &row->val[def->id]);
	}
	if (shred)
		sift_col_shred_row_rc(shred, values, isnull, row, first, mcxt, rc);
	MemoryContextSwitchTo(old);
}

void
sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt)
{
	sift_col_make_row_rc(r, tid, values, isnull, row, mcxt, NULL);
}

/*
 * The rows of a transaction after its first go to the write buffer in runs (wbuf.c, sift_cwbuf_pend). They are written before the commit record and before
 * a savepoint starts, dropped when the transaction or the savepoint aborts: the heap rows of an aborted savepoint are dead at once and VACUUM may free their
 * slots while the transaction goes on, so an index entry written for them later would point at whatever takes the slot. An ON CONFLICT statement kills
 * the heap row of a conflict after the index entries went in, with no abort to drop them: its rows go in at once (batch_stmts).
 */
static bool batch_hooked;

static bool batch_seen;

static int	batch_stmts;

static int	batch_writes;		/* statements running that modify data (sift_col_writing) */

static bool batch_inserting;	/* inside sift_col_aminsert; an error leaves it set until the (sub)transaction aborts */

static int	batch_updates;		/* statements running that replace row versions (UPDATE, MERGE, ON CONFLICT DO UPDATE): an index with track_changes supersedes the entries of the old versions */

static void batch_xact(XactEvent event, void *arg);
static void batch_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);

/* The callbacks that write the held rows and tombstones at the commit: made by the first row of the backend, or by the first tombstone a row trigger holds */
void
sift_col_batch_hook(void)
{
	if (!batch_hooked)
	{
		RegisterXactCallback(batch_xact, NULL);
		RegisterSubXactCallback(batch_subxact, NULL);
		batch_hooked = true;
	}
}

static void
batch_flush_all(void)
{
	Oid			oid;

	sift_col_tombs_flush();
	while (sift_cwbuf_pending(&oid))
	{
		Relation	index = relation_open(oid, RowExclusiveLock);

		if (sift_cwbuf_flush(index))
			sift_col_maint_after_seal(index);
		relation_close(index, NoLock);
	}
}

static void
batch_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
		batch_flush_all();
	else if (event == XACT_EVENT_ABORT)
	{
		sift_cwbuf_discard(InvalidOid);
		sift_col_tombs_discard();
		batch_inserting = false;
	}
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
		batch_seen = false;
}

static void
batch_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_START_SUB)
		batch_flush_all();
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		sift_cwbuf_discard(InvalidOid);
		sift_col_tombs_discard();
		batch_inserting = false;
	}
}

#define STMT_SPECULATIVE	1
#define STMT_UPDATING		2
#define STMT_WRITING		4

static void
batch_stmt_done(void *arg)
{
	uintptr_t	what = (uintptr_t) arg;

	batch_stmts -= (what & STMT_SPECULATIVE) != 0;
	batch_updates -= (what & STMT_UPDATING) != 0;
	batch_writes -= (what & STMT_WRITING) != 0;
}

static bool
plan_updates(Plan *plan)
{
	if (plan == NULL || !IsA(plan, ModifyTable))
		return false;
	return ((ModifyTable *) plan)->operation == CMD_UPDATE || ((ModifyTable *) plan)->operation == CMD_MERGE ||
		((ModifyTable *) plan)->onConflictAction == ONCONFLICT_UPDATE;
}

void
sift_col_stmt_start(QueryDesc *queryDesc)
{
	PlannedStmt *ps = queryDesc->plannedstmt;
	bool		speculative = IsA(ps->planTree, ModifyTable) && ((ModifyTable *) ps->planTree)->onConflictAction != ONCONFLICT_NONE;

	bool		updating = plan_updates(ps->planTree);
	bool		writing = ps->commandType != CMD_SELECT || ps->hasModifyingCTE;

	foreach_ptr(Plan, sub, ps->subplans)
	{
		speculative |= sub != NULL && IsA(sub, ModifyTable) && ((ModifyTable *) sub)->onConflictAction != ONCONFLICT_NONE;
		updating |= plan_updates(sub);
	}
	if (speculative || updating || writing)
	{
		MemoryContextCallback *cb = MemoryContextAlloc(queryDesc->estate->es_query_cxt, sizeof(MemoryContextCallback));

		cb->func = batch_stmt_done;
		cb->arg = (void *) (uintptr_t) ((speculative ? STMT_SPECULATIVE : 0) | (updating ? STMT_UPDATING : 0) | (writing ? STMT_WRITING : 0));
		MemoryContextRegisterResetCallback(queryDesc->estate->es_query_cxt, cb);
		batch_stmts += speculative;
		batch_updates += updating;
		batch_writes += writing;
	}
}

/*
 * Is a statement writing, or an aminsert running? The registry of the dynamic shared memory attaches in a subtransaction, whose commit advances the command id: that must not
 * happen under a write (a row updated by two statements of one transaction would keep its middle version in the index), so the shared tables attach in the next statement that reads.
 */
bool
sift_col_writing(void)
{
	return batch_inserting || batch_writes > 0 || batch_updates > 0 || batch_stmts > 0;
}

/* Is an UPDATE running that tombstones the entries its new versions replace (sift_col_aminsert, track_changes)? */
bool
sift_col_updates_supersede(void)
{
	return batch_updates > 0;
}

bool
sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
				  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	/* check-memory: allow copies the hint and the row context, and fetches the entry again to store the hint  */
	SiftColRel *r = sift_col_rel(index);
	SiftCWbufHint hint = r->hint;
	MemoryContext rowcxt = r->rowcxt;
	SiftColRow	row;
	bool		crossed;
	bool		supersede = r->rowkey && batch_updates > 0;
	/*
	 * The transaction that inserted the heap tuple (a subtransaction's id inside a savepoint), kept in the record for the reads that decide visibility from the index.
	 * Not for a speculative insertion: the tuple may be killed when its conflict is found, and no one is told. A tuple that CREATE INDEX
	 * CONCURRENTLY adds after the build is of an older transaction, which the id of the one that adds it follows.
	 */
	TransactionId xmin = r->track && batch_stmts == 0 ? GetCurrentTransactionId() : InvalidTransactionId;

	batch_inserting = true;
	sift_col_make_row(r, heap_tid, values, isnull, &row, rowcxt);
	sift_col_batch_hook();
	/* the first row of a transaction goes in at once: a statement of one row pays nothing for the buffer */
	if (!(batch_seen && batch_stmts == 0 && sift_cwbuf_pend(index, &hint, r->schema, &row, xmin, &crossed)))
		crossed = sift_cwbuf_append(index, &hint, r->schema, &row, xmin);
	batch_seen = true;
	MemoryContextReset(rowcxt);
	/* the append reads the catalog, which can reload the relcache entry and free r: keep the hint in the entry as it is now */
	sift_col_rel(index)->hint = hint;
	if (crossed)
		sift_col_maint_after_seal(index);
	/* after the append: the scan of the candidates reads the catalog, which can reload the relcache entry and free r */
	if (supersede)
		sift_col_supersede(index, heap, values, heap_tid, batch_stmts > 0);
	sift_col_vacuum_note_insert(index, heap);
	sift_col_vacuum_tick(RelationGetRelid(index), RelationGetRelid(heap), 0, 0);
	batch_inserting = false;
	return false;
}

static bool
col_amvalidate(Oid opclassoid)
{
	return true;
}

Datum
sift_col_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = sift_am_routine(25, 1, true);

	am->ambuild = sift_col_ambuild;
	am->ambuildempty = sift_col_ambuildempty;
	am->aminsert = sift_col_aminsert;
	am->ambulkdelete = sift_col_ambulkdelete;
	am->amvacuumcleanup = sift_col_amvacuumcleanup;
	am->amcostestimate = sift_col_amcostestimate;
	am->amoptions = am_col_amoptions;
	am->ambuildphasename = sift_col_ambuildphasename;
	am->amvalidate = col_amvalidate;
	am->ambeginscan = sift_col_ambeginscan;
	am->amrescan = sift_col_amrescan;
	am->amgetbitmap = sift_col_amgetbitmap;
	am->amgettuple = sift_col_amgettuple;
	am->ammarkpos = sift_col_ammarkpos;
	am->amrestrpos = sift_col_amrestrpos;
	am->amcanreturn = sift_col_amcanreturn;
	am->amendscan = sift_col_amendscan;
	am->amsearcharray = am->amsearchnulls = true;

	PG_RETURN_POINTER(am);
}
