/*
 * agg/two/exec.c - the executor node of SiftAggV6Two (agg/two/two.h): when the first tuple is asked, every cell of every arm folds into the outer groups (agg/two/fold.h), then one
 * tuple leaves for each group. The arms are nodes of the plan the node runs itself: the first one asked leads the one pass of the index the arms share (agg/fuse/fuse.h), and
 * EXPLAIN shows the node alone, with the arms, the keys of the cells and what the outer aggregates read of a cell.
 */
#include "postgres.h"

#include "commands/explain.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/engine.h"
#include "agg/two/fold.h"
#include "core/memtrack.h"
#include "agg/two/hash.h"
#include "agg/two/int.h"
#include "agg/two/two.h"
#include "core/compat.h"

typedef struct TwoState
{
	CustomScanState css;
	SiftV6Spec *spec;
	List	   *inputs;
	List	   *labels;
	List	   *sort;
	List	   *spine;			/* the equalities of the join to the spine (agg/two/int.h), NIL for none */
	ExprState  *post;			/* the WHERE above a LEFT JOIN to the spine, tested on each joined row (NULL: none) */
	ExprContext *post_cx;
	List	   *arms;			/* the PlanState of each arm, which are not the node's custom_ps: EXPLAIN shows no node of them; the spine's is */
	MemoryContext gcx;
	SiftV6Group *group;
	Tuplesortstate *sorted;		/* the groups in the order of the GROUP BY, when the plan asks for it */
	TupleTableSlot *sorted_slot;
	uint64		cells;
	bool		ran;
} TwoState;

static void
two_begin(CustomScanState *node, EState *estate, int eflags)
{
	TwoState   *st = (TwoState *) node;
	const CustomScan *cs = (const CustomScan *) node->ss.ps.plan;

	st->spec = sift_v6_spec_unpack(linitial(cs->custom_private), estate->es_param_list_info);
	st->inputs = copyObject(lsecond(cs->custom_private));
	st->labels = lthird(cs->custom_private);
	st->sort = lfourth(cs->custom_private);
	st->spine = copyObject((List *) list_nth(cs->custom_private, 4));
	fix_opfuncids((Node *) st->inputs);
	if (st->spine != NIL)
	{
		fix_opfuncids(lsecond(st->spine));
		fix_opfuncids(lthird(st->spine));
		fix_opfuncids(list_nth(st->spine, 4));
		st->post = ExecInitQual(lsecond(list_nth(st->spine, 4)), NULL);
		st->post_cx = CreateExprContext(estate);
	}
	if (st->sort != NIL)
		st->sorted_slot = ExecInitExtraTupleSlot(estate, node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
	foreach_ptr(Plan, arm, cs->custom_plans)
		if (st->spine != NIL && arm == llast(cs->custom_plans))
			node->custom_ps = list_make1(ExecInitNode(arm, estate, eflags));
		else
			st->arms = lappend(st->arms, ExecInitNode(arm, estate, eflags));
}

/* a pair of a cell and a spine row (NULL: no spine) the WHERE above the join holds for, into the groups */
static void
fold_pair(TwoState *st, TwoFold *fold, TupleTableSlot *cell, TupleTableSlot *sp)
{
	if (st->post != NULL)
	{
		ResetExprContext(st->post_cx);
		st->post_cx->ecxt_scantuple = cell;
		st->post_cx->ecxt_outertuple = sp;
		if (!ExecQual(st->post, st->post_cx))
			return;
	}
	sift_two_fold_cell(fold, cell, sp);
}

/* every cell of the arms, and with a spine every pair of a cell and a spine row it matches, then the spine rows of a LEFT JOIN no cell matched */
static void
fold_arms(TwoState *st, TwoFold *fold, EState *estate)
{
	TwoHash    *hash = st->spine == NIL ? NULL : sift_two_hash_new(st->spine, linitial(st->css.custom_ps), CreateExprContext(estate), st->gcx);
	TupleTableSlot *none;
	TupleTableSlot *sp;

	foreach_ptr(PlanState, arm, st->arms)
		for (TupleTableSlot *cell = ExecProcNode(arm); !TupIsNull(cell); cell = ExecProcNode(arm))
		{
			if (hash == NULL)
			{
				fold_pair(st, fold, cell, NULL);
				continue;
			}
			sift_two_hash_probe(hash, cell);
			while ((sp = sift_two_hash_next(hash)) != NULL)
				fold_pair(st, fold, cell, sp);
		}
	if (hash == NULL || !sift_two_hash_left(hash))
		return;
	none = ExecStoreAllNullTuple(MakeSingleTupleTableSlot(ExecGetResultType(linitial(st->arms)), &TTSOpsVirtual));
	while ((sp = sift_two_hash_unmatched(hash)) != NULL)
		fold_pair(st, fold, none, sp);
}

/* every group into a sort by the keys of the GROUP BY, read back in order */
static void
sort_groups(TwoState *st)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	const int	n = list_length(st->sort);
	AttrNumber *att = palloc(n * sizeof(AttrNumber));
	Oid		   *op = palloc(n * sizeof(Oid));
	Oid		   *coll = palloc(n * sizeof(Oid));
	bool	   *nulls_first = palloc(n * sizeof(bool));

	foreach_ptr(List, k, st->sort)
	{
		const int	i = foreach_current_index(k);

		att[i] = (AttrNumber) linitial_int(k);
		op[i] = (Oid) lsecond_int(k);
		coll[i] = (Oid) lthird_int(k);
		nulls_first[i] = lfourth_int(k) != 0;
	}
	st->sorted = tuplesort_begin_heap(slot->tts_tupleDescriptor, n, att, op, coll, nulls_first, work_mem, NULL, TUPLESORT_NONE);
	for (ExecClearTuple(slot); sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull); ExecClearTuple(slot))
		tuplesort_puttupleslot(st->sorted, ExecStoreVirtualTuple(slot));
	tuplesort_performsort(st->sorted);
}

static void
two_run(TwoState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	TwoFold    *fold;

	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg two groups");
	fold = sift_two_fold_new(st->spec, st->inputs, estate, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	fold_arms(st, fold, estate);
	st->group = sift_two_fold_finish(fold);
	st->cells = sift_two_fold_cells(fold);
	st->ran = true;
	if (st->sort != NIL)
		sort_groups(st);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
two_next(ScanState *ss)
{
	TwoState   *st = (TwoState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->ran)
		two_run(st, ss->ps.state);
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (st->sorted != NULL)
		return tuplesort_gettupleslot(st->sorted, true, false, st->sorted_slot, NULL) ? ExecCopySlot(slot, st->sorted_slot) : NULL;
	if (!sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull))
		return NULL;
	return ExecStoreVirtualTuple(slot);
}

static bool
two_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
two_exec(CustomScanState *node)
{
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot = ExecScan(&node->ss, two_next, two_recheck);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
two_reset(TwoState *st)
{
	if (st->sorted != NULL)
		tuplesort_end(st->sorted);
	st->sorted = NULL;
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->gcx = NULL;
	st->group = NULL;
	st->ran = false;
}

static void
two_end(CustomScanState *node)
{
	TwoState   *st = (TwoState *) node;

	foreach_ptr(PlanState, arm, list_concat_copy(st->arms, node->custom_ps))
		ExecEndNode(arm);
	two_reset(st);
}

static void
two_rescan(CustomScanState *node)
{
	TwoState   *st = (TwoState *) node;

	two_reset(st);
	foreach_ptr(PlanState, arm, list_concat_copy(st->arms, node->custom_ps))
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(arm, node->ss.ps.chgParam);
		ExecReScan(arm);
	}
}

static void
two_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	TwoState   *st = (TwoState *) node;
	const char *keys = strVal(list_nth(st->labels, TWOL_KEYS));
	const char *cells = strVal(list_nth(st->labels, TWOL_CELLS));
	const char *aggs = strVal(list_nth(st->labels, TWOL_AGGS));

	ExplainPropertyInteger("Arms", NULL, list_length(st->arms), es);
	if (*keys)
		ExplainPropertyText("Group Keys", keys, es);
	if (*cells)
		ExplainPropertyText("Cell Keys", cells, es);
	if (*aggs)
		ExplainPropertyText("Pushed Aggregates", aggs, es);
	if (es->analyze)
		ExplainPropertyUInteger("Cells", NULL, st->cells, es);
}

static const CustomExecMethods two_exec_methods = {
	.CustomName = "SiftAggV6Two",
	.BeginCustomScan = two_begin,
	.ExecCustomScan = two_exec,
	.EndCustomScan = two_end,
	.ReScanCustomScan = two_rescan,
	.ExplainCustomScan = two_explain,
};

static Node *
two_create_state(CustomScan *cs)
{
	TwoState   *st = (TwoState *) newNode(sizeof(TwoState), T_CustomScanState);

	st->css.methods = &two_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_two_plan_methods = {"SiftAggV6Two", two_create_state};
