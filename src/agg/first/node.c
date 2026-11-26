/*
 * agg/first/node.c - the path and the node of SiftAggV6First (v6_first_node.h).
 */
#include "postgres.h"

#include <math.h>

#include "commands/explain.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/tuplesort.h"

#include "agg/agg.h"
#include "agg/plan/int.h"
#include "agg/first/first.h"
#include "agg/first/node.h"
#include "agg/settings.h"
#include "core/memtrack.h"

#define FIRST_ENTRY_OVERHEAD	48

/* ---- the path ---- */

/* the position (1-based) in the target of the column that carries a sort or group reference */
static AttrNumber
target_att(const PathTarget *t, Index ref)
{
	for (int i = 0; i < list_length(t->exprs); i++)
		if (get_pathtarget_sortgroupref((PathTarget *) t, i) == ref)
			return (AttrNumber) (i + 1);
	return 0;
}

static bool
first_col(const PathTarget *t, const SortGroupClause *sgc, AttrNumber *att, Oid *collid)
{
	*att = target_att(t, sgc->tleSortGroupRef);
	if (*att == 0)
		return false;
	*collid = exprCollation(list_nth(t->exprs, *att - 1));
	return true;
}

/* the DISTINCT ON columns and the ORDER BY after them as positions of the input's target; false when a column is not in it or has no hash function */
static bool
first_keys(const Query *q, const PathTarget *t, SiftV6FirstKeys *k)
{
	const int	nd = list_length(q->distinctClause);
	const List *rest = list_copy_tail(q->sortClause, nd);
	Oid			lhs,
				rhs;

	if (nd > SIFT_V6_FIRST_MAX_COLS || list_length(q->sortClause) > SIFT_V6_FIRST_MAX_COLS)
		return false;
	foreach_node(SortGroupClause, sgc, q->distinctClause)
	{
		if (!first_col(t, sgc, &k->key_att[k->nkey], &k->key_coll[k->nkey]) || !get_op_hash_functions(sgc->eqop, &lhs, &rhs))
			return false;
		k->key_eqop[k->nkey++] = sgc->eqop;
	}
	foreach_node(SortGroupClause, sgc, rest)
	{
		if (!first_col(t, sgc, &k->ord_att[k->nord], &k->ord_coll[k->nord]))
			return false;
		k->ord_sortop[k->nord] = sgc->sortop;
		k->ord_nulls_first[k->nord++] = sgc->nulls_first;
	}
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		if (k->nsort == SIFT_V6_FIRST_MAX_COLS || !first_col(t, sgc, &k->sort_att[k->nsort], &k->sort_coll[k->nsort]))
			return false;
		k->sort_op[k->nsort] = sgc->sortop;
		k->sort_nulls_first[k->nsort++] = sgc->nulls_first;
	}
	return true;
}

static List *
keys_pack(const SiftV6FirstKeys *k)
{
	List	   *l = list_make2_int(k->nkey, k->nord);

	for (int i = 0; i < k->nkey; i++)
		l = list_concat(l, list_make3_int(k->key_att[i], (int) k->key_eqop[i], (int) k->key_coll[i]));
	for (int i = 0; i < k->nord; i++)
		l = list_concat(l, list_make4_int(k->ord_att[i], (int) k->ord_sortop[i], (int) k->ord_coll[i], k->ord_nulls_first[i]));
	l = lappend_int(l, k->nsort);
	for (int i = 0; i < k->nsort; i++)
		l = list_concat(l, list_make4_int(k->sort_att[i], (int) k->sort_op[i], (int) k->sort_coll[i], k->sort_nulls_first[i]));
	return l;
}

static void
keys_unpack(const List *l, SiftV6FirstKeys *k)
{
	int			at = 2;

	k->nkey = list_nth_int(l, 0);
	k->nord = list_nth_int(l, 1);
	for (int i = 0; i < k->nkey; i++, at += 3)
	{
		k->key_att[i] = (AttrNumber) list_nth_int(l, at);
		k->key_eqop[i] = (Oid) list_nth_int(l, at + 1);
		k->key_coll[i] = (Oid) list_nth_int(l, at + 2);
	}
	for (int i = 0; i < k->nord; i++, at += 4)
	{
		k->ord_att[i] = (AttrNumber) list_nth_int(l, at);
		k->ord_sortop[i] = (Oid) list_nth_int(l, at + 1);
		k->ord_coll[i] = (Oid) list_nth_int(l, at + 2);
		k->ord_nulls_first[i] = list_nth_int(l, at + 3);
	}
	k->nsort = list_nth_int(l, at++);
	for (int i = 0; i < k->nsort; i++, at += 4)
	{
		k->sort_att[i] = (AttrNumber) list_nth_int(l, at);
		k->sort_op[i] = (Oid) list_nth_int(l, at + 1);
		k->sort_coll[i] = (Oid) list_nth_int(l, at + 2);
		k->sort_nulls_first[i] = list_nth_int(l, at + 3);
	}
}

/* a table the bound format owns is read by the statement */
static bool
reads_owned_table(PlannerInfo *root)
{
	for (int i = 1; i < root->simple_rel_array_size; i++)
	{
		const RelOptInfo *rel = root->simple_rel_array[i];
		V6Plan		p = {0};

		if (rel == NULL || (rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL) || planner_rt_fetch(i, root)->rtekind != RTE_RELATION)
			continue;
		foreach_ptr(IndexOptInfo, ix, rel->indexlist)
			if (sift_v6_index_owned(ix))
				return true;
		if (sift_v6_plan_table(&p, planner_rt_fetch(i, root)->relid))
			return true;
	}
	return false;
}

/* the node holds the groups in half of work_mem, and sorts them in the other half */
#define FIRST_HALF_KB	Max(work_mem / 2, 64)

/* what PostgreSQL's sort of that many rows of that width costs above its input: the comparisons, and the pages written and read when they do not fit its half of work_mem */
static Cost
sort_cost(double rows, double width)
{
	const double bytes = Max(rows, 1.0) * width;
	const double pages = ceil(bytes / BLCKSZ);
	Cost		c = 2.0 * cpu_operator_cost * Max(rows, 1.0) * log2(Max(rows, 2.0));

	if (bytes > (double) FIRST_HALF_KB * 1024.0)
		c += 2.0 * pages * (0.75 * seq_page_cost + 0.25 * random_page_cost);
	return c;
}

/* the rows of the input once, a hash probe for each, the groups' first rows written and read back when they outgrow the hash memory, and the sort of the groups into the order of the statement */
static void
first_cost(PlannerInfo *root, const Path *child, const SiftV6FirstKeys *k, const List *key_exprs, Cost *total, double *groups)
{
	const double width = child->pathtarget->width + FIRST_ENTRY_OVERHEAD;
	const double limit = (double) FIRST_HALF_KB * 1024.0;
	const double ndv = Max(estimate_num_groups(root, (List *) key_exprs, child->rows, NULL, NULL), 1.0);
	const double outside = ndv * width > limit ? 1.0 - limit / (ndv * width) : 0.0;

	*groups = Min(ndv, Max(child->rows, 1.0));
	*total = child->total_cost + child->rows * cpu_operator_cost * (k->nkey + k->nord + 1) + *groups * cpu_tuple_cost + sort_cost(*groups, width) +
		outside * (2.0 * seq_page_cost * ceil(child->rows * width / BLCKSZ) + 2.0 * cpu_tuple_cost * child->rows);
}

static Plan *first_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods first_path_methods = {"SiftAggV6First", first_plan_path, NULL};

/* the inputs the node can take: the cheapest path of the rows, and the rows of the cheapest partial path gathered from the workers */
static List *
first_children(PlannerInfo *root, RelOptInfo *input_rel)
{
	List	   *out = input_rel->cheapest_total_path != NULL ? list_make1(input_rel->cheapest_total_path) : NIL;
	Path	   *partial = input_rel->partial_pathlist != NIL ? linitial(input_rel->partial_pathlist) : NULL;

	if (partial != NULL && input_rel->consider_parallel && partial->param_info == NULL)
		out = lappend(out, create_gather_path(root, input_rel, partial, partial->pathtarget, NULL, NULL));
	return out;
}

static void first_add_path(PlannerInfo *root, RelOptInfo *output_rel, Path *child);

void
sift_agg_v6_first_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	const Query *q = root->parse;

	if (stage != UPPERREL_DISTINCT || !sift_enable_agg || !q->hasDistinctOn || !reads_owned_table(root))
		return;
	foreach_ptr(Path, child, first_children(root, input_rel))
		first_add_path(root, output_rel, child);
}

static void
first_add_path(PlannerInfo *root, RelOptInfo *output_rel, Path *child)
{
	SiftV6FirstKeys keys = {0};
	List	   *key_exprs = NIL;
	CustomPath *cp;
	Cost		total;
	double		groups;

	if (child->param_info != NULL || contain_volatile_functions((Node *) child->pathtarget->exprs) || !first_keys(root->parse, child->pathtarget, &keys))
		return;
	for (int i = 0; i < keys.nkey; i++)
		key_exprs = lappend(key_exprs, list_nth(child->pathtarget->exprs, keys.key_att[i] - 1));
	first_cost(root, child, &keys, key_exprs, &total, &groups);
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = child->pathtarget;
	cp->path.parallel_safe = child->parallel_safe;
	cp->path.disabled_nodes = child->disabled_nodes;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->path.rows = clamp_row_est(groups);
	cp->path.startup_cost = total;
	cp->path.total_cost = total;
	cp->custom_paths = list_make1((Path *) child);
	cp->custom_private = list_make1(keys_pack(&keys));
	cp->methods = &first_path_methods;
	sift_v6_force_price(&cp->path);
	add_path(output_rel, &cp->path);
}

static Plan *
first_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(((Plan *) linitial(custom_plans))->targetlist);
	cs->custom_private = best->custom_private;
	cs->methods = &sift_agg_v6_first_plan_methods;
	return &cs->scan.plan;
}

/* ---- the node ---- */

typedef struct FirstState
{
	CustomScanState css;
	PlanState  *child;
	SiftV6FirstKeys keys;
	MemoryContext cx;
	SiftV6First *first;
	Tuplesortstate *sorted;
	SiftV6FirstStats done;
	bool		ran;			/* the input is read: the groups are in first, or sorted when the statement has an order */
} FirstState;

static void
first_begin(CustomScanState *node, EState *estate, int eflags)
{
	FirstState *st = (FirstState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	keys_unpack(linitial(cs->custom_private), &st->keys);
	st->child = ExecInitNode(linitial(cs->custom_plans), estate, eflags);
	node->custom_ps = list_make1(st->child);
}

static void
first_release(FirstState *st)
{
	if (st->first != NULL)
		sift_v6_first_free(st->first);
	if (st->sorted != NULL)
		tuplesort_end(st->sorted);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->first = NULL;
	st->sorted = NULL;
	st->cx = NULL;
	st->ran = false;
}

/* the groups' first rows into a sort in the order of the statement, so that no sort of the rows of the input and none of the groups' is left above the node; the groups are gone before the sort merges */
static void
first_sort(FirstState *st)
{
	const SiftV6FirstKeys *k = &st->keys;
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	TupleTableSlot *group = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);

	st->sorted = tuplesort_begin_heap(desc, k->nsort, (AttrNumber *) k->sort_att, (Oid *) k->sort_op, (Oid *) k->sort_coll, (bool *) k->sort_nulls_first, FIRST_HALF_KB, NULL, TUPLESORT_NONE);
	while (sift_v6_first_next(st->first, group))
		tuplesort_puttupleslot(st->sorted, group);
	ExecDropSingleTupleTableSlot(group);
	st->done = *sift_v6_first_stats(st->first);
	sift_v6_first_free(st->first);
	st->first = NULL;
	tuplesort_performsort(st->sorted);
}

static void
first_run(FirstState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	MemoryContext old = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);

	st->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 first");
	MemoryContextSwitchTo(st->cx);
	st->first = sift_v6_first_new(st->cx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, &st->keys, (uint64) FIRST_HALF_KB * 1024);
	MemoryContextSwitchTo(old);
	for (;;)
	{
		TupleTableSlot *row = ExecProcNode(st->child);

		if (TupIsNull(row))
			break;
		CHECK_FOR_INTERRUPTS();
		MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		sift_v6_first_offer(st->first, row);
		MemoryContextSwitchTo(old);
		ResetExprContext(econtext);
	}
	MemoryContextSwitchTo(st->cx);
	if (st->keys.nsort > 0)
		first_sort(st);
	MemoryContextSwitchTo(old);
	st->ran = true;
}

/* the next group of a statement with no order, as the groups leave the passes */
static bool
first_group(FirstState *st, TupleTableSlot *slot)
{
	MemoryContext old = MemoryContextSwitchTo(st->cx);
	const bool	got = sift_v6_first_next(st->first, slot);

	MemoryContextSwitchTo(old);
	return got;
}

static TupleTableSlot *
first_next(ScanState *ss)
{
	FirstState *st = (FirstState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->ran)
		first_run(st);
	ExecClearTuple(slot);
	if (st->sorted == NULL)
		return first_group(st, slot) ? slot : NULL;
	return tuplesort_gettupleslot(st->sorted, true, false, slot, NULL) ? slot : NULL;
}

static bool
first_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
first_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, first_next, first_recheck);
}

static void
first_end(CustomScanState *node)
{
	first_release((FirstState *) node);
	ExecEndNode(((FirstState *) node)->child);
}

static void
first_rescan(CustomScanState *node)
{
	FirstState *st = (FirstState *) node;

	first_release(st);
	st->done = (SiftV6FirstStats) {0};
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(st->child, node->ss.ps.chgParam);
	if (st->child->chgParam == NULL)
		ExecReScan(st->child);
}

static void
first_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	FirstState *st = (FirstState *) node;
	const SiftV6FirstStats *s = st->first != NULL ? sift_v6_first_stats(st->first) : &st->done;

	ExplainPropertyInteger("Distinct On Columns", NULL, st->keys.nkey, es);
	ExplainPropertyInteger("Order Columns", NULL, st->keys.nord, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Rows Read", NULL, (int64) s->rows, es);
		ExplainPropertyInteger("Groups", NULL, (int64) s->groups, es);
		ExplainPropertyInteger("Spilled Rows", NULL, (int64) s->spilled_rows, es);
		ExplainPropertyInteger("Passes", NULL, s->passes, es);
	}
}

static const CustomExecMethods first_exec_methods = {
	.CustomName = "SiftAggV6First",
	.BeginCustomScan = first_begin,
	.ExecCustomScan = first_exec,
	.EndCustomScan = first_end,
	.ReScanCustomScan = first_rescan,
	.ExplainCustomScan = first_explain,
};

static Node *
first_create_state(CustomScan *cscan)
{
	FirstState *st = (FirstState *) newNode(sizeof(FirstState), T_CustomScanState);

	st->css.methods = &first_exec_methods;
	st->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_first_plan_methods = {"SiftAggV6First", first_create_state};
