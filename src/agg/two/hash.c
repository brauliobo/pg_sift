/*
 * agg/two/hash.c - the spine rows of a two-level aggregate by the hash of their keys (agg/two/hash.h). The hash of a key is the operator's hash function of its side; a NULL key
 * hashes to 0 and equals only the NULL of an IS NOT DISTINCT FROM. The spine's plan runs in the memory of its caller (the executor's), which outlives the rows kept here.
 */
/* check-memory: allow-allocs the rows of the spine, which the planner bounded to work_mem (agg/two/spine.c) */
#include "postgres.h"

#include "common/hashfn.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/two/hash.h"
#include "core/host.h"
#include "core/memtrack.h"

typedef struct HashRow
{
	MinimalTuple tuple;
	uint32		hash;
	int			next;
	bool		matched;
	Datum	   *key;
	bool	   *isnull;
} HashRow;

struct TwoHash
{
	int			nkeys;
	ExprState **cell_key;
	ExprState **spine_key;
	FmgrInfo   *eq;
	FmgrInfo   *cell_hash;
	FmgrInfo   *spine_hash;
	Oid		   *coll;
	bool	   *null_safe;
	int16	   *key_len;		/* of the spine's side of each key */
	bool	   *key_byval;
	bool		left;
	ExprState  *match;			/* the other conditions of the join (NULL: none) */
	ExprContext *econtext;
	TupleTableSlot *slot;		/* the spine's tuple */
	HashRow    *row;
	int			nrows;
	Size		bytes;			/* of the rows kept: the planner admits a spine whose rows fit work_mem (agg/two/spine.c), and the hash holds no more */
	int		   *bucket;
	uint32		mask;
	Datum	   *probe;
	bool	   *probe_null;
	uint32		probe_hash;
	int			at;				/* the next row of the probe's chain, or of the rows no cell matched */
};

static uint32
key_hash(FmgrInfo *fn, Oid coll, Datum d, bool isnull)
{
	return isnull ? 0 : DatumGetUInt32(FunctionCall1Coll(fn, coll, d));
}

static uint32
keys_hash(TwoHash *h, ExprState **keys, FmgrInfo *fns, Datum *out, bool *isnull)
{
	uint32		hash = 0;

	for (int k = 0; k < h->nkeys; k++)
	{
		out[k] = ExecEvalExpr(keys[k], h->econtext, &isnull[k]);
		hash = hash_combine(hash, key_hash(&fns[k], h->coll[k], out[k], isnull[k]));
	}
	return hash;
}

static void
init_keys(TwoHash *h, List *cell_keys, List *spine_keys, List *ops, List *colls, List *null_safe)
{
	h->nkeys = list_length(cell_keys);
	h->cell_key = palloc(h->nkeys * sizeof(ExprState *));
	h->spine_key = palloc(h->nkeys * sizeof(ExprState *));
	h->eq = palloc(h->nkeys * sizeof(FmgrInfo));
	h->cell_hash = palloc(h->nkeys * sizeof(FmgrInfo));
	h->spine_hash = palloc(h->nkeys * sizeof(FmgrInfo));
	h->coll = palloc(h->nkeys * sizeof(Oid));
	h->null_safe = palloc(h->nkeys * sizeof(bool));
	h->key_len = palloc(h->nkeys * sizeof(int16));
	h->key_byval = palloc(h->nkeys * sizeof(bool));
	h->probe = palloc(h->nkeys * sizeof(Datum));
	h->probe_null = palloc(h->nkeys * sizeof(bool));
	for (int k = 0; k < h->nkeys; k++)
	{
		const Oid	op = list_nth_oid(ops, k);
		Oid			lhs,
					rhs;

		h->cell_key[k] = ExecInitExpr(list_nth(cell_keys, k), NULL);
		h->spine_key[k] = ExecInitExpr(list_nth(spine_keys, k), NULL);
		get_typlenbyval(exprType(list_nth(spine_keys, k)), &h->key_len[k], &h->key_byval[k]);
		fmgr_info(get_opcode(op), &h->eq[k]);
		SIFT_INVARIANT(get_op_hash_functions(op, &lhs, &rhs), "an equality of the spine of a two-level aggregate has no hash function");
		fmgr_info(lhs, &h->cell_hash[k]);
		fmgr_info(rhs, &h->spine_hash[k]);
		h->coll[k] = list_nth_oid(colls, k);
		h->null_safe[k] = list_nth_int(null_safe, k) != 0;
	}
}

/* a row of the spine: its tuple and its keys, copied into the memory of the hash */
static void
add_row(TwoHash *h, TupleTableSlot *s)
{
	HashRow    *r = &h->row[h->nrows++];

	r->key = palloc(h->nkeys * sizeof(Datum));
	r->isnull = palloc(h->nkeys * sizeof(bool));
	MemoryContext cx = MemoryContextSwitchTo(h->econtext->ecxt_per_tuple_memory);

	h->econtext->ecxt_outertuple = s;
	r->hash = keys_hash(h, h->spine_key, h->spine_hash, r->key, r->isnull);
	MemoryContextSwitchTo(cx);
	for (int k = 0; k < h->nkeys; k++)
		if (!r->isnull[k])
			r->key[k] = datumCopy(r->key[k], h->key_byval[k], h->key_len[k]);
	ResetExprContext(h->econtext);
	r->tuple = ExecCopySlotMinimalTuple(s);
	r->matched = false;
}

TwoHash *
sift_two_hash_new(List *spine, PlanState *ps, ExprContext *econtext, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	TwoHash    *h = palloc0(sizeof(TwoHash));
	int			cap = 64;
	uint32		nb = 1;

	h->left = linitial_int(linitial(spine)) != 0;
	h->econtext = econtext;
	init_keys(h, lsecond(spine), lthird(spine), linitial(lfourth(spine)), lsecond(lfourth(spine)), lthird(lfourth(spine)));
	h->match = ExecInitQual(linitial(list_nth(spine, 4)), NULL);
	h->slot = MakeSingleTupleTableSlot(ExecGetResultType(ps), &TTSOpsMinimalTuple);
	h->row = palloc(cap * sizeof(HashRow));
	MemoryContextSwitchTo(old);
	for (TupleTableSlot *s = ExecProcNode(ps); !TupIsNull(s); s = ExecProcNode(ps))
	{
		MemoryContextSwitchTo(cx);
		if (h->nrows == cap)
			h->row = repalloc(h->row, (cap *= 2) * sizeof(HashRow));
		add_row(h, s);
		h->bytes += sizeof(HashRow) + h->row[h->nrows - 1].tuple->t_len + h->nkeys * (sizeof(Datum) + sizeof(bool));
		sift_budget_check(h->bytes, work_mem, "the rows of the spine of a two-level aggregate", "work_mem", "ANALYZE the spine's table so that the planner sees its rows");
		MemoryContextSwitchTo(old);
	}
	MemoryContextSwitchTo(cx);
	while (nb < (uint32) h->nrows * 2)
		nb <<= 1;
	h->mask = nb - 1;
	h->bucket = palloc(nb * sizeof(int));
	memset(h->bucket, -1, nb * sizeof(int));
	for (int i = 0; i < h->nrows; i++)
	{
		h->row[i].next = h->bucket[h->row[i].hash & h->mask];
		h->bucket[h->row[i].hash & h->mask] = i;
	}
	h->at = -1;
	MemoryContextSwitchTo(old);
	return h;
}

bool
sift_two_hash_left(const TwoHash *h)
{
	return h->left;
}

void
sift_two_hash_probe(TwoHash *h, TupleTableSlot *cell)
{
	MemoryContext old;

	ResetExprContext(h->econtext);
	old = MemoryContextSwitchTo(h->econtext->ecxt_per_tuple_memory);
	h->econtext->ecxt_scantuple = cell;
	h->probe_hash = keys_hash(h, h->cell_key, h->cell_hash, h->probe, h->probe_null);
	MemoryContextSwitchTo(old);
	h->at = h->nrows == 0 ? -1 : h->bucket[h->probe_hash & h->mask];
}

static bool
row_equals(const TwoHash *h, const HashRow *r)
{
	if (r->hash != h->probe_hash)
		return false;
	for (int k = 0; k < h->nkeys; k++)
	{
		if (h->probe_null[k] || r->isnull[k])
		{
			if (!(h->null_safe[k] && h->probe_null[k] && r->isnull[k]))
				return false;
			continue;
		}
		if (!DatumGetBool(FunctionCall2Coll(&h->eq[k], h->coll[k], h->probe[k], r->key[k])))
			return false;
	}
	return true;
}

TupleTableSlot *
sift_two_hash_next(TwoHash *h)
{
	while (h->at >= 0)
	{
		HashRow    *r = &h->row[h->at];

		h->at = r->next;
		if (!row_equals(h, r))
			continue;
		h->econtext->ecxt_outertuple = ExecStoreMinimalTuple(r->tuple, h->slot, false);
		if (!ExecQual(h->match, h->econtext))
			continue;
		r->matched = true;
		return h->slot;
	}
	return NULL;
}

TupleTableSlot *
sift_two_hash_unmatched(TwoHash *h)
{
	for (h->at++; h->at < h->nrows; h->at++)
		if (!h->row[h->at].matched)
			return ExecStoreMinimalTuple(h->row[h->at].tuple, h->slot, false);
	return NULL;
}
