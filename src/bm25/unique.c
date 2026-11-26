/*
 * bm25/unique.c - UNIQUE on a sift index. The key of a row is a term of its postings (bm25/doc.h), so the rows that may have the same key are the
 * postings of one term; the heap says which of them are live and whether their key is the same (SnapshotDirty, as a btree waits for the transaction of
 * a row it finds that is not yet committed). Inserts of one key are serialized by a page lock of the index, taken from the check until the row's own
 * posting is written. The rows of a CREATE INDEX are told apart by a table of their keys instead, before anything is written.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

#include "bm25/doc.h"
#include "bm25/eval/eval.h"
#include "bm25/query/int.h"
#include "core/compat.h"

#define UNIQUE_LOCK_BASE	0xFFFFB000U
#define UNIQUE_LOCK_SLOTS	8192U
#define DETAIL_BYTES		256

StaticAssertDecl(UNIQUE_LOCK_BASE + UNIQUE_LOCK_SLOTS < InvalidBlockNumber, "the unique locks are valid block numbers");

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

static BlockNumber
bucket(uint64 key)
{
	return UNIQUE_LOCK_BASE + (BlockNumber) (key % UNIQUE_LOCK_SLOTS);
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

/* Does the row at cand have the key, in the version a dirty snapshot sees? A version of an open transaction is waited for */
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
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = p->slot;
	if (p->predicate && !ExecQual(p->predicate, econtext))
		return PROBE_FREE;
	FormIndexDatum(p->info, p->slot, p->estate, rvalues, rnull);
	if (!same_key(p, values, isnull, rvalues, rnull))
		return PROBE_FREE;
	if (!TransactionIdIsValid(dirty.xmin) && !TransactionIdIsValid(dirty.xmax))
		return PROBE_DUPLICATE;
	*xwait = TransactionIdIsValid(dirty.xmin) ? dirty.xmin : dirty.xmax;
	*token = dirty.speculativeToken;
	return PROBE_WAIT;
}

/* The entries of the postings of the key's term, the rows that may have the key: written in the write buffers or in a segment */
static List *
candidates(Relation index, uint64 key, MemoryContext cx)
{
	SiftIndexState *st = sift_state_open_terms(index, cx);
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

/* The key as BuildIndexValueDescription writes it, which cannot write a column of a class of any type; for the columns the user may read */
static char *
key_detail(Relation heap, Relation index, const Datum *values, const bool *isnull)
{
	StringInfoData cols,
				vals;
	char	   *d;

	if (pg_class_aclcheck(RelationGetRelid(heap), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
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

/* A live duplicate of the row (the first found), or the transaction to wait for, and the row it inserted, when the only ones are not committed */
static bool
find_duplicate(Prober *p, uint64 key, const Datum *values, const bool *isnull, ItemPointer self, TransactionId *xwait, ItemPointer at, uint32 *token)
{
	foreach_ptr(ItemPointerData, cand, candidates(p->index, key, CurrentMemoryContext))
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

bool
sift_bm25_unique_check(Relation index, Relation heap, const Datum *values, const bool *isnull,
					   ItemPointer tid, IndexUniqueCheck check, uint64 key)
{
	Prober		p;
	bool		duplicated;

	prober_open(&p, heap, index);
	for (;;)
	{
		TransactionId xwait = InvalidTransactionId;
		ItemPointerData at;
		uint32		token = 0;

		LockPage(index, bucket(key), ExclusiveLock);
		duplicated = find_duplicate(&p, key, values, isnull, tid, &xwait, &at, &token);
		if (!TransactionIdIsValid(xwait))
			break;
		UnlockPage(index, bucket(key), ExclusiveLock);
		if (token != 0)
			SpeculativeInsertionWait(xwait, token);
		else
			XactLockTableWait(xwait, heap, &at, XLTW_InsertIndex);
	}
	prober_close(&p);
	if (check == UNIQUE_CHECK_EXISTING)
		UnlockPage(index, bucket(key), ExclusiveLock);
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
	foreach_ptr(ItemPointerData, cand, candidates(index, key, CurrentMemoryContext))
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
	UnlockPage(index, bucket(key), ExclusiveLock);
}

typedef struct KeyEntry
{
	uint64		key;
	int			ntids;
	int			cap;
	ItemPointerData *tids;
} KeyEntry;

struct SiftUniqueBuild
{
	Prober		p;
	HTAB	   *seen;
	MemoryContext cx;
};

SiftUniqueBuild *
sift_unique_build_begin(Relation heap, Relation index, MemoryContext cx)
{
	SiftUniqueBuild *ub = MemoryContextAllocZero(cx, sizeof(SiftUniqueBuild));
	HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(KeyEntry),.hcxt = cx};

	ub->cx = cx;
	ub->seen = hash_create("sift unique keys", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	prober_open(&ub->p, heap, index);
	return ub;
}

void
sift_unique_build_add(SiftUniqueBuild *ub, ItemPointer tid, uint64 key, const Datum *values, const bool *isnull)
{
	bool		found;
	KeyEntry   *e = hash_search(ub->seen, &key, HASH_ENTER, &found);

	if (!found)
	{
		e->ntids = e->cap = 0;
		e->tids = NULL;
	}
	for (int i = 0; i < e->ntids; i++)
	{
		TransactionId xwait;
		uint32		token;

		if (probe_row(&ub->p, &e->tids[i], values, isnull, &xwait, &token) != PROBE_FREE)
			duplicate(ub->p.heap, ub->p.index, values, isnull, true);
	}
	if (e->ntids == e->cap)
	{
		e->cap = e->cap ? e->cap * 2 : 1;
		SIFT_REGROW_IN(ub->cx, e->tids, (Size) e->cap * sizeof(ItemPointerData));
	}
	e->tids[e->ntids++] = *tid;
}

void
sift_unique_build_end(SiftUniqueBuild *ub)
{
	prober_close(&ub->p);
	hash_destroy(ub->seen);
}
