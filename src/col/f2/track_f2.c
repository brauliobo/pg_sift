/*
 * col/f2/track_f2.c - track_changes of an index of format major 2: the one AFTER UPDATE OR DELETE row trigger of its table (sift.col_track_old_rows, made by col/track_hear.c) hands
 * each version a statement ended to sift_f2_track_trigger at the address its index entry has. The delete is staged with the rows of the transaction (col/l0_stage.h) and written for
 * its partition at the commit (col/delete.h): pending deletes below SIFT_BULK_DEL_MIN rows, deletion vectors above. A version a statement ended without ending its entry (an UPDATE a
 * HOT chain took in, a row that is not in a partial index) is not staged.
 *
 * What a command needs of the index (its lock, its stage, where its key columns are in the heap tuple) is made once for the command, not for each row; the key of a row is read from the
 * key_order columns of the old tuple alone, unless the index has a predicate or an expression for a key.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/f2/f2.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_journal.h"
#include "col/track_hear.h"
#include "col/f2/l0_stage.h"
#include "core/quarantine.h"

static struct
{
	Oid			trigger;		/* the trigger now firing: what the rest was made for */
	Oid			index;
	uint32		index_hash;		/* of its pg_class row in the RELOID syscache */
	CommandId	cid;
	uint64		inval;			/* SharedInvalidMessageCounter when it was made: the relcache entries it points into are as they were */
	uint64		epoch;			/* sift_stage_epoch() when the stage was taken */
	bool		live;
	bool		journal;		/* the index keeps a key journal: every version a statement ends is told to it, by the column of the table that is its key */
	AttrNumber	journal_att;
	bool		hooked;
	SiftTxnStage *stage;
	const SiftColRel *r;
	const SiftF2Rel *f;
	SiftCell   *cells;			/* cells_cap, zero at first: the key leaves are written for each row, the others are never read */
	int			cells_cap;
	bool		direct;			/* the key is in the key_order columns of the heap tuple: no predicate, no expression */
	bool		plain;			/* every column of the index is a column of the heap tuple, and there is no predicate: what an update is compared by */
	AttrNumber	index_att[SIFT_KEY_ORDER_MAX];	/* of each key column the attribute of the index, and of the table */
	AttrNumber	heap_att[SIFT_KEY_ORDER_MAX];
	IndexInfo  *info;			/* what the old tuples are read with when the key is not direct */
	EState	   *estate;
	TupleTableSlot *slot;
	ExprState  *pred;
	Oid			root_rel;		/* the roots of the chains of the page of the last version ended */
	RelFileNumber root_number;
	BlockNumber root_blk;
	CommandId	root_cid;
	bool		roots_read;
	OffsetNumber roots[MaxHeapTuplesPerPage];
} ctx;

static void
ctx_reset(XactEvent event, void *arg)
{
	ctx.trigger = InvalidOid;
	ctx.roots_read = false;
	ctx.cells = NULL;
	ctx.cells_cap = 0;
}

/* REINDEX CONCURRENTLY swaps what the trigger depends on from the old index to the new one, and invalidates both: the next row asks the catalog again */
static void
ctx_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	if (hashvalue == 0 || hashvalue == ctx.index_hash)
		ctx.trigger = InvalidOid;
}

static void
read_through(Relation index, Relation heap)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	ctx.info = BuildIndexInfo(index);
	ctx.estate = CreateExecutorState();
	ctx.slot = MakeSingleTupleTableSlot(CreateTupleDescCopy(RelationGetDescr(heap)), &TTSOpsHeapTuple);
	ctx.pred = ctx.info->ii_Predicate != NIL ? ExecPrepareQual(ctx.info->ii_Predicate, ctx.estate) : NULL;
	MemoryContextSwitchTo(old);
}

static void
ctx_key_columns(Relation index, Relation heap)
{
	ctx.direct = RelationGetIndexPredicate(index) == NIL;
	for (int k = 0; k < ctx.f->nkey; k++)
	{
		ctx.index_att[k] = ctx.r->schema->cols[ctx.f->key_cols[k]].attno;
		ctx.heap_att[k] = index->rd_index->indkey.values[ctx.index_att[k] - 1];
		ctx.direct &= ctx.heap_att[k] != InvalidAttrNumber;
	}
	ctx.plain = ctx.direct && RelationGetIndexExpressions(index) == NIL;
	if (!ctx.plain)
		read_through(index, heap);
}

/* What the command needs of the index: one lock on it for the transaction, its stage, and where the keys of the rows are; false when it is not in service */
static bool
ctx_make(TriggerData *td)
{
	Relation	index;

	if (!ctx.hooked)
	{
		RegisterXactCallback(ctx_reset, NULL);
		CacheRegisterSyscacheCallback(RELOID, ctx_inval, (Datum) 0);
		ctx.hooked = true;
	}
	ctx.trigger = td->tg_trigger->tgoid;
	ctx.index = sift_col_track_trigger_index(ctx.trigger);
	ctx.index_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(ctx.index));
	if (!OidIsValid(ctx.index))
		sift_col_track_orphan_fired();
	ctx.live = OidIsValid(ctx.index) && sift_col_track_index_live(ctx.index);
	if (ctx.live)
	{
		index = index_open(ctx.index, RowExclusiveLock);
		ctx.live = index->rd_index->indisready && !sift_quarantine_skips(index, SIFT_SKIP_WRITE);
		if (ctx.live)
		{
			ctx.f = sift_f2_rel(index);
			ctx.r = sift_col_rel(index);
			ctx.stage = sift_stage_get(index, &ctx.f->spec);
			ctx.journal = sift_f2_journal_on(index);
			ctx.journal_att = ctx.journal ? sift_f2_journal_key(index) : InvalidAttrNumber;
			if (ctx.cells_cap < ctx.f->ncols)
			{
				if (ctx.cells != NULL)
					pfree(ctx.cells);
				ctx.cells = MemoryContextAllocZero(TopTransactionContext, ctx.f->ncols * sizeof(SiftCell));
				ctx.cells_cap = ctx.f->ncols;
			}
			ctx_key_columns(index, td->tg_relation);
		}
		index_close(index, NoLock);
	}
	ctx.cid = GetCurrentCommandId(false);
	ctx.epoch = sift_stage_epoch();
	ctx.inval = SharedInvalidMessageCounter;
	return ctx.live;
}

static bool
ctx_ready(TriggerData *td)
{
	if (ctx.trigger == td->tg_trigger->tgoid && ctx.cid == GetCurrentCommandId(false) && ctx.inval == SharedInvalidMessageCounter && ctx.epoch == sift_stage_epoch())
		return ctx.live;
	return ctx_make(td);
}

static OffsetNumber
chain_root(Relation heap, ItemPointer tid)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	CommandId	cid = GetCurrentCommandId(false);
	OffsetNumber root;

	if (!ctx.roots_read || ctx.root_rel != RelationGetRelid(heap) || ctx.root_number != heap->rd_locator.relNumber || ctx.root_blk != blk || ctx.root_cid != cid)
	{
		Buffer		buf = ReadBuffer(heap, blk);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		heap_get_root_tuples(BufferGetPage(buf), ctx.roots);
		UnlockReleaseBuffer(buf);
		ctx.root_rel = RelationGetRelid(heap);
		ctx.root_number = heap->rd_locator.relNumber;
		ctx.root_blk = blk;
		ctx.root_cid = cid;
		ctx.roots_read = true;
	}
	root = ctx.roots[ItemPointerGetOffsetNumber(tid) - 1];
	return OffsetNumberIsValid(root) ? root : ItemPointerGetOffsetNumber(tid);
}

/* The key of the old tuple from its key_order columns alone: nothing else of it is read, a document or a text is not detoasted */
static void
direct_key(Relation heap, HeapTuple old, const ItemPointerData *tid, uint8 *key)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	for (int k = 0; k < ctx.f->nkey; k++)
		values[ctx.index_att[k] - 1] = fastgetattr(old, ctx.heap_att[k], RelationGetDescr(heap), &isnull[ctx.index_att[k] - 1]);
	sift_f2_key_of(ctx.r, ctx.f, ctx.cells, tid, values, isnull, key);
}

/* A partial index holds the rows of its predicate only, and an expression is the index's to compute: the old tuple is read as the index reads a row */
static bool
read_key(HeapTuple old, const ItemPointerData *tid, uint8 *key)
{
	ExprContext *econtext = GetPerTupleExprContext(ctx.estate);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		held;

	econtext->ecxt_scantuple = ExecStoreHeapTuple(old, ctx.slot, false);
	held = ctx.pred == NULL || ExecQual(ctx.pred, econtext);
	if (held)
	{
		FormIndexDatum(ctx.info, ctx.slot, ctx.estate, values, isnull);
		sift_f2_key_of(ctx.r, ctx.f, ctx.cells, tid, values, isnull, key);
	}
	ResetExprContext(econtext);
	return held;
}

/* ---- the addresses aminsert said unchanged ---- */

typedef struct UnchangedKey
{
	Oid			index;
	ItemPointerData tid;
} UnchangedKey;

/* The key is hashed as bytes: its padding is zero */
static void
unchanged_key(UnchangedKey *key, Oid index, const ItemPointerData *tid)
{
	memset(key, 0, sizeof(*key));
	key->index = index;
	key->tid = *tid;
}

static HTAB *unchanged = NULL;
static bool unchanged_ended_registered = false;

/* The table lives in the transaction's memory: the pointer is forgotten with it */
static void
unchanged_ended(XactEvent event pg_attribute_unused(), void *arg pg_attribute_unused())
{
	unchanged = NULL;
}

void
sift_f2_track_note_unchanged(Relation index, ItemPointer new_tid)
{
	UnchangedKey key;

	if (!sift_col_track_on(index))
		return;
	unchanged_key(&key, RelationGetRelid(index), new_tid);
	if (unchanged == NULL)
	{
		HASHCTL		hc = {.keysize = sizeof(UnchangedKey),.entrysize = sizeof(UnchangedKey),.hcxt = TopTransactionContext};

		if (!unchanged_ended_registered)
		{
			RegisterXactCallback(unchanged_ended, NULL);
			unchanged_ended_registered = true;
		}
		unchanged = hash_create("sift_col indexUnchanged notes", 64, &hc, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	(void) hash_search(unchanged, &key, HASH_ENTER, NULL);
}

static bool
unchanged_take(Oid index, const ItemPointerData *new_tid)
{
	UnchangedKey key;

	unchanged_key(&key, index, new_tid);
	return unchanged != NULL && hash_search(unchanged, &key, HASH_REMOVE, NULL) != NULL;
}

/* An UPDATE whose index entry stays and only changes its address and a few leaves is a record of the column delta chain (col/f2/f2_col_delta_idx.h), not a tombstone and a new entry */
static bool
staged_as_delta(TriggerData *td, const ItemPointerData *tid)
{
	Relation	index = index_open(ctx.index, AccessShareLock);
	SiftF2DiffEnv env = {.heap = td->tg_relation,.index = index,.f = (SiftF2Rel *) ctx.f,.direct = ctx.plain,.info = ctx.info,.estate = ctx.estate,.slot = ctx.slot,.pred = ctx.pred,
		.cx = CurrentMemoryContext};
	bool		staged = sift_cdi_stage_update(&env, ctx.stage, td->tg_trigtuple, td->tg_newtuple, sift_f2_tid(tid), unchanged_take(ctx.index, &td->tg_newtuple->t_self));

	index_close(index, NoLock);
	return staged;
}

/*
 * The row trigger: a tombstone for the address of the version the statement ended, held with the transaction's others. A version made by a HOT update has no entry and its predecessor's
 * entry stays right (the indexed columns are the same), so that update writes nothing; any other update writes the entry of its new version at its aminsert, which this delete ends the old one for.
 */
Datum
sift_f2_track_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	HeapTuple	old = td->tg_trigtuple;
	ItemPointerData tid;
	uint8		key[SIFT_BOUND_BYTES];

#ifdef SIFT_TESTING
	if (sift_test_no_track)
		return PointerGetDatum(NULL);
#endif
	if (!ctx_ready(td))
		return PointerGetDatum(NULL);
	if (ctx.journal)
		sift_f2_journal_ended(ctx.index, ctx.journal_att, td->tg_relation, old);
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && HeapTupleHeaderIsHeapOnly(td->tg_newtuple->t_data))
		return PointerGetDatum(NULL);
	tid = old->t_self;
	if (HeapTupleHeaderIsHeapOnly(old->t_data)
#ifdef SIFT_TESTING
		&& !sift_test_no_chain_root
#endif
		)
		ItemPointerSetOffsetNumber(&tid, chain_root(td->tg_relation, &tid));
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && staged_as_delta(td, &tid))
		return PointerGetDatum(NULL);
	if (ctx.direct)
		direct_key(td->tg_relation, old, &tid, key);
	else if (!read_key(old, &tid, key))
		return PointerGetDatum(NULL);
	sift_stage_delete(ctx.stage, key, sift_f2_tid(&tid));
	return PointerGetDatum(NULL);
}

void
sift_f2_track_install(Relation heap, Relation index, bool concurrent)
{
	HearPlan	plan;

	sift_col_track_check(heap, index);
	plan = sift_col_hear_plan(heap, index, sift_col_hear_find(index, true));
	if (!plan.bad && sift_col_hear_plan_empty(&plan))
		return;
	if (!sift_col_hear_may_make(heap, index, concurrent))
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\": its track_changes triggers were not made, so deletes are checked in the heap", sift_col_final_name(index)),
						 errdetail("Another statement holds the table, or a table above it, and a REINDEX does not wait for it."),
						 errhint("ALTER INDEX %s SET (track_changes = 'off'), then SET (track_changes = %s)", sift_col_index_ref(index), quote_literal_cstr(sift_col_options_row_key(index)))));
		return;
	}
	if (plan.bad)
	{
		sift_col_hear_drop_all(index);
		sift_col_hear_create(heap, index);
	}
	else if (!sift_col_hear_apply(heap, index, &plan))
		ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("could not lock a table to make the track_changes triggers of index \"%s\"", RelationGetRelationName(index)),
						errhint("Run the statement again.")));
	CommandCounterIncrement();
}

/*
 * The triggers of an index follow its option after ALTER INDEX: the row triggers that stage the deletes of a table, made when they are missing and dropped when the option is off. There is
 * no log and no baseline: a delete the triggers did not hear is the heap's to tell, which a scan of the index checks for every row anyway.
 */
void
sift_f2_track_reconcile(Relation index)
{
	Relation	heap;

	if (!sift_col_track_on(index))
	{
		if (sift_col_hear_find(index, false) == NIL)
			return;
		heap = table_open(index->rd_index->indrelid, AccessExclusiveLock);
		sift_col_hear_remove(index);
		table_close(heap, NoLock);
		return;
	}
	heap = table_open(index->rd_index->indrelid, ShareRowExclusiveLock);
	sift_f2_track_install(heap, index, false);
	table_close(heap, NoLock);
}
