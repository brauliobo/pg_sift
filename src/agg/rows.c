/*
 * agg/rows.c - Custom Scan (SiftAggRows): the grouping of the rows of a plan, with exact count(DISTINCT)
 *
 * A query that groups the rows of a join or an append of SiftAgg nodes and counts distinct values per group (the outer aggregate of a
 * report over per-row aggregates: count(DISTINCT id) over the groups of an id) is run by PostgreSQL as a sort of every row on the group
 * columns and the value, and a GroupAggregate over it: wide tuples, and a spill of hundreds of megabytes once the rows outgrow work_mem.
 * This node reads the rows of its child plan once and keeps what the answer needs: a hash table of the groups (the first spelling of a
 * key is the one shown, as a hash aggregate's), the counts, the states of PostgreSQL's own transition functions for sum and avg, and for
 * each count(DISTINCT x) of an integer a set of (group, x) pairs with a bit for every aggregate of the same argument that took the row
 * (they differ in their FILTER). A pair is 16 bytes: a set that outgrows hash_mem moves its pairs to a tuplesort of (group, value, bits)
 * and counts the runs of equal pairs when the rows are done, which is a sort of narrow tuples where PostgreSQL's is of the whole row.
 *
 * The child plan is made by the planner (agg/plan_rows.c) to return only the columns this node reads, in the order of SiftRowsSpec:
 * PostgreSQL evaluates the group expressions, the arguments and the FILTERs, and the node never runs an expression.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "agg/accum_int.h"
#include "agg/dist_int.h"
#include "agg/rows.h"
#include "core/compat.h"
#include "core/explain.h"

bool		sift_agg_rows = true;

void
sift_agg_rows_init(void)
{
	DefineCustomBoolVariable("sift.agg_rows",
							 "Groups the rows of a plan that reads Custom Scan (SiftAgg) nodes with exact count(DISTINCT) in Custom Scan (SiftAggRows), without a sort of the rows.",
							 NULL, &sift_agg_rows, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/* the spec as lists that copyObject and the plan cache keep: [nkeys naggs], eqops, collations, [kind argcol filtercol] and [fn restype argtype collid] per aggregate */
List *
sift_agg_rows_pack(const SiftRowsSpec *s)
{
	List	   *dims = list_make2_int(s->nkeys, s->naggs),
			   *eq = NIL,
			   *coll = NIL,
			   *ints = NIL,
			   *oids = NIL;

	for (int j = 0; j < s->nkeys; j++)
	{
		eq = lappend_oid(eq, s->eqop[j]);
		coll = lappend_oid(coll, s->collid[j]);
	}
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftRowsAgg *a = &s->agg[i];

		ints = lappend_int(lappend_int(lappend_int(ints, a->kind), a->argcol), a->filtercol);
		oids = lappend_oid(lappend_oid(lappend_oid(lappend_oid(oids, a->fn), a->restype), a->argtype), a->collid);
	}
	return list_make5(dims, eq, coll, ints, oids);
}

SiftRowsSpec *
sift_agg_rows_unpack(List *pack)
{
	SiftRowsSpec *s = palloc0(sizeof(SiftRowsSpec));
	List	   *dims = linitial(pack),
			   *ints = lfourth(pack),
			   *oids = list_nth(pack, 4);

	s->nkeys = linitial_int(dims);
	s->naggs = lsecond_int(dims);
	s->eqop = palloc(Max(s->nkeys, 1) * sizeof(Oid));
	s->collid = palloc(Max(s->nkeys, 1) * sizeof(Oid));
	s->agg = palloc0(Max(s->naggs, 1) * sizeof(SiftRowsAgg));
	for (int j = 0; j < s->nkeys; j++)
	{
		s->eqop[j] = list_nth_oid(lsecond(pack), j);
		s->collid[j] = list_nth_oid(lthird(pack), j);
	}
	for (int i = 0; i < s->naggs; i++)
	{
		SiftRowsAgg *a = &s->agg[i];

		a->kind = list_nth_int(ints, 3 * i);
		a->argcol = list_nth_int(ints, 3 * i + 1);
		a->filtercol = list_nth_int(ints, 3 * i + 2);
		a->fn = list_nth_oid(oids, 4 * i);
		a->restype = list_nth_oid(oids, 4 * i + 1);
		a->argtype = list_nth_oid(oids, 4 * i + 2);
		a->collid = list_nth_oid(oids, 4 * i + 3);
	}
	return s;
}


/* a pair of a count(DISTINCT) set: bit b of mask is set when the b-th aggregate of the set took a row of the group with the value */
typedef struct RowsPair
{
	int64		v;
	uint32		g;
	uint32		mask;			/* 0: the slot is free */
} RowsPair;

typedef struct RowsSet
{
	int			argcol;
	Oid			argtype;
	int			nagg;
	int			agg[SIFT_ROWS_MAX_SHARED];	/* the aggregates of the set */
	RowsPair   *ent;
	uint64		cap;
	uint64		n;
	Tuplesortstate *sort;		/* the pairs once the table outgrew its memory */
	TupleTableSlot *put;
	TupleTableSlot *get;
} RowsSet;

typedef struct RowsExec
{
	CustomScanState css;
	SiftRowsSpec *spec;
	PlanState  *child;
	MemoryContext cxt;
	MemoryContext kcxt;			/* the keys of the groups */
	MemoryContext call;			/* the transition and final functions' own memory */
	bool		started;
	/* the groups */
	FmgrInfo   *hash;
	FmgrInfo   *eq;
	int16	   *klen;
	bool	   *kbyval;
	uint32		ng;
	uint32		gcap;
	Datum	   *kval;			/* [g * nkeys + j] */
	bool	   *knull;
	uint32	   *ghash;
	uint32	   *tab;			/* open addressing: group + 1, 0 is free */
	uint64		tabcap;
	/* the aggregates, per group */
	PgAgg	  **pg;
	AggState   *pgstate;
	int64	  **cnt;
	Datum	  **val;
	bool	  **has;
	RowsSet    *set;
	int			nsets;
	int		   *setof;			/* per aggregate: its set, -1 */
	size_t		budget;			/* bytes a set may take before it moves to a sort */
	uint64		nrows;
	uint64		npairs;
	int			nspilled;
	uint32		gpos;
} RowsExec;

static void rows_rehash(RowsExec *ex);
static void set_alloc(RowsExec *ex, RowsSet *st, uint64 cap);

/* no groups, empty sets, the memory of the keys and of the states released: the node's start and its rescan */
static void
rows_reset(RowsExec *ex)
{
	uint64		cap = 4096;

	for (int k = 0; k < ex->nsets; k++)
	{
		RowsSet    *st = &ex->set[k];

		if (st->sort)
		{
			tuplesort_end(st->sort);
			st->sort = NULL;
		}
		if (st->ent)
			pfree(st->ent);
		set_alloc(ex, st, cap);
	}
	MemoryContextReset(ex->kcxt);
	if (ex->pgstate)
		MemoryContextReset(ex->pgstate->curaggcontext->ecxt_per_tuple_memory);
	if (ex->tab)
		memset(ex->tab, 0, ex->tabcap * sizeof(uint32));
	for (int i = 0; i < ex->spec->naggs; i++)
	{
		if (ex->has[i])
			memset(ex->has[i], 0, ex->gcap);
		if (ex->cnt[i])
			memset(ex->cnt[i], 0, ex->gcap * sizeof(int64));
	}
	ex->ng = ex->gpos = 0;
	ex->nrows = ex->npairs = 0;
	ex->nspilled = 0;
	ex->started = false;
	if (ex->tabcap == 0)
		rows_rehash(ex);
}

static void
rows_groups_grow(RowsExec *ex)
{
	SiftRowsSpec *s = ex->spec;
	uint32		cap = Max(ex->gcap * 2, 1024);

	ex->kval = ex->kval ? repalloc(ex->kval, (Size) cap * s->nkeys * sizeof(Datum)) : MemoryContextAlloc(ex->cxt, (Size) cap * s->nkeys * sizeof(Datum));
	ex->knull = ex->knull ? repalloc(ex->knull, (Size) cap * s->nkeys) : MemoryContextAlloc(ex->cxt, (Size) cap * s->nkeys);
	ex->ghash = ex->ghash ? repalloc(ex->ghash, cap * sizeof(uint32)) : MemoryContextAlloc(ex->cxt, cap * sizeof(uint32));
	for (int i = 0; i < s->naggs; i++)
	{
		if (s->agg[i].kind == SRA_PG)
		{
			ex->val[i] = ex->val[i] ? repalloc(ex->val[i], cap * sizeof(Datum)) : MemoryContextAlloc(ex->cxt, cap * sizeof(Datum));
			ex->has[i] = ex->has[i] ? repalloc(ex->has[i], cap) : MemoryContextAlloc(ex->cxt, cap);
			memset(ex->has[i] + ex->gcap, 0, cap - ex->gcap);
		}
		else
		{
			ex->cnt[i] = ex->cnt[i] ? repalloc(ex->cnt[i], cap * sizeof(int64)) : MemoryContextAlloc(ex->cxt, cap * sizeof(int64));
			memset(ex->cnt[i] + ex->gcap, 0, (cap - ex->gcap) * sizeof(int64));
		}
	}
	ex->gcap = cap;
}

static void
rows_rehash(RowsExec *ex)
{
	uint64		cap = Max(ex->tabcap * 2, 4096);

	if (ex->tab)
		pfree(ex->tab);
	ex->tab = MemoryContextAllocZero(ex->cxt, cap * sizeof(uint32));
	ex->tabcap = cap;
	for (uint32 g = 0; g < ex->ng; g++)
	{
		uint64		i = ex->ghash[g] & (cap - 1);

		while (ex->tab[i])
			i = (i + 1) & (cap - 1);
		ex->tab[i] = g + 1;
	}
}

static bool
rows_keys_equal(RowsExec *ex, uint32 g, const Datum *values, const bool *isnull)
{
	const SiftRowsSpec *s = ex->spec;

	for (int j = 0; j < s->nkeys; j++)
	{
		Size		at = (Size) g * s->nkeys + j;

		if (isnull[j] || ex->knull[at])
		{
			if (isnull[j] != ex->knull[at])
				return false;
			continue;
		}
		if (!DatumGetBool(FunctionCall2Coll(&ex->eq[j], s->collid[j], ex->kval[at], values[j])))
			return false;
	}
	return true;
}

/* the group of the key columns of the row, made when it is the first with them */
static uint32
rows_group(RowsExec *ex, const Datum *values, const bool *isnull)
{
	const SiftRowsSpec *s = ex->spec;
	uint32		h = 0;
	uint64		i;
	uint32		g;
	MemoryContext old;

	for (int j = 0; j < s->nkeys; j++)
		h = hash_combine(h, isnull[j] ? 0x9e3779b9 : DatumGetUInt32(FunctionCall1Coll(&ex->hash[j], s->collid[j], values[j])));
	for (i = h & (ex->tabcap - 1); ex->tab[i]; i = (i + 1) & (ex->tabcap - 1))
	{
		g = ex->tab[i] - 1;
		if (ex->ghash[g] == h && rows_keys_equal(ex, g, values, isnull))
			return g;
	}
	if (ex->ng == ex->gcap)
		rows_groups_grow(ex);
	g = ex->ng++;
	old = MemoryContextSwitchTo(ex->kcxt);
	for (int j = 0; j < s->nkeys; j++)
	{
		Size		at = (Size) g * s->nkeys + j;

		ex->knull[at] = isnull[j];
		ex->kval[at] = isnull[j] ? (Datum) 0 : datumCopy(values[j], ex->kbyval[j], ex->klen[j]);
	}
	MemoryContextSwitchTo(old);
	ex->ghash[g] = h;
	ex->tab[i] = g + 1;
	if ((uint64) ex->ng * 2 > ex->tabcap)
		rows_rehash(ex);
	return g;
}

static inline uint64
pair_hash(uint32 g, int64 v)
{
	uint64		x = (uint64) v ^ ((uint64) g << 32) ^ g;

	x *= UINT64CONST(0x9E3779B97F4A7C15);
	x ^= x >> 32;
	x *= UINT64CONST(0xD6E8FEB86659FD93);
	return x ^ (x >> 32);
}

static int64
rows_int(Oid type, Datum d)
{
	return type == INT2OID ? (int64) DatumGetInt16(d) : type == INT4OID ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
}

static void
set_put_sort(RowsSet *st, uint32 g, int64 v, uint32 mask)
{
	TupleTableSlot *t = st->put;

	ExecClearTuple(t);
	t->tts_values[0] = Int32GetDatum((int32) g);
	t->tts_values[1] = Int64GetDatum(v);
	t->tts_values[2] = Int32GetDatum((int32) mask);
	t->tts_isnull[0] = t->tts_isnull[1] = t->tts_isnull[2] = false;
	ExecStoreVirtualTuple(t);
	tuplesort_puttupleslot(st->sort, t);
}

/* the pairs of the table go to a tuplesort of (group, value, bits), and so do the next ones */
static void
set_spill(RowsExec *ex, RowsSet *st)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	agg_pair_sort(3, TUPLESORT_NONE, &st->sort, &st->put, &st->get);
	MemoryContextSwitchTo(old);
	for (uint64 i = 0; i < st->cap; i++)
		if (st->ent[i].mask)
			set_put_sort(st, st->ent[i].g, st->ent[i].v, st->ent[i].mask);
	pfree(st->ent);
	st->ent = NULL;
	st->cap = st->n = 0;
	ex->nspilled++;
}

static void
set_alloc(RowsExec *ex, RowsSet *st, uint64 cap)
{
	st->ent = MemoryContextAllocZero(ex->cxt, cap * sizeof(RowsPair));
	st->cap = cap;
	st->n = 0;
}

static void
set_add(RowsExec *ex, RowsSet *st, uint32 g, int64 v, uint32 mask)
{
	uint64		i;

	if (st->sort)
	{
		set_put_sort(st, g, v, mask);
		return;
	}
	for (i = pair_hash(g, v) & (st->cap - 1); st->ent[i].mask; i = (i + 1) & (st->cap - 1))
		if (st->ent[i].g == g && st->ent[i].v == v)
		{
			st->ent[i].mask |= mask;
			return;
		}
	st->ent[i] = (RowsPair) {v, g, mask};
	if (++st->n * 2 <= st->cap)
		return;
	/* the table is half full: twice the size, or the sort when that is more than the set's share of hash_mem */
	if (st->cap * 2 * sizeof(RowsPair) > ex->budget / Max(ex->nsets, 1))
		set_spill(ex, st);
	else
	{
		RowsPair   *old = st->ent;
		uint64		ocap = st->cap;

		set_alloc(ex, st, ocap * 2);
		for (uint64 k = 0; k < ocap; k++)
			if (old[k].mask)
			{
				for (i = pair_hash(old[k].g, old[k].v) & (st->cap - 1); st->ent[i].mask; i = (i + 1) & (st->cap - 1))
					;
				st->ent[i] = old[k];
				st->n++;
			}
		pfree(old);
	}
}

/* the pair (g, bits) is final: each aggregate whose bit is set counts one more value in the group */
static void
set_count(RowsExec *ex, const RowsSet *st, uint32 g, uint32 mask)
{
	for (int b = 0; b < st->nagg; b++)
		if (mask >> b & 1)
			ex->cnt[st->agg[b]][g]++;
}

static void
set_finish(RowsExec *ex, RowsSet *st)
{
	if (st->sort == NULL)
	{
		for (uint64 i = 0; i < st->cap; i++)
			if (st->ent[i].mask)
				set_count(ex, st, st->ent[i].g, st->ent[i].mask);
		return;
	}
	{
		bool		isnull;
		bool		any = false;
		uint32		pg = 0,
					mask = 0;
		int64		pv = 0;

		tuplesort_performsort(st->sort);
		while (tuplesort_gettupleslot(st->sort, true, false, st->get, NULL))
		{
			uint32		g = (uint32) DatumGetInt32(slot_getattr(st->get, 1, &isnull));
			int64		v = DatumGetInt64(slot_getattr(st->get, 2, &isnull));
			uint32		m = (uint32) DatumGetInt32(slot_getattr(st->get, 3, &isnull));

			if (any && g == pg && v == pv)
			{
				mask |= m;
				continue;
			}
			if (any)
				set_count(ex, st, pg, mask);
			any = true;
			pg = g;
			pv = v;
			mask = m;
		}
		if (any)
			set_count(ex, st, pg, mask);
	}
}

static inline bool
rows_takes(const SiftRowsAgg *a, const Datum *values, const bool *isnull)
{
	return a->filtercol < 0 || (!isnull[a->filtercol] && DatumGetBool(values[a->filtercol]));
}

static void
rows_run(RowsExec *ex)
{
	const SiftRowsSpec *s = ex->spec;

	for (;;)
	{
		TupleTableSlot *row = ExecProcNode(ex->child);
		Datum	   *values;
		bool	   *isnull;
		uint32		g;

		if (TupIsNull(row))
			break;
		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(row);
		values = row->tts_values;
		isnull = row->tts_isnull;
		ex->nrows++;
		g = rows_group(ex, values, isnull);
		for (int i = 0; i < s->naggs; i++)
		{
			const SiftRowsAgg *a = &s->agg[i];

			if (a->kind == SRA_DISTINCT || !rows_takes(a, values, isnull))
				continue;
			switch (a->kind)
			{
				case SRA_COUNT_STAR:
					ex->cnt[i][g]++;
					break;
				case SRA_COUNT:
					ex->cnt[i][g] += !isnull[a->argcol];
					break;
				default:
					if (!isnull[a->argcol])
					{
						bool		has = ex->has[i][g];

						pgagg_step(ex->pg[i], ex->pgstate, a->collid, ex->call, &ex->val[i][g], &has, values[a->argcol]);
						ex->has[i][g] = has;
					}
			}
		}
		for (int k = 0; k < ex->nsets; k++)
		{
			RowsSet    *st = &ex->set[k];
			uint32		mask = 0;

			if (isnull[st->argcol])
				continue;
			for (int b = 0; b < st->nagg; b++)
				if (rows_takes(&s->agg[st->agg[b]], values, isnull))
					mask |= (uint32) 1 << b;
			if (mask == 0)
				continue;
			ex->npairs++;
			set_add(ex, st, g, rows_int(st->argtype, values[st->argcol]), mask);
		}
	}
	for (int k = 0; k < ex->nsets; k++)
		set_finish(ex, &ex->set[k]);
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SRA_PG)
			for (uint32 g = 0; g < ex->ng; g++)
			{
				bool		has = ex->has[i][g];

				pgagg_result(ex->pg[i], ex->pgstate, s->agg[i].collid, ex->call, ex->cxt, &ex->val[i][g], &has);
				ex->has[i][g] = has;
			}
	ex->started = true;
}

static TupleTableSlot *
rows_next(ScanState *node)
{
	RowsExec   *ex = (RowsExec *) node;
	const SiftRowsSpec *s = ex->spec;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	uint32		g;

	if (!ex->started)
		rows_run(ex);
	if (ex->gpos >= ex->ng)
		return NULL;
	g = ex->gpos++;
	ExecClearTuple(slot);
	for (int j = 0; j < s->nkeys; j++)
	{
		slot->tts_values[j] = ex->kval[(Size) g * s->nkeys + j];
		slot->tts_isnull[j] = ex->knull[(Size) g * s->nkeys + j];
	}
	for (int i = 0; i < s->naggs; i++)
	{
		int			c = s->nkeys + i;

		if (s->agg[i].kind == SRA_PG)
		{
			slot->tts_values[c] = ex->val[i][g];
			slot->tts_isnull[c] = !ex->has[i][g];
		}
		else
		{
			slot->tts_values[c] = Int64GetDatum(ex->cnt[i][g]);
			slot->tts_isnull[c] = false;
		}
	}
	return ExecStoreVirtualTuple(slot);
}

static bool
rows_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
rows_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, rows_next, rows_recheck);
}

static void
rows_begin(CustomScanState *node, EState *estate, int eflags)
{
	RowsExec   *ex = (RowsExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	SiftRowsSpec *s;

	s = ex->spec = sift_agg_rows_unpack(cs->custom_private);
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
	ex->child = linitial(node->custom_ps);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	ex->cxt = AllocSetContextCreate(estate->es_query_cxt, "sift agg rows", ALLOCSET_DEFAULT_SIZES);
	ex->kcxt = AllocSetContextCreate(ex->cxt, "sift agg rows keys", ALLOCSET_DEFAULT_SIZES);
	ex->call = AllocSetContextCreate(ex->cxt, "sift agg rows call", ALLOCSET_SMALL_SIZES);
	ex->hash = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(FmgrInfo));
	ex->eq = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(FmgrInfo));
	ex->klen = MemoryContextAlloc(ex->cxt, Max(s->nkeys, 1) * sizeof(int16));
	ex->kbyval = MemoryContextAlloc(ex->cxt, Max(s->nkeys, 1) * sizeof(bool));
	for (int j = 0; j < s->nkeys; j++)
	{
		RegProcedure lhs,
					rhs;

		SIFT_INVARIANT(get_op_hash_functions(s->eqop[j], &lhs, &rhs), "sift: no hash function for the grouping operator %u", s->eqop[j]);
		fmgr_info_cxt(lhs, &ex->hash[j], ex->cxt);
		fmgr_info_cxt(get_opcode(s->eqop[j]), &ex->eq[j], ex->cxt);
		get_typlenbyval(exprType((Node *) ((TargetEntry *) list_nth(cs->custom_scan_tlist, j))->expr), &ex->klen[j], &ex->kbyval[j]);
	}
	ex->pg = MemoryContextAllocZero(ex->cxt, Max(s->naggs, 1) * sizeof(PgAgg *));
	ex->cnt = MemoryContextAllocZero(ex->cxt, Max(s->naggs, 1) * sizeof(int64 *));
	ex->val = MemoryContextAllocZero(ex->cxt, Max(s->naggs, 1) * sizeof(Datum *));
	ex->has = MemoryContextAllocZero(ex->cxt, Max(s->naggs, 1) * sizeof(bool *));
	ex->set = MemoryContextAllocZero(ex->cxt, Max(s->naggs, 1) * sizeof(RowsSet));
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftRowsAgg *a = &s->agg[i];
		int			k;

		if (a->kind == SRA_PG)
		{
			if (ex->pgstate == NULL)
				ex->pgstate = pgagg_state();
			ex->pg[i] = MemoryContextAllocZero(ex->cxt, sizeof(PgAgg));
			pgagg_lookup(ex->pg[i], a->fn, a->restype);
		}
		if (a->kind != SRA_DISTINCT)
			continue;
		for (k = 0; k < ex->nsets && ex->set[k].argcol != a->argcol; k++)
			;
		if (k == ex->nsets)
		{
			ex->set[k].argcol = a->argcol;
			ex->set[k].argtype = a->argtype;
			ex->nsets++;
		}
		ex->set[k].agg[ex->set[k].nagg++] = i;
	}
	ex->budget = get_hash_memory_limit();
	rows_reset(ex);
}

static void
rows_end(CustomScanState *node)
{
	RowsExec   *ex = (RowsExec *) node;

	for (int k = 0; k < ex->nsets; k++)
		if (ex->set[k].sort)
			tuplesort_end(ex->set[k].sort);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	if (ex->cxt)
		MemoryContextDelete(ex->cxt);
}

static void
rows_rescan(CustomScanState *node)
{
	RowsExec   *ex = (RowsExec *) node;

	if (ex->child->chgParam == NULL || node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(ex->child, node->ss.ps.chgParam);
	if (ex->child->chgParam == NULL)
		ExecReScan(ex->child);
	rows_reset(ex);
}

static void
rows_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	RowsExec   *ex = (RowsExec *) node;
	const SiftRowsSpec *s = ex->spec;
	int			nd = 0;

	for (int i = 0; i < s->naggs; i++)
		nd += s->agg[i].kind == SRA_DISTINCT;
	ExplainPropertyInteger("Group Columns", NULL, s->nkeys, es);
	ExplainPropertyInteger("Distinct Counts", NULL, nd, es);
	if (es->analyze && ex->started)
	{
		SiftExplainLine ln;

		sift_explain_begin(&ln, es, "Rows");
		sift_explain_int(&ln, "Rows Read", NULL, "read", ex->nrows, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Groups", NULL, "groups", ex->ng, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Distinct Pairs", NULL, "distinct pairs", ex->npairs, 0);
		sift_explain_int(&ln, "Distinct Sets Spilled", NULL, "distinct sets spilled", ex->nspilled, SIFT_XI_KEEP);
		sift_explain_end(&ln);
	}
}

static const CustomExecMethods rows_exec_methods = {
	.CustomName = "SiftAggRows",
	.BeginCustomScan = rows_begin,
	.ExecCustomScan = rows_exec,
	.EndCustomScan = rows_end,
	.ReScanCustomScan = rows_rescan,
	.ExplainCustomScan = rows_explain,
};

Node *
sift_agg_rows_create_state(CustomScan *cscan)
{
	RowsExec   *ex = (RowsExec *) newNode(sizeof(RowsExec), T_CustomScanState);

	ex->css.methods = &rows_exec_methods;
	ex->css.flags = cscan->flags;
	return (Node *) ex;
}
