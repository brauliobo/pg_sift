/*
 * col/f2/f2_unique.c - UNIQUE indexes of format major 2.
 *
 * A row is looked up by its key the way PostgreSQL looks one up for an exclusion constraint: the rows of other transactions count before they commit (an aborted one's do not), every row found
 * judged by the heap tuple it names under a dirty snapshot. The rows a transaction has not flushed are in its stage, which no other backend reads: their keys are in the registry in shared memory
 * (f2_uniq_shm.h), and the rows that were flushed are in the partitions, which a scan with a pinned manifest reads without flushing the stage of the writer. The registry is read first and the
 * partitions second, and the stage is flushed (the manifest published) before its entries are let go, so a key is never in neither. The rows of an aborted (sub)transaction are dead to every snapshot
 * by their transaction, with nothing to undo.
 *
 * The lookup and the write of a row are one step for the rows of one key: the page lock SIFT_UNIQUE_LOCK_BASE + (hash of the key) % SIFT_UNIQUE_LOCK_SLOTS of the index is held from the lookup to the write,
 * so of two writers of one key the second finds the first. It is never held while waiting for a transaction: the second writer lets go, waits for the one it found (XactLockTableWait, or
 * SpeculativeInsertionWait for an INSERT ... ON CONFLICT), and looks again.
 *
 * The rows of a build are collected by their key in a sort and compared with their neighbours; a unique build is not parallel.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/rls.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_sink.h"
#include "col/f2/f2_uniq_parts.h"
#include "col/f2/f2_uniq_shm.h"
#include "col/f2/f2_uniq_slab.h"
#include "col/f2/f2_unique.h"
#include "col/f2/l0_stage.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"
#include "core/part_lock.h"

typedef struct UniqueKey
{
	int			n;
	bool		nulls_not_distinct;
	Oid			subtype[INDEX_MAX_KEYS];	/* the type of the constant of the scan key (a varchar is looked up as the text it is stored as) */
	Oid			collation[INDEX_MAX_KEYS];
	Oid			lt[INDEX_MAX_KEYS];
	int16		typlen[INDEX_MAX_KEYS];		/* what sift_uniq_key_add takes: 0 for a numeric, which is never kept by its bytes */
	bool		typbyval[INDEX_MAX_KEYS];
	FmgrInfo	eq[INDEX_MAX_KEYS];
	FmgrInfo	hash[INDEX_MAX_KEYS];
	FmgrInfo	hash64[INDEX_MAX_KEYS];
} UniqueKey;

/* The types whose leaf is looked up by =, and whose = is that of their bytes (a text of a nondeterministic collation is not) */
static bool
key_exact(Oid typid, Oid collation)
{
	switch (getBaseType(typid))
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case UUIDOID:
		case NUMERICOID:
			return true;
		case TEXTOID:
		case VARCHAROID:
			return get_collation_isdeterministic(collation);
		default:
			return false;
	}
}

void
sift_f2_unique_check_keys(Relation index)
{
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);

		if (!key_exact(att->atttypid, index->rd_indcollation[i]))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("a UNIQUE sift_col index cannot have the key column \"%s\" of type %s", NameStr(att->attname), format_type_be(att->atttypid)),
							errhint("The key of a unique index is made of smallint, integer, bigint, boolean, date, timestamp, timestamptz, numeric, uuid, text and varchar (deterministic collation) columns; "
									"put other columns in INCLUDE.")));
	}
}

static void
key_init(UniqueKey *k, Relation index)
{
	TupleDesc	desc = RelationGetDescr(index);

	k->n = IndexRelationGetNumberOfKeyAttributes(index);
	k->nulls_not_distinct = index->rd_index->indnullsnotdistinct;
	for (int i = 0; i < k->n; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		Oid			base = getBaseType(att->atttypid);
		TypeCacheEntry *tc = lookup_type_cache(base, TYPECACHE_EQ_OPR_FINFO | TYPECACHE_HASH_PROC_FINFO | TYPECACHE_HASH_EXTENDED_PROC_FINFO | TYPECACHE_LT_OPR);

		k->subtype[i] = base == VARCHAROID ? TEXTOID : base;
		k->collation[i] = index->rd_indcollation[i];
		k->lt[i] = tc->lt_opr;
		k->typlen[i] = base == NUMERICOID ? 0 : att->attlen;
		k->typbyval[i] = att->attbyval;
		fmgr_info_copy(&k->eq[i], &tc->eq_opr_finfo, CurrentMemoryContext);
		fmgr_info_copy(&k->hash[i], &tc->hash_proc_finfo, CurrentMemoryContext);
		fmgr_info_copy(&k->hash64[i], &tc->hash_extended_proc_finfo, CurrentMemoryContext);
	}
}

static bool
key_equal(UniqueKey *k, const Datum *a, const bool *a_null, const Datum *b, const bool *b_null)
{
	for (int i = 0; i < k->n; i++)
	{
		if (a_null[i] || b_null[i])
		{
			if (a_null[i] != b_null[i])
				return false;
			continue;
		}
		if (!DatumGetBool(FunctionCall2Coll(&k->eq[i], k->collation[i], a[i], b[i])))
			return false;
	}
	return true;
}

static bool
key_has_null(const UniqueKey *k, const bool *isnull)
{
	for (int i = 0; i < k->n; i++)
		if (isnull[i])
			return true;
	return false;
}

/* The hash of a key: equal keys hash alike (numeric 1.0 and 1.00, a text by its collation's hash). It picks the lock of the rows of the key and their place in the registry */
static uint32
key_hash(UniqueKey *k, const Datum *values, const bool *isnull)
{
	uint32		h = 0;

	for (int i = 0; i < k->n; i++)
		if (!isnull[i])
			h = hash_combine(h, DatumGetUInt32(FunctionCall1Coll(&k->hash[i], k->collation[i], values[i])));
	return h;
}

static void
registry_key(UniqueKey *k, Relation index, uint32 hash, const Datum *values, const bool *isnull, SiftUniqKey *rk)
{
	sift_uniq_key_begin(rk, &index->rd_locator, hash, k->n);
	for (int i = 0; i < k->n; i++)
		sift_uniq_key_add(rk, values[i], isnull[i], k->typlen[i], k->typbyval[i],
						  isnull[i] ? 0 : DatumGetUInt64(FunctionCall2Coll(&k->hash64[i], k->collation[i], values[i], Int64GetDatum(0))));
}

/* "(col, ...)=(value, ...)" of the key, what BuildIndexValueDescription says of a key whose operator class is not polymorphic (it prints by the type of the class: anyelement here); NULL for a caller who may not read the table */
static char *
key_description(Relation index, Relation heap, const Datum *values, const bool *isnull)
{
	TupleDesc	desc = RelationGetDescr(index);
	StringInfoData names,
				vals;

	if (pg_class_aclcheck(RelationGetRelid(heap), GetUserId(), ACL_SELECT) != ACLCHECK_OK || check_enable_rls(RelationGetRelid(heap), InvalidOid, true) == RLS_ENABLED)
		return NULL;
	initStringInfo(&names);
	initStringInfo(&vals);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		Oid			out;
		bool		varlena;

		getTypeOutputInfo(att->atttypid, &out, &varlena);
		appendStringInfo(&names, "%s%s", i > 0 ? ", " : "", NameStr(att->attname));
		appendStringInfo(&vals, "%s%s", i > 0 ? ", " : "", isnull[i] ? "null" : OidOutputFunctionCall(out, values[i]));
	}
	return psprintf("(%s)=(%s)", names.data, vals.data);
}

static void
key_violation(Relation index, Relation heap, Datum *values, bool *isnull, bool building)
{
	char	   *key = key_description(index, heap, values, isnull);

	ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
					building ? errmsg("could not create unique index \"%s\"", RelationGetRelationName(index)) :
					errmsg("duplicate key value violates unique constraint \"%s\"", RelationGetRelationName(index)),
					key ? errdetail(building ? "Key %s is duplicated." : "Key %s already exists.", key) : 0,
					errtableconstraint(heap, RelationGetRelationName(index))));
}

typedef enum Found
{
	FOUND_NONE,
	FOUND_LIVE,
	FOUND_RUNNING
} Found;

/* The row of the same key as the new one, if there is one: live, or written (or deleted) by a running transaction, which the dirty snapshot names, and the speculative insertion it is in */
typedef struct Conflict
{
	Found		found;
	bool		in_stage;		/* the row is one a running transaction holds in its stage */
	TransactionId xwait;
	uint32		token;
	ItemPointerData at;
} Conflict;

typedef struct Row
{
	Relation	index;
	IndexInfo  *info;
	EState	   *estate;
} Row;

/* The key of the row a slot holds */
static void
row_key(const Row *row, TupleTableSlot *slot, Datum *values, bool *isnull)
{
	if (row->info->ii_Expressions == NIL)
		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(row->index); i++)
			values[i] = slot_getattr(slot, row->index->rd_index->indkey.values[i], &isnull[i]);
	else
	{
		GetPerTupleExprContext(row->estate)->ecxt_scantuple = slot;
		FormIndexDatum(row->info, slot, row->estate, values, isnull);
	}
}

/* Whether the row of the slot has the key */
static bool
row_has_key(UniqueKey *k, const Row *row, TupleTableSlot *slot, const Datum *values, const bool *isnull)
{
	Datum		have[INDEX_MAX_KEYS];
	bool		have_null[INDEX_MAX_KEYS];
	bool		equal;

	row_key(row, slot, have, have_null);
	equal = key_equal(k, have, have_null, values, isnull);
	if (row->estate != NULL)
		ResetExprContext(GetPerTupleExprContext(row->estate));
	return equal;
}

/* What the dirty snapshot saw of the heap tuple at `at` says whether the row is live or a transaction is still to decide it, and whom to wait for */
static void
judge(Conflict *c, const SnapshotData *dirty, const ItemPointerData *at, bool in_stage)
{
	c->xwait = TransactionIdIsValid(dirty->xmin) ? dirty->xmin : dirty->xmax;
	c->token = dirty->speculativeToken;
	c->at = *at;
	c->in_stage = in_stage;
	c->found = TransactionIdIsValid(c->xwait) ? FOUND_RUNNING : FOUND_LIVE;
}

/* The rows of the key that transactions hold in their stages: the chains of the heap tuples of the entries of the registry (an entry names the root of a HOT chain, and the row of the key is its visible member) */
static void
find_staged(UniqueKey *k, const Row *row, Relation heap, const SiftUniqHits *hits, const Datum *values, const bool *isnull, ItemPointer self, Conflict *c)
{
	SnapshotData dirty;
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	IndexFetchTableData *fetch = sift_table_index_fetch_begin(heap);

	InitDirtySnapshot(dirty);
	for (int i = 0; i < hits->n && c->found == FOUND_NONE; i++)
	{
		ItemPointerData at = hits->hit[i].tid;
		bool		again = false,
					all_dead;

		while (c->found == FOUND_NONE && table_index_fetch_tuple(fetch, &at, &dirty, slot, &again, &all_dead))
		{
			if (!ItemPointerEquals(self, &at) && (!hits->hit[i].external || row_has_key(k, row, slot, values, isnull)))
				judge(c, &dirty, &at, true);
			if (!again)
				break;
		}
	}
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
}

typedef struct SlabKey
{
	UniqueKey  *k;
	const Row  *row;
} SlabKey;

/* The 64-bit hash of the key of a row of a slab, as the registry makes it */
static uint64
slab_key_fp(void *arg, TupleTableSlot *slot)
{
	const SlabKey *s = arg;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	SiftUniqKey rk;

	row_key(s->row, slot, values, isnull);
	registry_key(s->k, s->row->index, 0, values, isnull, &rk);
	if (s->row->estate != NULL)
		ResetExprContext(GetPerTupleExprContext(s->row->estate));
	return key_has_null(s->k, isnull) && !s->k->nulls_not_distinct ? 0 : rk.fp64;
}

static bool
plan_reads_nothing(const SiftPlanSnapshot *plan)
{
	for (int i = 0; i < plan->npartitions; i++)
		if (plan->partitions[i].l0_slab_blk != InvalidBlockNumber || plan->partlists[i].nparts > 0)
			return false;
	return true;
}

/* The manifest of the partitions as the key needs it read: without the small parts and the slabs that hold no row of it (f2_uniq_parts.h, f2_uniq_slab.h) */
static const SiftPlanSnapshot *
published_plan(UniqueKey *k, const Row *row, Relation heap, const SiftUniqKey *rk, MemoryContext cx)
{
	SlabKey		sk = {.k = k,.row = row};
	const SiftPlanSnapshot *plan = sift_plan_cache_get(row->index, cx);

	plan = sift_uniq_parts_prune(row->index, heap, plan, rk->fp64, slab_key_fp, &sk, cx);
	return sift_uniq_slab_prune(row->index, heap, plan, rk->fp64, slab_key_fp, &sk, cx);
}

static MemoryContext
probe_context(void)
{
	static MemoryContext cx;

	if (cx == NULL)
		cx = sift_cache_create_context("sift cache unique probe");
	MemoryContextReset(cx);
	return cx;
}

/* The rows in the partitions of plan: a scan of the manifest as it is, which does not flush the stage of this transaction (its rows are in the registry) */
static void
scan_published(UniqueKey *k, const Row *row, Relation heap, const SiftPlanSnapshot *plan, const Datum *values, const bool *isnull, ItemPointer self, Conflict *c)
{
	SnapshotData dirty;
	ScanKeyData keys[INDEX_MAX_KEYS];
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	IndexScanDesc scan;

	InitDirtySnapshot(dirty);
	for (int i = 0; i < k->n; i++)
		ScanKeyEntryInitialize(&keys[i], isnull[i] ? SK_ISNULL | SK_SEARCHNULL : 0, i + 1, BTEqualStrategyNumber, k->subtype[i], k->collation[i], isnull[i] ? InvalidOid : k->eq[i].fn_oid,
							   isnull[i] ? (Datum) 0 : values[i]);
	scan = sift_index_beginscan(heap, row->index, &dirty, k->n);
	index_rescan(scan, keys, k->n, NULL, 0);
	sift_f2_scan_set_plan(scan, plan);
	while (c->found == FOUND_NONE && index_getnext_slot(scan, ForwardScanDirection, slot))
		if (!ItemPointerEquals(self, &slot->tts_tid) && row_has_key(k, row, slot, values, isnull))
			judge(c, &dirty, &slot->tts_tid, false);
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
}

static void
find_published(UniqueKey *k, const Row *row, Relation heap, const SiftUniqKey *rk, const Datum *values, const bool *isnull, ItemPointer self, Conflict *c)
{
	MemoryContext cx = probe_context();
	const SiftPlanSnapshot *plan = published_plan(k, row, heap, rk, cx);

	if (!plan_reads_nothing(plan))
		scan_published(k, row, heap, plan, values, isnull, self, c);
	MemoryContextReset(cx);
}

/* The rows of this transaction's stage go to the partition, and then its entries of the index leave the registry */
static void
spill(Relation index)
{
	sift_stage_flush(index);
	sift_uniq_shm_release(&index->rd_locator);
}

/* The entries of the registry of the key; a stripe that holds more than a probe reports is emptied first (the rows of this transaction go to the partition) */
static void
probe_staged(Relation index, const SiftUniqKey *rk, SiftUniqHits *hits)
{
	sift_uniq_shm_probe(rk, hits);
	if (hits->overflow)
	{
		spill(index);
		sift_uniq_shm_probe(rk, hits);
		if (hits->overflow)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("too many transactions hold a row of one key of the unique index \"%s\"", RelationGetRelationName(index))));
	}
}

/* A row of the same key as the new one, if there is one: in the registry first, in the partitions after */
static void
find_conflict(UniqueKey *k, Relation index, Relation heap, IndexInfo *info, const SiftUniqKey *rk, const Datum *values, const bool *isnull, ItemPointer self, Conflict *c)
{
	Row			row = {.index = index,.info = info,.estate = info->ii_Expressions != NIL ? CreateExecutorState() : NULL};
	SiftUniqHits hits;

	*c = (Conflict) {.found = FOUND_NONE};
	probe_staged(index, rk, &hits);
	find_staged(k, &row, heap, &hits, values, isnull, self, c);
	if (c->found == FOUND_NONE)
		find_published(k, &row, heap, rk, values, isnull, self, c);
	if (row.estate != NULL)
		FreeExecutorState(row.estate);
}

bool
sift_f2_unique_lookup(IndexScanDesc scan, MemoryContext cx, SiftUniqLookup *out)
{
	Relation	index = scan->indexRelation;
	UniqueKey	k;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	SiftUniqKey rk;
	Row			row;

	if (scan->xs_snapshot == NULL || scan->xs_snapshot->snapshot_type != SNAPSHOT_DIRTY || scan->heapRelation == NULL || scan->xs_want_itup || !index->rd_index->indisunique ||
		scan->numberOfKeys != IndexRelationGetNumberOfKeyAttributes(index))
		return false;
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		const ScanKeyData *sk = &scan->keyData[i];

		if (sk->sk_attno != i + 1 || sk->sk_strategy != BTEqualStrategyNumber)
			return false;
		isnull[i] = (sk->sk_flags & SK_ISNULL) != 0;
		values[i] = isnull[i] ? (Datum) 0 : sk->sk_argument;
	}
	key_init(&k, index);
	if (key_has_null(&k, isnull) && !k.nulls_not_distinct)
		return false;
	registry_key(&k, index, key_hash(&k, values, isnull), values, isnull, &rk);
	probe_staged(index, &rk, &out->hits);
	row = (Row) {.index = index,.info = BuildIndexInfo(index)};
	row.estate = row.info->ii_Expressions != NIL ? CreateExecutorState() : NULL;
	out->plan = published_plan(&k, &row, scan->heapRelation, &rk, cx);
	if (row.estate != NULL)
		FreeExecutorState(row.estate);
	return true;
}

/* The row of a key nobody holds goes to the stage, its key to the registry first; a stripe with no entry free sends the rows of the stage to the partition, and the entries of the index go */
static void
write_row(Relation index, const SiftUniqKey *rk, Datum *values, bool *isnull, ItemPointer heap_tid)
{
	if (!sift_uniq_shm_reserve(rk, GetCurrentTransactionId(), heap_tid))
	{
		spill(index);
		if (!sift_uniq_shm_reserve(rk, GetCurrentTransactionId(), heap_tid))
		{
			sift_f2_stage_row(index, values, isnull, heap_tid);
			sift_stage_flush(index);
			return;
		}
	}
	sift_f2_stage_row(index, values, isnull, heap_tid);
}

bool
sift_f2_unique_insert(Relation index, Relation heap, Datum *values, bool *isnull, ItemPointer heap_tid, IndexUniqueCheck check, IndexInfo *info)
{
	UniqueKey	k;
	SiftUniqKey rk;
	uint32		hash;
	BlockNumber lock;
	bool		write = check != UNIQUE_CHECK_EXISTING;

	key_init(&k, index);
	if (key_has_null(&k, isnull) && !k.nulls_not_distinct)
	{
		if (write)
			sift_f2_stage_row(index, values, isnull, heap_tid);
		return true;
	}
	hash = key_hash(&k, values, isnull);
	registry_key(&k, index, hash, values, isnull, &rk);
	lock = SIFT_UNIQUE_LOCK_BASE + hash % SIFT_UNIQUE_LOCK_SLOTS;
	for (;;)
	{
		Conflict	c;

		LockPage(index, lock, ExclusiveLock);
		find_conflict(&k, index, heap, info, &rk, values, isnull, heap_tid, &c);
		if (c.found == FOUND_NONE && write)
			write_row(index, &rk, values, isnull, heap_tid);
		UnlockPage(index, lock, ExclusiveLock);
		if (c.found == FOUND_NONE)
			return true;
		if (check == UNIQUE_CHECK_PARTIAL && !(c.found == FOUND_RUNNING && c.in_stage))
			return false;
		if (c.found == FOUND_LIVE)
			key_violation(index, heap, values, isnull, false);
		if (c.token != 0)
			SpeculativeInsertionWait(c.xwait, c.token);
		else
			XactLockTableWait(c.xwait, heap, &c.at, XLTW_InsertIndex);
	}
}

/* ---- the check of a build ---- */

struct SiftF2UniqueBuild
{
	Relation	heap;
	Relation	index;
	UniqueKey	key;
	TupleDesc	desc;
	Tuplesortstate *sort;
	TupleTableSlot *put;
};

SiftF2UniqueBuild *
sift_f2_unique_build_begin(Relation heap, Relation index)
{
	SiftF2UniqueBuild *u;
	AttrNumber	cols[INDEX_MAX_KEYS];
	bool		nulls_first[INDEX_MAX_KEYS] = {false};

	if (!index->rd_index->indisunique)
		return NULL;
	sift_f2_unique_check_keys(index);
	u = palloc0(sizeof(SiftF2UniqueBuild));
	u->heap = heap;
	u->index = index;
	key_init(&u->key, index);
	u->desc = CreateTemplateTupleDesc(u->key.n);
	for (int i = 0; i < u->key.n; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);

		cols[i] = i + 1;
		TupleDescInitEntry(u->desc, i + 1, "k", att->atttypid, att->atttypmod, 0);
		TupleDescInitEntryCollation(u->desc, i + 1, u->key.collation[i]);
	}
	sift_tupdesc_finalize(u->desc);
	u->sort = tuplesort_begin_heap(u->desc, u->key.n, cols, u->key.lt, u->key.collation, nulls_first, maintenance_work_mem, NULL, TUPLESORT_NONE);
	u->put = MakeSingleTupleTableSlot(u->desc, &TTSOpsVirtual);
	return u;
}

void
sift_f2_unique_build_add(SiftF2UniqueBuild *u, const Datum *values, const bool *isnull)
{
	if (key_has_null(&u->key, isnull) && !u->key.nulls_not_distinct)
		return;
	ExecClearTuple(u->put);
	memcpy(u->put->tts_values, values, u->key.n * sizeof(Datum));
	memcpy(u->put->tts_isnull, isnull, u->key.n * sizeof(bool));
	ExecStoreVirtualTuple(u->put);
	tuplesort_puttupleslot(u->sort, u->put);
}

void
sift_f2_unique_build_end(SiftF2UniqueBuild *u)
{
	TupleTableSlot *prev = MakeSingleTupleTableSlot(u->desc, &TTSOpsMinimalTuple),
			   *cur = MakeSingleTupleTableSlot(u->desc, &TTSOpsMinimalTuple);
	bool		have = false;

	tuplesort_performsort(u->sort);
	while (tuplesort_gettupleslot(u->sort, true, true, cur, NULL))
	{
		slot_getallattrs(cur);
		if (have)
		{
			slot_getallattrs(prev);
			if (key_equal(&u->key, prev->tts_values, prev->tts_isnull, cur->tts_values, cur->tts_isnull))
				key_violation(u->index, u->heap, cur->tts_values, cur->tts_isnull, true);
		}
		have = true;
		{
			TupleTableSlot *t = prev;

			prev = cur;
			cur = t;
		}
	}
	ExecDropSingleTupleTableSlot(prev);
	ExecDropSingleTupleTableSlot(cur);
	ExecDropSingleTupleTableSlot(u->put);
	tuplesort_end(u->sort);
}
