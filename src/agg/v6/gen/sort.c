/*
 * agg/v6/gen/sort.c - the tuplesort of the inputs of a DISTINCT, ORDER BY or WITHIN GROUP aggregate (v6_gen_sort.h)
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"

#include "core/compat.h"
#include "agg/v6/gen/sort.h"

struct V6GenSort
{
	V6GenAgg   *ga;
	Tuplesortstate *ts;
	TupleTableSlot *put;
	TupleTableSlot *get;
	int		   *vcol;			/* the column of the tuple, after the group number, that holds argument k; -1: a constant */
	int			ndcol;			/* DISTINCT: the columns that make two inputs equal, with their equality functions ... */
	int		   *dcol;
	FmgrInfo   *deq;
	int16	   *dlen;
	bool	   *dbyval;
	Datum	   *prev;			/* ... and the previous input's values */
	bool	   *prevnull;
	MemoryContext pcx;
};

static int
argument_index(const Aggref *ref, const TargetEntry *te)
{
	int			k = 0;

	while (list_nth(ref->args, k) != te)
		k++;
	return k;
}

/* the fixed-length columns first: the offset of a column after a varlena one is walked to at every comparison and read */
static int
number_columns(const V6GenAgg *ga, int *vcol)
{
	int			nv = 0;

	for (int k = 0; k < ga->nargs; k++)
		vcol[k] = -1;
	for (int pass = 0; pass < 2; pass++)
		for (int k = 0; k < ga->nargs; k++)
			if (ga->arg[k].varies && (get_typlen(ga->arg[k].typid) > 0) == (pass == 0))
				vcol[k] = nv++;
	return nv;
}

V6GenSort *
v6_gen_sort_begin(V6GenAgg *ga, MemoryContext cx, int mem_kb)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	V6GenSort  *s = palloc0(sizeof(V6GenSort));
	const Aggref *ref = ga->ref;
	List	   *sl = ga->ordered ? NIL : ref->aggdistinct != NIL ? ref->aggdistinct : ref->aggorder;	/* WITHIN GROUP: the transition function sorts */
	int			nk = 1,
				nv;
	AttrNumber *att = palloc0((list_length(sl) + 1) * sizeof(AttrNumber));
	Oid		   *ops = palloc0((list_length(sl) + 1) * sizeof(Oid)),
			   *coll = palloc0((list_length(sl) + 1) * sizeof(Oid));
	bool	   *nf = palloc0((list_length(sl) + 1) * sizeof(bool));
	TupleDesc	desc;

	s->ga = ga;
	s->vcol = palloc(Max(ga->nargs, 1) * sizeof(int));
	nv = number_columns(ga, s->vcol);
	s->dcol = palloc(Max(list_length(sl), 1) * sizeof(int));
	s->deq = palloc(Max(list_length(sl), 1) * sizeof(FmgrInfo));
	s->dlen = palloc(Max(list_length(sl), 1) * sizeof(int16));
	s->dbyval = palloc(Max(list_length(sl), 1) * sizeof(bool));
	s->prev = palloc0(Max(list_length(sl), 1) * sizeof(Datum));
	s->prevnull = palloc0(Max(list_length(sl), 1) * sizeof(bool));
	att[0] = 1;
	ops[0] = Int4LessOperator;
	desc = CreateTemplateTupleDesc(1 + nv);
	TupleDescInitEntry(desc, 1, "sid", INT4OID, -1, 0);
	for (int k = 0; k < ga->nargs; k++)
		if (s->vcol[k] >= 0)
			TupleDescInitEntry(desc, 2 + s->vcol[k], "a", ga->arg[k].typid, -1, 0);
	foreach_node(SortGroupClause, sc, sl)
	{
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);
		const int	k = argument_index(ref, te);

		if (s->vcol[k] < 0)
			continue;
		att[nk] = 2 + s->vcol[k];
		ops[nk] = sc->sortop;
		coll[nk] = exprCollation((Node *) te->expr);
		nf[nk] = sc->nulls_first;
		nk++;
		if (ga->distinct)
		{
			fmgr_info(get_opcode(sc->eqop), &s->deq[s->ndcol]);
			get_typlenbyval(ga->arg[k].typid, &s->dlen[s->ndcol], &s->dbyval[s->ndcol]);
			s->dcol[s->ndcol++] = s->vcol[k];
		}
	}
	sift_tupdesc_finalize(desc);
	/* MEM[MEM-AGG-26]: the inputs of the DISTINCT, ORDER BY and WITHIN GROUP aggregates of a scan (one tuplesort each, an equal part of work_mem, spilled to disk beyond it; the distinct inputs of the
	 * previous tuple in a context of their own) */
	s->pcx = AllocSetContextCreate(cx, "sift agg v6 distinct", ALLOCSET_SMALL_SIZES);
	s->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	s->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	s->ts = tuplesort_begin_heap(desc, nk, att, ops, coll, nf, mem_kb, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
	return s;
}

void
v6_gen_sort_put(V6GenSort *s, uint32 sid, const Datum *args, const bool *nulls)
{
	ExecClearTuple(s->put);
	s->put->tts_values[0] = Int32GetDatum((int32) sid);
	s->put->tts_isnull[0] = false;
	for (int k = 0; k < s->ga->nargs; k++)
		if (s->vcol[k] >= 0)
		{
			s->put->tts_values[1 + s->vcol[k]] = args[k];
			s->put->tts_isnull[1 + s->vcol[k]] = nulls[k];
		}
	ExecStoreVirtualTuple(s->put);
	tuplesort_puttupleslot(s->ts, s->put);
}

/* DISTINCT: is the tuple just read equal, in every column the DISTINCT list names (NULLs equal), to the one before it of the same group? The tuple becomes the one before when it is not. */
static bool
same_input(V6GenSort *s, bool samegroup)
{
	const Oid	collid = s->ga->ref->inputcollid;
	bool		same = samegroup;
	MemoryContext old;

	for (int i = 0; same && i < s->ndcol; i++)
	{
		bool		n;
		Datum		d = slot_getattr(s->get, 2 + s->dcol[i], &n);

		same = n == s->prevnull[i] && (n || DatumGetBool(FunctionCall2Coll(&s->deq[i], collid, s->prev[i], d)));
	}
	if (same)
		return true;
	MemoryContextReset(s->pcx);
	old = MemoryContextSwitchTo(s->pcx);
	for (int i = 0; i < s->ndcol; i++)
	{
		Datum		d = slot_getattr(s->get, 2 + s->dcol[i], &s->prevnull[i]);

		s->prev[i] = s->prevnull[i] ? (Datum) 0 : datumCopy(d, s->dbyval[i], s->dlen[i]);
	}
	MemoryContextSwitchTo(old);
	return false;
}

static void
finish_group(V6Gen *gen, V6GenAgg *ga, V6Acc *acc)
{
	v6_gen_final(gen, ga, acc);
	ReScanExprContext(ga->cx);
}

void
v6_gen_sort_drain(V6Gen *gen, V6GenAgg *ga, V6Acc **accs, uint32 nsid)
{
	V6GenSort  *s = ga->sort;
	uint32		cur = UINT32_MAX;
	Datum		args[FUNC_MAX_ARGS];
	bool		nulls[FUNC_MAX_ARGS];
	bool		isnull;

	for (int k = 0; k < ga->nargs; k++)
	{
		args[k] = ga->arg[k].k;
		nulls[k] = ga->arg[k].knull;
	}
	tuplesort_performsort(s->ts);
	while (tuplesort_gettupleslot(s->ts, true, false, s->get, NULL))
	{
		const uint32 sid = (uint32) DatumGetInt32(slot_getattr(s->get, 1, &isnull));

		Assert(sid < nsid && accs[sid] != NULL);
		for (int k = 0; k < ga->ntrans; k++)
			if (s->vcol[k] >= 0)
				args[k] = slot_getattr(s->get, 2 + s->vcol[k], &nulls[k]);
		if (sid != cur && cur != UINT32_MAX)
			finish_group(gen, ga, accs[cur]);
		if (ga->distinct && same_input(s, sid == cur))
			continue;
		cur = sid;
		v6_gen_advance(gen, ga, accs[sid], args, nulls);
		ResetExprContext(gen->tmp);
	}
	if (cur != UINT32_MAX)
		finish_group(gen, ga, accs[cur]);
}

void
v6_gen_sort_end(V6GenSort *s)
{
	tuplesort_end(s->ts);
	ExecDropSingleTupleTableSlot(s->put);
	ExecDropSingleTupleTableSlot(s->get);
	MemoryContextDelete(s->pcx);
}
