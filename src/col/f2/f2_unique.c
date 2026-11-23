/*
 * col/f2/f2_unique.c - UNIQUE indexes of format major 2.
 *
 * A row is looked up by its key the way PostgreSQL looks one up for an exclusion constraint: an index scan with a dirty snapshot (the rows of a running transaction count, an aborted one's do not),
 * every row it finds judged by the heap tuple it points at. That needs the rows of other transactions to be in the index before they commit, so a unique index never stages a row: aminsert
 * writes it to the partition (sift_stage_flush) at once, and the rows of an aborted (sub)transaction are dead to every snapshot by their transaction, with nothing to undo.
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
#include "col/f2/f2_unique.h"
#include "col/f2/l0_stage.h"
#include "core/part_lock.h"

typedef struct UniqueKey
{
	int			n;
	bool		nulls_not_distinct;
	Oid			subtype[INDEX_MAX_KEYS];	/* the type of the constant of the scan key (a varchar is looked up as the text it is stored as) */
	Oid			collation[INDEX_MAX_KEYS];
	Oid			lt[INDEX_MAX_KEYS];
	FmgrInfo	eq[INDEX_MAX_KEYS];
	FmgrInfo	hash[INDEX_MAX_KEYS];
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
		Oid			base = getBaseType(TupleDescAttr(desc, i)->atttypid);
		TypeCacheEntry *tc = lookup_type_cache(base, TYPECACHE_EQ_OPR_FINFO | TYPECACHE_HASH_PROC_FINFO | TYPECACHE_LT_OPR);

		k->subtype[i] = base == VARCHAROID ? TEXTOID : base;
		k->collation[i] = index->rd_indcollation[i];
		k->lt[i] = tc->lt_opr;
		fmgr_info_copy(&k->eq[i], &tc->eq_opr_finfo, CurrentMemoryContext);
		fmgr_info_copy(&k->hash[i], &tc->hash_proc_finfo, CurrentMemoryContext);
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

/* The lock of the rows of one key: equal keys hash alike (numeric 1.0 and 1.00, a text by its collation's hash) */
static BlockNumber
key_lock(UniqueKey *k, const Datum *values, const bool *isnull)
{
	uint32		h = 0;

	for (int i = 0; i < k->n; i++)
		if (!isnull[i])
			h = hash_combine(h, DatumGetUInt32(FunctionCall1Coll(&k->hash[i], k->collation[i], values[i])));
	return SIFT_UNIQUE_LOCK_BASE + h % SIFT_UNIQUE_LOCK_SLOTS;
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

/* The row goes to the partition at once, where the lookups of the other transactions find it */
static void
publish(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid)
{
	sift_f2_stage_row(index, values, isnull, heap_tid);
	sift_stage_flush(index);
}

typedef enum Found
{
	FOUND_NONE,
	FOUND_LIVE,
	FOUND_RUNNING
} Found;

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

/* A row of the same key as the new one, live or written by a running transaction: what the dirty snapshot saw of it says which (and whom to wait for) */
static Found
find_row(UniqueKey *k, Relation index, Relation heap, IndexInfo *info, const Datum *values, const bool *isnull, ItemPointer self, TransactionId *xwait, uint32 *token, ItemPointerData *at)
{
	SnapshotData dirty;
	ScanKeyData keys[INDEX_MAX_KEYS];
	Row			row = {.index = index,.info = info,.estate = info->ii_Expressions != NIL ? CreateExecutorState() : NULL};
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	IndexScanDesc scan;
	Found		found = FOUND_NONE;

	InitDirtySnapshot(dirty);
	for (int i = 0; i < k->n; i++)
		ScanKeyEntryInitialize(&keys[i], isnull[i] ? SK_ISNULL | SK_SEARCHNULL : 0, i + 1, BTEqualStrategyNumber, k->subtype[i], k->collation[i], isnull[i] ? InvalidOid : k->eq[i].fn_oid,
							   isnull[i] ? (Datum) 0 : values[i]);
	scan = index_beginscan(heap, index, &dirty, NULL, k->n, 0);
	index_rescan(scan, keys, k->n, NULL, 0);
	while (found == FOUND_NONE && index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		Datum		have[INDEX_MAX_KEYS];
		bool		have_null[INDEX_MAX_KEYS];

		if (ItemPointerEquals(self, &slot->tts_tid))
			continue;
		row_key(&row, slot, have, have_null);
		if (row.estate != NULL)
			ResetExprContext(GetPerTupleExprContext(row.estate));
		if (!key_equal(k, have, have_null, values, isnull))
			continue;
		*xwait = TransactionIdIsValid(dirty.xmin) ? dirty.xmin : dirty.xmax;
		*token = dirty.speculativeToken;
		*at = slot->tts_tid;
		found = TransactionIdIsValid(*xwait) ? FOUND_RUNNING : FOUND_LIVE;
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	if (row.estate != NULL)
		FreeExecutorState(row.estate);
	return found;
}

bool
sift_f2_unique_insert(Relation index, Relation heap, Datum *values, bool *isnull, ItemPointer heap_tid, IndexUniqueCheck check, IndexInfo *info)
{
	UniqueKey	k;
	BlockNumber lock;
	bool		write = check != UNIQUE_CHECK_EXISTING;

	key_init(&k, index);
	if (key_has_null(&k, isnull) && !k.nulls_not_distinct)
	{
		if (write)
			publish(index, values, isnull, heap_tid);
		return true;
	}
	lock = key_lock(&k, values, isnull);
	for (;;)
	{
		TransactionId xwait = InvalidTransactionId;
		uint32		token = 0;
		ItemPointerData at;
		Found		found;

		LockPage(index, lock, ExclusiveLock);
		found = find_row(&k, index, heap, info, values, isnull, heap_tid, &xwait, &token, &at);
		if (found == FOUND_NONE || check == UNIQUE_CHECK_PARTIAL)
		{
			if (write)
				publish(index, values, isnull, heap_tid);
			UnlockPage(index, lock, ExclusiveLock);
			return found == FOUND_NONE;
		}
		UnlockPage(index, lock, ExclusiveLock);
		if (found == FOUND_LIVE)
			key_violation(index, heap, values, isnull, false);
		if (token != 0)
			SpeculativeInsertionWait(xwait, token);
		else
			XactLockTableWait(xwait, heap, &at, XLTW_InsertIndex);
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
