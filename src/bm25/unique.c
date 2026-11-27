/*
 * bm25/unique.c - UNIQUE on a sift index. The key of a row is a term of its postings (bm25/doc.h), so the rows that may have the same key are the
 * postings of one term; the heap says which of them are live and whether their key is the same (SnapshotDirty, as a btree waits for the transaction of
 * a row it finds that is not yet committed). Inserts of one key are serialized by a tuple lock of the index named by 48 bits of the key's hash (no index
 * tuple has that address, and writers of other keys almost never share it), taken from the check until the row's own posting is written. The rows of a
 * CREATE INDEX are told apart by a table of their keys instead, before anything is written.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "bm25/doc.h"
#include "bm25/eval/eval.h"
#include "bm25/query/int.h"
#include "core/compat.h"
#include "core/memtrack.h"

#define DETAIL_BYTES		256

typedef struct Prober
{
	Relation	index;
	Relation	heap;
	IndexInfo  *info;
	EState	   *estate;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	ExprState  *predicate;
} Prober;

typedef enum ProbeResult
{
	PROBE_FREE,
	PROBE_DUPLICATE,
	PROBE_WAIT
} ProbeResult;

static ItemPointerData
key_lock(uint64 key)
{
	ItemPointerData tid;

	ItemPointerSet(&tid, (BlockNumber) (key >> 16), (OffsetNumber) (key % PG_UINT16_MAX + 1));
	return tid;
}

static void
prober_open(Prober *p, Relation heap, Relation index)
{
	p->index = index;
	p->heap = heap;
	p->info = BuildIndexInfo(index);
	p->estate = CreateExecutorState();
	p->slot = table_slot_create(heap, NULL);
	p->fetch = sift_table_index_fetch_begin(heap);
	p->predicate = p->info->ii_Predicate ? ExecPrepareQual(p->info->ii_Predicate, p->estate) : NULL;
}

static void
prober_close(Prober *p)
{
	table_index_fetch_end(p->fetch);
	ExecDropSingleTupleTableSlot(p->slot);
	FreeExecutorState(p->estate);
}

static bool
same_key(const Prober *p, const Datum *a, const bool *anull, const Datum *b, const bool *bnull)
{
	for (int k = 0; k < IndexRelationGetNumberOfKeyAttributes(p->index); k++)
	{
		TypeCacheEntry *tc;

		if (anull[k] || bnull[k])
		{
			if (anull[k] != bnull[k])
				return false;
			continue;
		}
		tc = lookup_type_cache(TupleDescAttr(RelationGetDescr(p->index), k)->atttypid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(tc->eq_opr))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s", format_type_be(tc->type_id))));
		if (!DatumGetBool(FunctionCall2Coll(&tc->eq_opr_finfo, p->index->rd_indcollation[k], a[k], b[k])))
			return false;
	}
	return true;
}

/*
 * Does the row at cand have the key, in the version a dirty snapshot sees? A version of an open transaction is waited for: the insert of one before its key is read, as a sift_table hands
 * over a row a running insert staged with no values (table/f2/staged.c).
 */
static ProbeResult
probe_row(Prober *p, const ItemPointerData *cand, const Datum *values, const bool *isnull, TransactionId *xwait, uint32 *token)
{
	SnapshotData dirty;
	ItemPointerData tid = *cand;
	Datum		rvalues[INDEX_MAX_KEYS];
	bool		rnull[INDEX_MAX_KEYS],
				again = false,
				all_dead = false;
	ExprContext *econtext = GetPerTupleExprContext(p->estate);

	InitDirtySnapshot(dirty);
	if (!table_index_fetch_tuple(p->fetch, &tid, &dirty, p->slot, &again, &all_dead))
		return PROBE_FREE;
	if (!TransactionIdIsValid(dirty.xmin))
	{
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = p->slot;
		if (p->predicate && !ExecQual(p->predicate, econtext))
			return PROBE_FREE;
		FormIndexDatum(p->info, p->slot, p->estate, rvalues, rnull);
		if (!same_key(p, values, isnull, rvalues, rnull))
			return PROBE_FREE;
		if (!TransactionIdIsValid(dirty.xmax))
			return PROBE_DUPLICATE;
	}
	*xwait = TransactionIdIsValid(dirty.xmin) ? dirty.xmin : dirty.xmax;
	*token = dirty.speculativeToken;
	return PROBE_WAIT;
}

/*
 * The segments a probe of a key looks at: all of them, the immutable ones (seen NULL), or under the key lock the others: the write buffers, where a row of the key
 * that is being written is, and the immutable segments that were not in seen, which a promotion or a merge wrote since (they may hold a row of a buffer seen gone)
 */
typedef struct SegFilter
{
	bool		all;
	const SiftDirSnapshot *seen;
	const SiftDirSnapshot *read;	/* out: the segments the probe read */
} SegFilter;

static bool
seen_gen(const SiftDirSnapshot *seen, uint64 gen)
{
	for (int i = 0; i < seen->nsegs; i++)
		if (seen->segs[i].gen == gen)
			return true;
	return false;
}

static bool
seg_kept(const SegFilter *f, const SiftSegEntry *e)
{
	if (f->all)
		return true;
	if (f->seen == NULL)
		return e->kind == SIFT_SEG_IMMUTABLE;
	return e->kind != SIFT_SEG_IMMUTABLE || !seen_gen(f->seen, e->gen);
}

/* The state keeps its own copy of the directory, whose segments are opened when first read: the ones the filter drops are left out of it before */
static SiftIndexState *
state_of(Relation index, SegFilter *f, MemoryContext cx)
{
	SiftIndexState *st = sift_state_open_terms(index, cx);
	int			n = 0;

	for (int i = 0; i < st->dir->nsegs; i++)
		if (seg_kept(f, &st->dir->segs[i]))
			st->dir->segs[n++] = st->dir->segs[i];
	st->dir->nsegs = n;
	f->read = st->dir;
	return st;
}

/* The entries of the postings of the key's term in the segments of f, the rows that may have the key */
static List *
candidates(Relation index, uint64 key, SegFilter *f, MemoryContext cx)
{
	SiftIndexState *st = state_of(index, f, cx);
	SiftQNode  *leaf = sift_query_new_node(SQ_TERM, 1);
	SiftQuery	q = {.root = leaf,.nleaves = 1,.analyzed = true};
	SiftQuery  *queries[1] = {&q};
	SiftMatchIter *it;
	SiftPageMatch pm;
	List	   *found = NIL;

	leaf->text = palloc0(10);
	for (int i = 0; i < 8; i++)
		leaf->text[1 + i] = (char) (key >> (56 - 8 * i));
	leaf->textlen = 9;
	leaf->leafno = 0;
	it = sift_iter_begin(sift_plan_bind(st, queries, 1, SIFT_BIND_NO_READS));
	while (sift_iter_next(it, &pm))
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = pm.offmask[wd]; m; m &= m - 1)
			{
				ItemPointer t = palloc(sizeof(ItemPointerData));

				ItemPointerSet(t, pm.blk, wd * 64 + pg_rightmost_one_pos64(m) + 1);
				found = lappend(found, t);
			}
	sift_iter_end(it);
	sift_state_close(st);
	return found;
}


/* The key as BuildIndexValueDescription writes it, which cannot write a column of a class of any type; for a user who may read the table and whom no row-level security policy limits */
static char *
key_detail(Relation heap, Relation index, const Datum *values, const bool *isnull)
{
	StringInfoData cols,
				vals;
	char	   *d;

	if (check_enable_rls(RelationGetRelid(heap), InvalidOid, true) == RLS_ENABLED ||
		pg_class_aclcheck(RelationGetRelid(heap), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return NULL;
	initStringInfo(&cols);
	initStringInfo(&vals);
	for (int k = 0; k < IndexRelationGetNumberOfKeyAttributes(index); k++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), k);
		Oid			outfunc;
		bool		isvarlena;

		appendStringInfo(&cols, "%s%s", k ? ", " : "", NameStr(att->attname));
		if (isnull[k])
			appendStringInfo(&vals, "%snull", k ? ", " : "");
		else
		{
			getTypeOutputInfo(att->atttypid, &outfunc, &isvarlena);
			appendStringInfo(&vals, "%s%s", k ? ", " : "", OidOutputFunctionCall(outfunc, values[k]));
		}
	}
	d = psprintf("(%s)=(%s)", cols.data, vals.data);
	if (strlen(d) > DETAIL_BYTES)
	{
		d[pg_mbcliplen(d, strlen(d), DETAIL_BYTES)] = '\0';
		d = psprintf("%s...", d);
	}
	return d;
}

static void
duplicate(Relation heap, Relation index, const Datum *values, const bool *isnull, bool building)
{
	char	   *detail = key_detail(heap, index, values, isnull);

	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 building ? errmsg("could not create unique index \"%s\"", RelationGetRelationName(index))
			 : errmsg("duplicate key value violates unique constraint \"%s\"", RelationGetRelationName(index)),
			 detail ? errdetail(building ? "Key %s is duplicated." : "Key %s already exists.", detail) : 0,
			 errtableconstraint(heap, RelationGetRelationName(index))));
}

/* A live duplicate of the row in the segments of f (the first found), or the transaction to wait for, and the row it inserted, when the only ones are not committed */
static bool
find_duplicate(Prober *p, uint64 key, SegFilter *f, const Datum *values, const bool *isnull, ItemPointer self, TransactionId *xwait, ItemPointer at, uint32 *token)
{
	foreach_ptr(ItemPointerData, cand, candidates(p->index, key, f, CurrentMemoryContext))
	{
		const TransactionId before = *xwait;
		ProbeResult r;

		if (ItemPointerEquals(cand, self))
			continue;
		r = probe_row(p, cand, values, isnull, xwait, token);
		if (*xwait != before)
			*at = *cand;
		if (r == PROBE_DUPLICATE)
		{
			*xwait = InvalidTransactionId;
			return true;
		}
	}
	return false;
}

/*
 * The immutable segments are probed before the key lock is taken: a row enters them only from a write buffer, where the probe under the lock finds it, or by a promotion or a
 * merge that writes a segment the first probe did not see, which the probe under the lock reads too. The key lock is held over the write buffers only.
 */
bool
sift_bm25_unique_check(Relation index, Relation heap, const Datum *values, const bool *isnull,
					   ItemPointer tid, IndexUniqueCheck check, uint64 key)
{
	Prober		p;
	bool		duplicated;
	ItemPointerData lock = key_lock(key);

	prober_open(&p, heap, index);
	for (;;)
	{
		TransactionId xwait = InvalidTransactionId;
		ItemPointerData at;
		uint32		token = 0;
		SegFilter	immutable = {0};

		CHECK_FOR_INTERRUPTS();
		duplicated = find_duplicate(&p, key, &immutable, values, isnull, tid, &xwait, &at, &token);
		if (!TransactionIdIsValid(xwait))
		{
			LockTuple(index, &lock, ExclusiveLock);
			if (!duplicated)
				duplicated = find_duplicate(&p, key, &(SegFilter) {.seen = immutable.read}, values, isnull, tid, &xwait, &at, &token);
			if (!TransactionIdIsValid(xwait))
				break;
			UnlockTuple(index, &lock, ExclusiveLock);
		}
		if (token != 0)
			SpeculativeInsertionWait(xwait, token);
		else
			XactLockTableWait(xwait, heap, &at, XLTW_InsertIndex);
	}
	prober_close(&p);
	if (check == UNIQUE_CHECK_EXISTING)
		UnlockTuple(index, &lock, ExclusiveLock);
	if (duplicated && check == UNIQUE_CHECK_YES)
		duplicate(heap, index, values, isnull, false);
	if (duplicated && check == UNIQUE_CHECK_EXISTING)
		duplicate(heap, index, values, isnull, false);
	return !duplicated;
}

/* The rows a speculative pre-check (INSERT ... ON CONFLICT) must look at: those a dirty snapshot sees with the key of the row about to be inserted, a row of an open transaction included */
List *
sift_bm25_unique_twins(Relation index, Relation heap, const ScanKeyData *keys, int nkeys)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	List	   *twins = NIL;
	uint64		key;
	Prober		p;

	for (int k = 0; k < nkeys; k++)
	{
		values[keys[k].sk_attno - 1] = keys[k].sk_argument;
		isnull[keys[k].sk_attno - 1] = (keys[k].sk_flags & SK_ISNULL) != 0;
	}
	if (!sift_bm25_unique_key(index, values, isnull, &key))
		return NIL;
	prober_open(&p, heap, index);
	foreach_ptr(ItemPointerData, cand, candidates(index, key, &(SegFilter) {.all = true}, CurrentMemoryContext))
	{
		TransactionId xwait;
		uint32		token;

		if (probe_row(&p, cand, values, isnull, &xwait, &token) != PROBE_FREE)
			twins = lappend(twins, cand);
	}
	prober_close(&p);
	return twins;
}

void
sift_bm25_unique_release(Relation index, uint64 key)
{
	ItemPointerData lock = key_lock(key);

	UnlockTuple(index, &lock, ExclusiveLock);
}

/* The rows of a build sorted by key, in a share of the maintenance memory beside the buffers of the build: a key two rows have is checked once the scan ends */
struct SiftUniqueBuild
{
	Prober		p;
	Prober		row;			/* fetches the row whose key an earlier row had */
	TupleDesc	desc;			/* (key, tid) */
	Tuplesortstate *sort;
	TupleTableSlot *put;
};

SiftUniqueBuild *
sift_unique_build_begin(Relation heap, Relation index, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftUniqueBuild *ub = palloc0(sizeof(SiftUniqueBuild));
	AttrNumber	col = 1;
	Oid			lt = Int8LessOperator,
				coll = InvalidOid;
	bool		nulls_first = false;

	ub->desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(ub->desc, 1, "key", INT8OID, -1, 0);
	TupleDescInitEntry(ub->desc, 2, "tid", TIDOID, -1, 0);
	sift_tupdesc_finalize(ub->desc);
	ub->sort = tuplesort_begin_heap(ub->desc, 1, &col, &lt, &coll, &nulls_first,
									(int) (sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 8) / 1024), NULL, TUPLESORT_NONE);
	ub->put = MakeSingleTupleTableSlot(ub->desc, &TTSOpsVirtual);
	prober_open(&ub->p, heap, index);
	prober_open(&ub->row, heap, index);
	MemoryContextSwitchTo(old);
	return ub;
}

void
sift_unique_build_add(SiftUniqueBuild *ub, ItemPointer tid, uint64 key, const Datum *values, const bool *isnull)
{
	ExecClearTuple(ub->put);
	ub->put->tts_values[0] = Int64GetDatum((int64) key);
	ub->put->tts_values[1] = ItemPointerGetDatum(tid);
	ub->put->tts_isnull[0] = ub->put->tts_isnull[1] = false;
	ExecStoreVirtualTuple(ub->put);
	tuplesort_puttupleslot(ub->sort, ub->put);
}

/* The row at tid against the rows before it with its key: one that has its values too is a duplicate */
static void
check_twins(SiftUniqueBuild *ub, ItemPointerData tid, const List *same)
{
	SnapshotData dirty;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS],
				again = false,
				all_dead = false;
	ExprContext *econtext = GetPerTupleExprContext(ub->row.estate);

	InitDirtySnapshot(dirty);
	if (!table_index_fetch_tuple(ub->row.fetch, &tid, &dirty, ub->row.slot, &again, &all_dead))
		return;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = ub->row.slot;
	FormIndexDatum(ub->row.info, ub->row.slot, ub->row.estate, values, isnull);
	foreach_ptr(ItemPointerData, twin, same)
	{
		TransactionId xwait;
		uint32		token;

		if (probe_row(&ub->p, twin, values, isnull, &xwait, &token) != PROBE_FREE)
			duplicate(ub->p.heap, ub->p.index, values, isnull, true);
	}
}

void
sift_unique_build_end(SiftUniqueBuild *ub)
{
	TupleTableSlot *got = MakeSingleTupleTableSlot(ub->desc, &TTSOpsMinimalTuple);
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift unique key"),
				old;
	List	   *same = NIL;
	ItemPointer twin;
	int64		key = 0;
	bool		isnull;

	tuplesort_performsort(ub->sort);
	while (tuplesort_gettupleslot(ub->sort, true, false, got, NULL))
	{
		int64		k = DatumGetInt64(slot_getattr(got, 1, &isnull));
		ItemPointerData tid = *DatumGetItemPointer(slot_getattr(got, 2, &isnull));

		CHECK_FOR_INTERRUPTS();
		if (same != NIL && k == key)
			check_twins(ub, tid, same);
		else
		{
			MemoryContextReset(cx);
			same = NIL;
			key = k;
		}
		old = MemoryContextSwitchTo(cx);
		twin = palloc(sizeof(ItemPointerData));
		*twin = tid;
		same = lappend(same, twin);
		MemoryContextSwitchTo(old);
	}
	MemoryContextDelete(cx);
	ExecDropSingleTupleTableSlot(got);
	ExecDropSingleTupleTableSlot(ub->put);
	tuplesort_end(ub->sort);
	prober_close(&ub->row);
	prober_close(&ub->p);
}
