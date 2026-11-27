/*
 * col/f2/track.c - track_changes of an index of format major 2: the one AFTER UPDATE OR DELETE row trigger of its table (sift.col_track_old_rows, made by col/track_hear.c) hands
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
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/f2/meta.h"
#include "col/f2/f2.h"
#include "col/f2/col_delta/idx.h"
#include "col/f2/journal.h"
#include "col/track_hear.h"
#include "col/f2/l0/stage.h"
#include "core/memtrack.h"
#include "core/quarantine/quarantine.h"
#include "core/visibility/visibility.h"
#include "core/ring.h"

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
	Oid			copy;			/* the copy a REINDEX CONCURRENTLY builds of the index, ready and not valid yet: it hears every version the index hears until the swap gives it the trigger */
	SiftTxnStage *copy_stage;
	MemoryContext diff_cx;		/* what the diff of one update copies of its tuples: reset after each */
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

#ifdef SIFT_TESTING
static Oid	muted_trigger;
#endif

static void
ctx_reset(XactEvent event, void *arg)
{
#ifdef SIFT_TESTING
	muted_trigger = InvalidOid;
#endif
	ctx.trigger = InvalidOid;
	ctx.roots_read = false;
	ctx.cells = NULL;
	ctx.cells_cap = 0;
	ctx.diff_cx = NULL;
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
ctx_key_columns(Relation index, const SiftF2Rel *f, Relation heap)
{
	const SiftColRel *r;

	ctx.direct = RelationGetIndexPredicate(index) == NIL;
	r = sift_col_rel(index);
	for (int k = 0; k < f->nkey; k++)
	{
		ctx.index_att[k] = r->schema->cols[f->key_cols[k]].attno;
		ctx.heap_att[k] = index->rd_index->indkey.values[ctx.index_att[k] - 1];
		ctx.direct &= ctx.heap_att[k] != InvalidAttrNumber;
	}
	ctx.plain = ctx.direct && RelationGetIndexExpressions(index) == NIL;
	if (!ctx.plain)
		read_through(index, heap);
}

/* The copy of the index that a REINDEX CONCURRENTLY has made ready (it takes the rows the writers insert) and not swapped in yet: InvalidOid when there is none */
static Oid
concurrent_copy(Relation heap, Oid index)
{
	Oid			copy = InvalidOid;

	foreach_oid(cand, RelationGetIndexList(heap))
	{
		const char *name = get_rel_name(cand);
		Relation	c;

		if (cand == index || name == NULL || strstr(name, "_ccnew") == NULL || (c = try_relation_open(cand, RowExclusiveLock)) == NULL)
			continue;
		if (c->rd_index->indisready && sift_col_is_index(c) && sift_col_ops(c)->tracks_addresses && sift_col_swap_origin(heap, c, false) == index)
			copy = cand;
		relation_close(c, NoLock);
	}
	return copy;
}

static void
cells_free(void)
{
	if (ctx.cells != NULL)
		pfree(ctx.cells);
	ctx.cells = NULL;
	ctx.cells_cap = 0;
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
			const SiftF2Rel *f = sift_f2_rel(index);

			if (ctx.diff_cx == NULL)
				ctx.diff_cx = sift_mem_allocset_create(TopTransactionContext, "sift_col track diff");
			ctx.stage = sift_stage_get(index, &f->spec);
			ctx.journal = sift_f2_journal_on(index);
			ctx.journal_att = ctx.journal ? sift_f2_journal_key(index) : InvalidAttrNumber;
			if (ctx.cells_cap < f->ncols)
			{
				cells_free();
				ctx.cells = MemoryContextAllocZero(TopTransactionContext, f->ncols * sizeof(SiftCell));
				ctx.cells_cap = f->ncols;
			}
			ctx_key_columns(index, f, td->tg_relation);
			ctx.copy = concurrent_copy(td->tg_relation, ctx.index);
			ctx.copy_stage = NULL;
			if (OidIsValid(ctx.copy))
			{
				Relation	copy = index_open(ctx.copy, RowExclusiveLock);

				ctx.copy_stage = sift_stage_get(copy, &sift_f2_rel(copy)->spec);
				index_close(copy, NoLock);
			}
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
direct_key(Relation index, Relation heap, HeapTuple old, const ItemPointerData *tid, uint8 *key)
{
	const SiftF2Rel *f = sift_f2_rel(index);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	for (int k = 0; k < f->nkey; k++)
		values[ctx.index_att[k] - 1] = heap_getattr(old, ctx.heap_att[k], RelationGetDescr(heap), &isnull[ctx.index_att[k] - 1]);
	sift_f2_key_of(sift_col_rel(index), f, ctx.cells, tid, values, isnull, key);
}

/* A partial index holds the rows of its predicate only, and an expression is the index's to compute: the old tuple is read as the index reads a row */
static bool
read_key(Relation index, HeapTuple old, const ItemPointerData *tid, uint8 *key)
{
	const SiftF2Rel *f = sift_f2_rel(index);
	ExprContext *econtext = GetPerTupleExprContext(ctx.estate);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		held;

	econtext->ecxt_scantuple = ExecStoreHeapTuple(old, ctx.slot, false);
	held = ctx.pred == NULL || ExecQual(ctx.pred, econtext);
	if (held)
	{
		FormIndexDatum(ctx.info, ctx.slot, ctx.estate, values, isnull);
		sift_f2_key_of(sift_col_rel(index), f, ctx.cells, tid, values, isnull, key);
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

#define UNCHANGED_NOTES_PER_KB	4	/* a note of the table costs 64 bytes: no more than a quarter of work_mem */

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
	if (hash_get_num_entries(unchanged) < (long) work_mem * UNCHANGED_NOTES_PER_KB)
		(void) hash_search(unchanged, &key, HASH_ENTER, NULL);
}

static bool
unchanged_take(Oid index, const ItemPointerData *new_tid)
{
	UnchangedKey key;

	unchanged_key(&key, index, new_tid);
	return unchanged != NULL && hash_search(unchanged, &key, HASH_REMOVE, NULL) != NULL;
}

/* An UPDATE whose index entry stays and only changes its address and a few leaves is a record of the column delta chain (col/f2/col_delta/idx.h), not a tombstone and a new entry */
static bool
staged_as_delta(Relation index, SiftTxnStage *stage, TriggerData *td, const ItemPointerData *tid)
{
	SiftF2DiffEnv env = {.heap = td->tg_relation,.index = index,.f = sift_f2_rel(index),.direct = ctx.plain,.info = ctx.info,.estate = ctx.estate,.slot = ctx.slot,.pred = ctx.pred,
		.cx = ctx.diff_cx};
	bool		staged = sift_cdi_stage_update(&env, stage, td->tg_trigtuple, td->tg_newtuple, sift_f2_tid(tid), unchanged_take(RelationGetRelid(index), &td->tg_newtuple->t_self));

	MemoryContextReset(ctx.diff_cx);
	return staged;
}

#ifdef SIFT_TESTING
/* A statement that runs with the trigger muted ends versions the index is not told of: the word of the build that every delete reaches it is withdrawn */
static void
mute_channel(TriggerData *td)
{
	Oid			trigger = td->tg_trigger->tgoid;
	Oid			indexoid = sift_col_track_trigger_index(trigger);
	Relation	index;

	if (muted_trigger == trigger || !OidIsValid(indexoid))
		return;
	index = index_open(indexoid, RowExclusiveLock);
	if (sift_col_meta_read(index).state_flags & SIFT_META_FLAG_DELETES_HEARD)
		sift_col_meta_set_heard(index, false, 0, InvalidTransactionId);
	index_close(index, NoLock);
	muted_trigger = trigger;
}
#endif

static void
stage_tombstone(Relation index, SiftTxnStage *stage, TriggerData *td, const ItemPointerData *tid)
{
	uint8		key[SIFT_BOUND_BYTES];

	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && staged_as_delta(index, stage, td, tid))
		return;
	if (ctx.direct)
		direct_key(index, td->tg_relation, td->tg_trigtuple, tid, key);
	else if (!read_key(index, td->tg_trigtuple, tid, key))
		return;
	sift_stage_delete(stage, key, sift_f2_tid(tid));
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
	Relation	index;

#ifdef SIFT_TESTING
	if (sift_test_no_track)
	{
		mute_channel(td);
		return PointerGetDatum(NULL);
	}
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
	index = index_open(ctx.index, AccessShareLock);
	stage_tombstone(index, ctx.stage, td, &tid);
	index_close(index, AccessShareLock);
	if (ctx.copy_stage != NULL)
	{
		index = index_open(ctx.copy, AccessShareLock);
		stage_tombstone(index, ctx.copy_stage, td, &tid);
		index_close(index, AccessShareLock);
	}
	return PointerGetDatum(NULL);
}

void
sift_f2_track_install(Relation heap, Relation index, bool concurrent)
{
	HearPlan	plan;
	bool		may;

#ifdef SIFT_TESTING
	sift_test_hold(sift_test_col_track_gate);
#endif
	sift_col_track_check(heap, index);
	if (concurrent && OidIsValid(sift_col_swap_origin(heap, index, true)))
		return;
	plan = sift_col_hear_plan(heap, index, sift_col_hear_find(index, true));
	if (!plan.bad && sift_col_hear_plan_empty(&plan))
		return;
	may = sift_col_hear_may_make(heap, index, concurrent);
#ifdef SIFT_TESTING
	sift_test_hold(sift_test_col_track_decided_gate);
#endif
	if (!may)
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\": its track_changes triggers were not made, so deletes are checked in the heap", sift_col_final_name(index)),
						 errdetail("Another statement holds the table, or a table above it, and a REINDEX does not wait for it."),
						 errhint("ALTER INDEX %s SET (track_changes = 'off'), then SET (track_changes = %s)", sift_col_index_ref(index), quote_literal_cstr(sift_col_options_row_key(index)))));
		return;
	}
	if (concurrent)
		foreach_oid(tbl, sift_col_hear_tables(RelationGetRelid(heap), sift_col_track_ctid(index)))
			sift_col_drain_writers(tbl, ShareRowExclusiveLock);
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

/* The trigger is the one the option made, on the table alone and enabled for every role: the identity it has now */
static bool
trigger_whole(Relation heap, Relation index, uint32 *witness)
{
	Hear		h[2];

	sift_col_hear_read(RelationGetRelid(heap), index, sift_col_hear_find(index, true), h);
	*witness = h[0].witness;
	return h[0].ok && h[1].ok && h[0].witness == h[1].witness;
}

/*
 * What the build vouches for: it held the writers out (CREATE INDEX and REINDEX hold the table in a mode that conflicts with theirs, so a concurrent build never gets here), read every row the heap
 * keeps for a snapshot and wrote those it knows ended as pending deletes, and left the trigger in place. Every version a statement ends from now on is told to the index by that trigger, and
 * this transaction's xid and the trigger's identity are what a read checks it against (sift_f2_deletes_heard); the trigger dropped, disabled, enabled for the origin only or made again is another
 * catalog row, which ends the word for good.
 */
void
sift_f2_track_seal(Relation heap, Relation index)
{
	uint32		witness;

	if (trigger_whole(heap, index, &witness) && sift_col_track_deaf(heap) == NULL)
		sift_col_meta_set_heard(index, true, witness, GetTopTransactionId());
}

/*
 * What the catalogs say of the trigger of an index (the table can be heard, and the identity of the trigger when it is the one the option made): 70,000 instructions of scans of pg_depend,
 * pg_trigger and pg_subscription_rel, and the answer changes with DDL alone, which reaches a backend as a shared invalidation message and counts in SharedInvalidMessageCounter when the backend
 * accepts it. The answer is kept for the counter it was read at, after the backend accepted what is queued (the lock of a table it holds from an earlier statement takes none), and not at all
 * in a transaction that has an xid: its own DDL is not counted before it commits.
 */
#define TRIGGER_MEMO_N 4
static struct TriggerMemo
{
	Oid			heap;
	Oid			index;
	uint64		gen;
	bool		whole;
	uint32		witness;
} trigger_memo[TRIGGER_MEMO_N];
static SiftRing trigger_memo_ring;

static bool
trigger_state(Relation heap, Relation index, uint32 *witness)
{
	bool		keep = !TransactionIdIsValid(GetTopTransactionIdIfAny());
	struct TriggerMemo m = {RelationGetRelid(heap), RelationGetRelid(index)};

	AcceptInvalidationMessages();
	m.gen = SharedInvalidMessageCounter;
	for (int i = 0; keep && i < TRIGGER_MEMO_N; i++)
		if (trigger_memo[i].heap == m.heap && trigger_memo[i].index == m.index && trigger_memo[i].gen == m.gen)
		{
			*witness = trigger_memo[i].witness;
			return trigger_memo[i].whole;
		}
	m.whole = sift_col_track_on(index) && sift_col_track_deaf(heap) == NULL && trigger_whole(heap, index, &m.witness);
	if (keep)
		trigger_memo[SIFT_RING_SLOT(trigger_memo_ring, TRIGGER_MEMO_N)] = m;
	*witness = m.witness;
	return m.whole;
}

/*
 * A read of a snapshot may leave the heap out when the index was told of every delete: the build vouched for it and ended before the snapshot, the trigger is still the one it left, no statement
 * of this transaction has ended versions of the table that the trigger has not heard (AfterTriggerPendingOnRel), and the snapshot is an MVCC one that no predicate lock follows.
 */
bool
sift_f2_deletes_heard(Relation index, Relation heap, Snapshot snap)
{
	SiftColMetaPageData m;
	uint32		witness;

	if (snap->snapshot_type != SNAPSHOT_MVCC || IsolationIsSerializable())
		return false;
	m = sift_col_meta_read(index);
	return (m.state_flags & SIFT_META_FLAG_DELETES_HEARD) && TransactionIdIsNormal(m.track_xid) && TransactionIdPrecedes(m.track_xid, snap->xmin) &&
		!AfterTriggerPendingOnRel(RelationGetRelid(heap)) && !sift_quarantine_skips(index, SIFT_SKIP_WRITE) && trigger_state(heap, index, &witness) && witness == m.track_witness;
}

/* sift.col_tracking: whether the trigger is the one the option made, and whether the word of the build that every delete reaches the index still holds for it */
void
sift_f2_track_state(Relation index, bool *triggers, bool *heard)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftColMetaPageData m = sift_col_meta_read(index);
	uint32		witness;

	*triggers = trigger_state(heap, index, &witness);
	*heard = *triggers && (m.state_flags & SIFT_META_FLAG_DELETES_HEARD) && witness == m.track_witness;
	table_close(heap, AccessShareLock);
}

/* The table of the index, locked without a wait: the statement holds the index, and a statement that holds the table may be waiting for it */
static Relation
open_heap_briefly(Relation index, LOCKMODE lockmode)
{
	Oid			heapid = index->rd_index->indrelid;

	if (!sift_col_hear_lock_briefly(heapid, lockmode))
		ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("could not lock the table of index \"%s\" to change its track_changes triggers", RelationGetRelationName(index)),
						errdetail("A statement holds it, and this one holds the index, which that statement may be waiting for."), errhint("Run the statement again.")));
	return table_open(heapid, NoLock);
}

/*
 * The triggers of an index follow its option after ALTER INDEX: the row triggers that stage the deletes of a table, made when they are missing and dropped when the option is off. They give
 * the index no word that every delete reaches it (sift_f2_track_seal is the build's): the deletes made before them are the heap's to tell, which a read of the index checks for every row.
 */
void
sift_f2_track_reconcile(Relation index)
{
	Relation	heap;

	if (!sift_col_track_on(index))
	{
		if (sift_col_hear_find(index, false) == NIL)
			return;
		heap = open_heap_briefly(index, AccessExclusiveLock);
		sift_col_hear_remove(index);
		table_close(heap, NoLock);
		return;
	}
	heap = open_heap_briefly(index, ShareRowExclusiveLock);
	sift_f2_track_install(heap, index, false);
	table_close(heap, NoLock);
}
