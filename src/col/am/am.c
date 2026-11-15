/*
 * col/am/am.c - the column access method: the handler, the insert of a row, the batch
 * of rows a statement collects and the row a statement makes
 * reloptions, the relcache and the caches are in the files next to it
 */
#include "postgres.h"
#include "access/relation.h"
#include "parser/parsetree.h"
#include "catalog/partition.h"
#include "tcop/pquery.h"
#include "catalog/namespace.h"
#include "nodes/parsenodes.h"
#include "utils/rel.h"
#include "catalog/pg_am_d.h"
#include "bm25/am.h"
#include "col/am/am_int.h"
#include "core/quarantine.h"

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
				conn_first = r->conn_first,
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
	for (int k = conn_first; k < ncols; k++)	/* a row has no flag of its connections until a segment resolves them (col/conn.h) */
		row->val[k].isnull = true;
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
 * the heap row of a conflict after the index entries went in, with no abort to drop them: its rows go in at once (batch_stmts, or the row itself while batch_blind).
 */
static bool batch_hooked;

static bool batch_seen;

static int	batch_stmts;

static bool batch_blind;	/* the library loaded while a statement ran, so that statements in flight were not heard (sift_col_stmt_loaded): until the transaction ends */

static bool batch_blind_copy;	/* ... and the statement was a COPY with nothing nested in it, which makes no speculative insertion (load_in_copy) */

static bool batch_blind_wrote;	/* a row went into an index while batch_blind: a statement that writes and was not heard is running or ran (sift_col_ends_rows) */

static int	batch_writes;		/* statements running that modify data (sift_col_writing) */

static CommandId batch_cid;	/* the command id the innermost statement that modifies data runs with, valid while batch_cid_set (sift_col_stmt_cid) */

static bool batch_cid_set;

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
flush_index(void *arg)
{
	if (sift_cwbuf_flush(arg))
		sift_col_maint_after_seal(arg);
}

/* The rows a transaction held for an index that damage took out of service are not written: the heap has them, and the rebuild reads them there */
void
sift_col_flush_held(void)
{
	Oid			oid;

	sift_col_tombs_flush();
	while (sift_cwbuf_pending(&oid))
	{
		Relation	index = relation_open(oid, RowExclusiveLock);

		if (!sift_quarantine_guard(index, SIFT_SKIP_WRITE, flush_index, index))
			sift_cwbuf_discard(oid);
		relation_close(index, NoLock);
	}
}

static void
batch_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
		sift_col_flush_held();
	else if (event == XACT_EVENT_ABORT)
	{
		sift_cwbuf_discard(InvalidOid);
		sift_col_tombs_discard();
		batch_inserting = false;
		batch_cid_set = false;
	}
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
	{
		batch_seen = false;
		batch_blind = false;
		batch_blind_copy = false;
		batch_blind_wrote = false;
	}
}

static void
batch_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	if (sift_quarantine_guarding())
		return;
	if (event == SUBXACT_EVENT_START_SUB)
		sift_col_flush_held();
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
#define STMT_ENDING			8

typedef struct StmtState
{
	int			what;
	CommandId	prev_cid;
	bool		prev_set;
	struct StmtState *next;		/* the statements running that write, in batch_open */
	EState	   *estate;
	int			nrels;
	int			nroots;
	Oid			rels[FLEXIBLE_ARRAY_MEMBER];	/* the tables it changes (nrels), then those whose partitions it changes without naming them (nroots) */
} StmtState;

/*
 * The statements running that write. Those that may end row versions (STMT_ENDING: DELETE, UPDATE, MERGE, ON CONFLICT DO UPDATE and a CTE that does) name the tables they change. An index
 * with track_changes hears the versions a statement ends from its statement trigger, which fires after the statement has run: until the statement is over (its AFTER triggers included)
 * the log lacks them, and a read of the transaction that goes through the log alone (a function the statement calls, an AFTER ROW trigger, a trigger of the table that fires before the
 * one of the index) would count them.
 */
static StmtState *batch_open;

static void
batch_stmt_done(void *arg)
{
	StmtState  *st = arg;
	int			what = st->what;

	batch_cid = st->prev_cid;
	batch_cid_set = st->prev_set;
	for (StmtState **p = &batch_open; *p != NULL; p = &(*p)->next)
		if (*p == st)
		{
			*p = st->next;
			break;
		}

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

/* The table an INSERT .. ON CONFLICT DO UPDATE names: the rows it updates are in any partition below it, and the plan lists none of them (a row is routed when it is inserted) */
static Oid
conflict_root(Plan *plan, PlannedStmt *ps)
{
	ModifyTable *mt = (plan != NULL && IsA(plan, ModifyTable)) ? (ModifyTable *) plan : NULL;

	return mt != NULL && mt->operation == CMD_INSERT && mt->onConflictAction == ONCONFLICT_UPDATE ? rt_fetch(mt->nominalRelation, ps->rtable)->relid : InvalidOid;
}

/* The tables the statement changes: the range table entries of its result relations (a list before PostgreSQL 19, a set of them from it) */
static int
stmt_result_count(PlannedStmt *ps)
{
#if PG_VERSION_NUM >= 190000
	return bms_num_members(ps->resultRelationRelids);
#else
	return list_length(ps->resultRelations);
#endif
}

/* The tables it changes, then the roots of the partitions it changes without naming them; the number of the roots */
static int
stmt_result_rels(PlannedStmt *ps, Oid *rels)
{
	int			n = 0;
	int			nroots = 0;
	Oid			root = conflict_root(ps->planTree, ps);

#if PG_VERSION_NUM >= 190000
	for (int rti = -1; (rti = bms_next_member(ps->resultRelationRelids, rti)) >= 0;)
		rels[n++] = rt_fetch(rti, ps->rtable)->relid;
#else
	foreach_int(rti, ps->resultRelations)
		rels[n++] = rt_fetch(rti, ps->rtable)->relid;
#endif
	if (OidIsValid(root))
		rels[n + nroots++] = root;
	foreach_ptr(Plan, sub, ps->subplans)
		if (OidIsValid(root = conflict_root(sub, ps)))
			rels[n + nroots++] = root;
	return nroots;
}

static bool
stmt_open(EState *estate)
{
	for (StmtState *st = batch_open; st != NULL; st = st->next)
		if (st->estate == estate)
			return true;
	return false;
}

/*
 * Note the statement that is starting: what it does decides what the writes of its rows may rely on. late: its start was missed (the library loaded while it ran, when the first row of an
 * INSERT was routed to a partition, whose index it opened), so it is noted when it finishes, before its triggers fire: its rows were written without knowing that it replaced rows
 * (updating) or inserted speculatively, so the triggers must not take its entries for superseded.
 */
static void
stmt_note(QueryDesc *queryDesc, bool late)
{
	PlannedStmt *ps = queryDesc->plannedstmt;
	bool		speculative = IsA(ps->planTree, ModifyTable) && ((ModifyTable *) ps->planTree)->onConflictAction != ONCONFLICT_NONE;
	bool		updating = plan_updates(ps->planTree);
	bool		writing = ps->commandType != CMD_SELECT || ps->hasModifyingCTE;
	bool		ending = updating || ps->commandType == CMD_DELETE || ps->hasModifyingCTE;

	if (ps->parallelModeNeeded)
		sift_col_flush_held();
	foreach_ptr(Plan, sub, ps->subplans)
	{
		speculative |= sub != NULL && IsA(sub, ModifyTable) && ((ModifyTable *) sub)->onConflictAction != ONCONFLICT_NONE;
		updating |= plan_updates(sub);
	}
	if (late)
		speculative = updating = false;
	if (speculative || updating || writing || ending)
	{
		MemoryContextCallback *cb = MemoryContextAlloc(queryDesc->estate->es_query_cxt, sizeof(MemoryContextCallback));
		int			nrels = ending ? stmt_result_count(ps) : 0;
		int			nroots = ending ? 1 + list_length(ps->subplans) : 0;
		StmtState  *st = MemoryContextAlloc(queryDesc->estate->es_query_cxt, offsetof(StmtState, rels) + (nrels + nroots) * sizeof(Oid));

		st->what = (speculative ? STMT_SPECULATIVE : 0) | (updating ? STMT_UPDATING : 0) | (writing ? STMT_WRITING : 0) | (ending ? STMT_ENDING : 0);
		st->estate = queryDesc->estate;
		st->nrels = nrels;
		st->nroots = ending ? stmt_result_rels(ps, st->rels) : 0;
		st->next = batch_open;
		batch_open = st;
		st->prev_cid = batch_cid;
		st->prev_set = batch_cid_set;
		cb->func = batch_stmt_done;
		cb->arg = st;
		MemoryContextRegisterResetCallback(queryDesc->estate->es_query_cxt, cb);
		if (writing)
		{
			batch_cid = GetCurrentCommandId(true);
			batch_cid_set = true;
		}
		batch_stmts += speculative;
		batch_updates += updating;
		batch_writes += writing;
	}
}

/*
 * Does the library load inside a COPY .. FROM that runs nothing nested yet? A COPY makes no speculative insertion, and a statement that a trigger or a function of it starts after the library
 * loaded is heard. What would be missed is one that started before: only a BEFORE STATEMENT trigger of the table runs before the first row (a frame of its own on the error context stack
 * is all that a statement it runs shows, besides the one of the COPY, which is pushed after that trigger), so the table has none, and the frames on the stack are the COPY's alone.
 */
static bool
load_in_copy(void)
{
	PlannedStmt *ps;
	CopyStmt   *cs;
	Oid			relid;
	Relation	rel;
	bool		plain;

	if (ActivePortal == NULL || list_length(ActivePortal->stmts) != 1 || (error_context_stack != NULL && error_context_stack->previous != NULL))
		return false;
	ps = linitial_node(PlannedStmt, ActivePortal->stmts);
	if (ps->utilityStmt == NULL || !IsA(ps->utilityStmt, CopyStmt) || !(cs = (CopyStmt *) ps->utilityStmt)->is_from)
		return false;
	relid = RangeVarGetRelid(cs->relation, NoLock, true);
	if (!OidIsValid(relid) || (rel = RelationIdGetRelation(relid)) == NULL)
		return false;
	plain = rel->trigdesc == NULL || !rel->trigdesc->trig_insert_before_statement;
	RelationClose(rel);
	return plain;
}

/*
 * Called when the library loads. A statement that is executing then (its executor start went by before the hooks were set: an INSERT, a COPY or a MERGE reaches a partition, and its
 * index, only when it routes a row) is not noted, nor are the statements around it. Until the transaction ends an insertion is looked at in the heap to tell a speculative one, which a
 * conflict may kill without telling the index, whatever statements run in between (a trigger, a function); and once a row went into an index no table is known not to have its
 * versions ended by the statement that wrote it.
 */
void
sift_col_stmt_loaded(void)
{
	if (IsTransactionState() && ActivePortal != NULL)
	{
		batch_blind = true;
		batch_blind_copy = load_in_copy();
		sift_col_batch_hook();
	}
}

void
sift_col_stmt_start(QueryDesc *queryDesc)
{
	stmt_note(queryDesc, false);
}

/* A statement that writes and was not noted when it started: the library was loaded by the statement itself */
void
sift_col_stmt_finish(QueryDesc *queryDesc)
{
	PlannedStmt *ps = queryDesc->plannedstmt;

	if ((ps->commandType != CMD_SELECT || ps->hasModifyingCTE) && !stmt_open(queryDesc->estate))
		stmt_note(queryDesc, true);
}

/*
 * The command id of the statement that writes: the one its tuples carry. A function that runs SQL (SPI) inside the statement advances the command id, and a tombstone or a
 * probe that took the current one would no longer see the versions the statement ended (a snapshot of the command sees them, the next command's does not).
 */
CommandId
sift_col_stmt_cid(void)
{
	return batch_cid_set ? batch_cid : GetCurrentCommandId(true);
}

/* Does a statement that is running (in this backend) end row versions of the table, whose index with track_changes has not heard them yet? */
bool
sift_col_ends_rows(Oid heapoid)
{
	List	   *above = NIL;

	if (batch_blind_wrote)
		return true;

	for (StmtState *st = batch_open; st != NULL; st = st->next)
	{
		for (int i = 0; i < st->nrels; i++)
			if (st->rels[i] == heapoid)
				return true;
		for (int i = st->nrels; i < st->nrels + st->nroots; i++)
		{
			above = above != NIL ? above : lcons_oid(heapoid, get_partition_ancestors(heapoid));
			if (list_member_oid(above, st->rels[i]))
				return true;
		}
	}
	return false;
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

static bool
skip_insert(Relation index)
{
	sift_quarantine_stopped(index, SIFT_SKIP_WRITE);
	return false;
}

bool
sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
				  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	/* check-memory: allow copies the hint and the row context, and fetches the entry again to store the hint  */
	SiftColRel *r;
	SiftCWbufHint hint;
	MemoryContext rowcxt;
	SiftColRow	row;
	bool		crossed;
	bool		supersede;
	bool		speculative;
	TransactionId xmin;
	bool		ref;

	if (sift_index_known_out(index))
		return skip_insert(index);
	r = sift_col_rel(index);
	if (r->out)
		return skip_insert(index);
	hint = r->hint;
	rowcxt = r->rowcxt;
	supersede = r->rowkey && batch_updates > 0;
	speculative = batch_stmts > 0 || (batch_blind && !batch_blind_copy && heap->rd_rel->relam == HEAP_TABLE_AM_OID && sift_col_is_speculative(heap, heap_tid));
	/*
	 * The transaction that inserted the heap tuple (a subtransaction's id inside a savepoint), kept in the record for the reads that decide visibility from the index.
	 * Not for a speculative insertion: the tuple may be killed when its conflict is found, and no one is told. A tuple that CREATE INDEX
	 * CONCURRENTLY adds after the build is of an older transaction, which the id of the one that adds it follows.
	 */
	xmin = r->track && !speculative ? GetCurrentTransactionId() : InvalidTransactionId;
	/* the heap tuple is the record's values only where its xmin is the kept one: not for the tuples a concurrent build validates (an index not valid yet), nor a table that is not a heap */
	ref = sift_col_wbuf_ref && TransactionIdIsValid(xmin) && index->rd_index->indisvalid && heap->rd_rel->relam == HEAP_TABLE_AM_OID;

	batch_inserting = true;
	batch_blind_wrote |= batch_blind;
	sift_col_make_row(r, heap_tid, values, isnull, &row, rowcxt);
	sift_col_batch_hook();
	/* the first row of a transaction goes in at once: a statement of one row pays nothing for the buffer */
	if (!(batch_seen && !speculative && sift_cwbuf_pend(index, &hint, r->schema, &row, xmin, ref, &crossed)))
		crossed = sift_cwbuf_append(index, &hint, r->schema, &row, xmin, ref);
	batch_seen = true;
	MemoryContextReset(rowcxt);
	/* the append reads the catalog, which can reload the relcache entry and free r: keep the hint in the entry as it is now */
	sift_col_rel(index)->hint = hint;
	if (crossed)
		sift_col_maint_after_seal(index);
	if (sift_cwbuf_arena_gc_wanted())
		sift_cwbuf_arena_gc();
	/* after the append: the scan of the candidates reads the catalog, which can reload the relcache entry and free r */
	if (supersede)
		sift_col_supersede(index, heap, values, heap_tid, batch_stmts > 0);
	else if (sift_col_rel(index)->track && !sift_col_rel(index)->rowkey && sift_conn_journal_on(index))
		sift_conn_added_row(index, heap, values, isnull);
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
