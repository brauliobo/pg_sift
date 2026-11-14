/*
 * agg/combo/child.c - combos: the rows of a child table
 * The key of a child row and the rows of a child a combo joins.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "portability/instr_time.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/memo.h"
#include "agg/combo/int.h"

/*
 * The key of the child's parameters (their values, serialized): what the processes of a parallel scan share the rows of the child by.
 * Returns the length; *key is in the current context.
 */
static uint32
child_key(EState *estate, ComboSt *cs, const SiftAggJoin *j, char **key)
{
	Size		len = 0;
	char	   *at;
	ListCell   *lp,
			   *la;

	forboth(lp, j->params, la, cs->args)
	{
		ParamExecData *prm = &estate->es_param_exec_vals[lfirst_int(lp)];
		int16		tlen;
		bool		tbyval;

		get_typlenbyval(exprType((Node *) ((ExprState *) lfirst(la))->expr), &tlen, &tbyval);
		len += datumEstimateSpace(prm->value, prm->isnull, tbyval, tlen);
	}
	*key = at = palloc(Max(len, 1));
	forboth(lp, j->params, la, cs->args)
	{
		ParamExecData *prm = &estate->es_param_exec_vals[lfirst_int(lp)];
		int16		tlen;
		bool		tbyval;

		get_typlenbyval(exprType((Node *) ((ExprState *) lfirst(la))->expr), &tlen, &tbyval);
		datumSerialize(prm->value, prm->isnull, tbyval, tlen, &at);
	}
	return (uint32) (at - *key);
}

int
combo_child_rows(AggExec *ex, int ci, const uint64 *v, uint32 **rows)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	QueryDesc  *wq = ex->wchild ? ex->wchild[cb->join] : NULL;
	EState     *estate = wq ? wq->estate : ex->css.ss.ps.state;
	PlanState  *child = wq ? wq->planstate : list_nth(estate->es_subplanstates, j->subplan - 1);
	ChildMemo  *memo = ex->cmemo ? ex->cmemo[cb->join] : NULL;
	ListCell   *lp, *la;
	int         start = cs->map->nrow, n = 0;
	MemoryContext caller = CurrentMemoryContext;
	instr_time  t0;
	char	   *key = NULL;
	uint32		klen = 0,
				nmt = 0;
	const char *stored;
	MinimalTuple *mts = NULL;

	INSTR_TIME_SET_CURRENT(t0);
	if (combo_probe_take(ex, ci, (uint32 *) &start, &n))
		goto probed;
	forboth(lp, j->params, la, cs->args)
	{
		int         id = lfirst_int(lp);
		ParamExecData *prm = &estate->es_param_exec_vals[id];

		prm->value = ExecEvalExprSwitchContext(lfirst(la), ex->lutcx, &prm->isnull);
		Assert(prm->execPlan == NULL);
	}
	if (cs->fruns)
	{
		int			r;

		MemoryContextSwitchTo(ex->cxt);
		r = map_first_row(ex, ci, estate);
		MemoryContextSwitchTo(caller);
		if (r >= 0)
			goto done;
	}
	if (memo)
	{
		uint32		nstored;

		klen = child_key(estate, cs, j, &key);
		stored = memo_find(memo, key, klen, &nstored);
		if (stored)
		{
			TupleTableSlot *slot = ex->cmslot[cb->join];

			MemoryContextSwitchTo(ex->cxt);
			if (slot == NULL)
				slot = ex->cmslot[cb->join] = MakeSingleTupleTableSlot(ExecGetResultType(child), &TTSOpsMinimalTuple);
			for (uint32 r = 0; r < nstored; r++)
			{
				MinimalTuple mt;

				stored = memo_row(stored, &mt);
				ExecStoreMinimalTuple(mt, slot, false);
				map_row(ex, cs->map, slot, j, cb, NULL);
			}
			MemoryContextSwitchTo(caller);
			pfree(key);
			goto done;
		}
		mts = palloc(8 * sizeof(MinimalTuple));
	}
	foreach(lp, j->params)
		child->chgParam = bms_add_member(child->chgParam, lfirst_int(lp));
	MemoryContextSwitchTo(estate->es_query_cxt);
	ExecReScan(child);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);

		if (TupIsNull(slot))
			break;
		if (memo)
		{
			if (nmt >= 8 && (nmt & (nmt - 1)) == 0)
				mts = repalloc(mts, nmt * 2 * sizeof(MinimalTuple));
			mts[nmt++] = ExecCopySlotMinimalTuple(slot);
		}
		MemoryContextSwitchTo(ex->cxt);
		map_row(ex, cs->map, slot, j, cb, NULL);
		MemoryContextSwitchTo(estate->es_query_cxt);
	}
	MemoryContextSwitchTo(caller);
	if (memo)
	{
		memo_put(memo, key, klen, nmt, mts);
		for (uint32 r = 0; r < nmt; r++)
			pfree(mts[r]);
		pfree(mts);
		pfree(key);
	}
done:
	n = cs->map->nrow - start;
probed:
	map_loaded(ex, n, t0);
	*rows = palloc(Max(n, 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		(*rows)[i] = start + i;
	return n;
}
