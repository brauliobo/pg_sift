/*
 * agg/combo/child.c - combos: the rows of a child table
 * The key of a child row, the warm-up that reads the pages a child needs, and the rows of a
 * child a combo joins.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/wide.h"
#include "agg/memo.h"
#include "agg/combo/int.h"

/*
 * The relations a correlated child reads (the indexes and heaps of its scans), and what its runs read of each. A child run for each distinct input is a probe at a random
 * place: when the runs have read a good share of a relation from the OS, and each takes long enough for the reads to be the device's, the whole relation is asked of the OS
 * in block order, which the device serves in a sequential read (ahead of the probes, which then find the pages in its cache): 25,000 probes of an index of 14,600 pages read
 * 15,000 of them, one at a time. A relation is asked once, when the runs read at least a sixteenth of its blocks, so at most 16 times the blocks the probes read.
 */
#define WARM_MAX_RELS	4
#define WARM_MIN_RUNS	256			/* a power of two: the check runs every that many runs */
#define WARM_MIN_READS	1024
#define WARM_MIN_US		25			/* an average run shorter than this finds its pages in memory */
#define WARM_SHARE		16
#define WARM_MAX_BLOCKS	(1 << 20)

typedef struct ChildWarm
{
	Relation	rel[WARM_MAX_RELS];
	int64		base[WARM_MAX_RELS];	/* the blocks read of the relation (fetched, not found in the buffers) before the first run */
	bool		asked[WARM_MAX_RELS];
	int			nrel;
	uint64		runs;
	uint64		us;
} ChildWarm;

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

/* the blocks of the relation this backend has read from the OS so far (-1: the statistics are off) */
static int64
warm_reads(Relation rel)
{
	if (rel->pgstat_info == NULL)
		return -1;
	return rel->pgstat_info->counts.blocks_fetched - rel->pgstat_info->counts.blocks_hit;
}

static void
warm_add(ChildWarm *w, Relation rel)
{
	if (rel == NULL || w->nrel == WARM_MAX_RELS)
		return;
	for (int i = 0; i < w->nrel; i++)
		if (RelationGetRelid(w->rel[i]) == RelationGetRelid(rel))
			return;
	w->base[w->nrel] = warm_reads(rel);
	w->rel[w->nrel++] = rel;
}

static bool
warm_walk(PlanState *ps, void *context)
{
	ChildWarm  *w = context;

	switch (nodeTag(ps))
	{
		case T_IndexScanState:
			warm_add(w, ((IndexScanState *) ps)->iss_RelationDesc);
			warm_add(w, ((ScanState *) ps)->ss_currentRelation);
			break;
		case T_IndexOnlyScanState:
			warm_add(w, ((IndexOnlyScanState *) ps)->ioss_RelationDesc);
			warm_add(w, ((ScanState *) ps)->ss_currentRelation);
			break;
		case T_BitmapIndexScanState:
			warm_add(w, ((BitmapIndexScanState *) ps)->biss_RelationDesc);
			break;
		case T_BitmapHeapScanState:
			warm_add(w, ((ScanState *) ps)->ss_currentRelation);
			break;
		default:
			break;
	}
	return planstate_tree_walker(ps, warm_walk, context);
}

/* the child ran since t0: when its runs are slow and have read a sixteenth of a relation, ask the OS for the rest of it (the blocks counted in ex->child_warmed) */
static void
child_warm(AggExec *ex, ComboSt *cs, PlanState *child, instr_time t0)
{
	ChildWarm  *w = cs->warm;
	instr_time	t1;
	int64		reads = 0;
	bool		test = sift_child_warm_test;	/* every run counts, whatever it read and however long it took */

	if (w == NULL)
	{
		w = cs->warm = MemoryContextAllocZero(ex->cxt, sizeof(ChildWarm));
		(void) warm_walk(child, w);
	}
	if (w->nrel == 0)
		return;
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	w->us += INSTR_TIME_GET_MICROSEC(t1);
	if (++w->runs % (test ? 4 : WARM_MIN_RUNS) != 0 || (!test && w->us < w->runs * WARM_MIN_US))
		return;
	for (int i = 0; i < w->nrel; i++)
		reads += Max(warm_reads(w->rel[i]) - w->base[i], 0);
	if (!test && reads < WARM_MIN_READS)
		return;
	for (int i = 0; i < w->nrel; i++)
	{
		BlockNumber nb;

		if (w->asked[i] || (nb = RelationGetNumberOfBlocks(w->rel[i])) > WARM_MAX_BLOCKS || (!test && (warm_reads(w->rel[i]) - w->base[i]) * WARM_SHARE < nb))
			continue;
		w->asked[i] = true;
		for (BlockNumber b = 0; b < nb; b++)
		{
			PrefetchBuffer(w->rel[i], MAIN_FORKNUM, b);
			if ((b & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
		}
		ex->child_warmed += nb;
	}
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
	child_warm(ex, cs, child, t0);
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
	map_loaded(ex, n, t0);
	*rows = palloc(Max(n, 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		(*rows)[i] = start + i;
	return n;
}
