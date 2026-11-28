/*
 * The partitions of a generic plan that the values of its parameters rule out. The planner gives the scan of a partitioned table PostgreSQL's pruning steps
 * (make_partition_pruneinfo, as for an Append); when the scan begins, get_matching_partitions runs the steps that need the values of the execution, and the scan
 * opens only the partitions they keep, so a generic plan locks the index of the partitions it reads, as a custom plan does.
 *
 * PostgreSQL's steps take `key = ANY (array)` of a constant array or of a list only. `key = ANY ($1)` is planned as `key = ` a template parameter, and the scan runs
 * the steps once for each element of the array the execution gives, with the element as the template's value: the partitions of the ANY are those of its elements.
 */
#include "postgres.h"

#include "access/table.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"

#include "agg/part_prune.h"

/* the template parameter: no statement numbers a parameter 0 */
#define ELEM_PARAM	0

static bool
is_elem_param(const Node *n)
{
	return IsA(n, Param) && ((const Param *) n)->paramkind == PARAM_EXTERN && ((const Param *) n)->paramid == ELEM_PARAM;
}

static bool
uses_elem_param(Node *n, void *context)
{
	return n != NULL && (is_elem_param(n) || expression_tree_walker(n, uses_elem_param, context));
}

/* the quals with the first `key = ANY (array)` whose array the execution gives as `key = ` the template parameter; *array is that array */
static List *
with_elem_param(List *quals, Expr **array)
{
	List	   *out = NIL;

	foreach_ptr(Expr, q, quals)
	{
		const ScalarArrayOpExpr *sa = IsA(q, ScalarArrayOpExpr) ? (const ScalarArrayOpExpr *) q : NULL;
		Expr	   *arr = sa != NULL ? lsecond(sa->args) : NULL;

		if (*array == NULL && arr != NULL && sa->useOr && !IsA(arr, Const) && !IsA(arr, ArrayExpr) && !contain_var_clause((Node *) arr) &&
			!contain_volatile_functions((Node *) arr))
		{
			Param	   *p = makeNode(Param);

			p->paramkind = PARAM_EXTERN;
			p->paramid = ELEM_PARAM;
			p->paramtype = get_element_type(exprType((Node *) arr));
			p->paramtypmod = -1;
			p->paramcollid = exprCollation((Node *) arr);
			p->location = -1;
			*array = arr;
			q = make_opclause(sa->opno, BOOLOID, false, linitial(sa->args), (Expr *) p, InvalidOid, sa->inputcollid);
		}
		out = lappend(out, q);
	}
	return out;
}

void
sift_v6_part_prune_plan(PlannerInfo *root, RelOptInfo *parent, List *leaves, SiftV6Spec *spec)
{
	Expr	   *array = NULL;
	List	   *quals = with_elem_param(extract_actual_clauses(parent->baserestrictinfo, false), &array);
	List	   *subpaths = NIL;

	if (!enable_partition_pruning || quals == NIL)
		return;
	foreach_ptr(RelOptInfo, leaf, leaves)
	{
		Path	   *p = makeNode(Path);

		p->parent = leaf;
		subpaths = lappend(subpaths, p);
	}
	spec->prune = sift_make_partition_pruneinfo(root, parent, subpaths, quals);
	if (spec->prune == NULL)
		return;
	foreach_ptr(List, levels, spec->prune->prune_infos)
		foreach_node(PartitionedRelPruneInfo, pi, levels)
		{
			fix_opfuncids((Node *) pi->initial_pruning_steps);
			spec->prune_rels = lappend_oid(spec->prune_rels, planner_rt_fetch(pi->rtindex, root)->relid);
			if (uses_elem_param((Node *) pi->initial_pruning_steps, NULL))
				spec->prune_array = array;
		}
}

/* a partitioned table of the steps: its partitions, NULL when it has no step for the start of the execution or its partitions are no longer those of the plan */
typedef struct PruneLevel
{
	const PartitionedRelPruneInfo *pi;
	PartitionKey key;
	PartitionDesc desc;
	List	   *steps;
	PartitionPruneContext cx;
} PruneLevel;

static PartitionDirectory
directory_of(EState *estate)
{
	if (estate->es_partition_directory == NULL)
		estate->es_partition_directory = CreatePartitionDirectory(estate->es_query_cxt, false);
	return estate->es_partition_directory;
}

static void
open_level(PruneLevel *l, const PartitionedRelPruneInfo *pi, Oid relid, EState *estate)
{
	Relation	rel = table_open(relid, AccessShareLock);
	PartitionDesc desc = PartitionDirectoryLookup(directory_of(estate), rel);

	*l = (PruneLevel) {.pi = pi,.key = RelationGetPartitionKey(rel)};
	if (pi->initial_pruning_steps != NIL && desc->nparts == pi->nparts && memcmp(desc->oids, pi->relid_map, sizeof(Oid) * pi->nparts) == 0)
		l->desc = desc;
	table_close(rel, NoLock);
}

static Node *
elem_as_const(Node *n, Const *elem)
{
	if (n == NULL)
		return NULL;
	if (is_elem_param(n))
		return (Node *) elem;
	return expression_tree_mutator(n, elem_as_const, elem);
}

static void
init_step_exprs(PartitionPruneContext *cx, const PartitionPruneStepOp *op, PlanState *ps)
{
	ListCell   *e = list_head(op->exprs);

	for (int k = 0; k < cx->partnatts && e != NULL; k++)
	{
		if (bms_is_member(k, op->nullkeys))
			continue;
		if (!IsA(lfirst(e), Const))
			cx->exprstates[PruneCxtStateIdx(cx->partnatts, op->step.step_id, k)] = ExecInitExpr(lfirst(e), ps);
		e = lnext(op->exprs, e);
	}
}

/* the steps of a level with the element as the template's value, and their context (InitPartitionPruneContext) */
static void
bind_level(PruneLevel *l, Const *elem, PlanState *ps)
{
	const int	n = list_length(l->pi->initial_pruning_steps) * l->key->partnatts;

	l->steps = elem == NULL ? l->pi->initial_pruning_steps : (List *) elem_as_const((Node *) l->pi->initial_pruning_steps, elem);
	l->cx = (PartitionPruneContext) {.strategy = l->key->strategy,.partnatts = l->key->partnatts,.nparts = l->desc->nparts,.boundinfo = l->desc->boundinfo,
		.partcollation = l->key->partcollation,.partsupfunc = l->key->partsupfunc,.stepcmpfuncs = palloc0(sizeof(FmgrInfo) * n),.ppccontext = CurrentMemoryContext,
	.planstate = ps,.exprcontext = ps->ps_ExprContext,.exprstates = palloc0(sizeof(ExprState *) * n)};
	foreach_ptr(PartitionPruneStep, step, l->steps)
		if (IsA(step, PartitionPruneStepOp))
			init_step_exprs(&l->cx, (PartitionPruneStepOp *) step, ps);
}

/* the partitions of the scan one partitioned table keeps, and those of the partitioned tables under it (find_matching_subplans_recurse) */
static void
match_level(PruneLevel *lv, int j, Bitmapset **keep)
{
	const PartitionedRelPruneInfo *pi = lv[j].pi;
	Bitmapset  *parts = lv[j].desc != NULL ? get_matching_partitions(&lv[j].cx, lv[j].steps) : pi->present_parts;

	for (int i = -1; (i = bms_next_member(parts, i)) >= 0;)
		if (pi->subplan_map[i] >= 0)
			*keep = bms_add_member(*keep, pi->subplan_map[i]);
		else if (pi->subpart_map[i] >= 0)
			match_level(lv, pi->subpart_map[i], keep);
}

/* the values of the template: the elements of the array the execution gives (none of a NULL array), or one run of the steps as they are */
static List *
elem_values(const SiftV6Spec *spec, PlanState *ps)
{
	ExprState  *es;
	Datum		d;
	bool		isnull;
	Oid			elemtype;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *values;
	bool	   *nulls;
	int			n;
	List	   *out = NIL;

	if (spec->prune_array == NULL)
		return list_make1(NULL);
	es = ExecInitExpr(spec->prune_array, ps);
	d = ExecEvalExprSwitchContext(es, ps->ps_ExprContext, &isnull);
	if (isnull)
		return NIL;
	elemtype = ARR_ELEMTYPE(DatumGetArrayTypeP(d));
	get_typlenbyvalalign(elemtype, &len, &byval, &align);
	deconstruct_array(DatumGetArrayTypeP(d), elemtype, len, byval, align, &values, &nulls, &n);
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			out = lappend(out, makeConst(elemtype, -1, exprCollation((Node *) spec->prune_array), len, values[i], false, byval));
	return out;
}

/* the partitions the steps keep, in their order; the first one when the steps keep none, whose quals then pass no row: the scan has a source, and its groups are those of no row */
static void
keep_parts(SiftV6Spec *s, Bitmapset *keep)
{
	int			n = 0;

	s->parts_none = bms_is_empty(keep);
	if (s->parts_none)
		keep = bms_make_singleton(0);
	for (int k = -1; (k = bms_next_member(keep, k)) >= 0; n++)
	{
		s->part_index[n] = s->part_index[k];
		s->part_is_table[n] = s->part_is_table[k];
		memmove(&s->part_attno[n * s->part_ncols], &s->part_attno[k * s->part_ncols], s->part_ncols * sizeof(AttrNumber));
	}
	s->parts_removed = s->nparts - n;
	s->nparts = n;
}

void
sift_v6_part_prune_exec(SiftV6Spec *spec, PlanState *ps, int eflags)
{
	List	   *elems;
	Bitmapset  *keep;
	ListCell   *rel;

	if (spec->prune == NULL || (eflags & EXEC_FLAG_EXPLAIN_GENERIC))
		return;
	elems = elem_values(spec, ps);
	keep = bms_copy(spec->prune->other_subplans);
	rel = list_head(spec->prune_rels);
	foreach_ptr(List, levels, spec->prune->prune_infos)
	{
		const int	n = list_length(levels);
		PruneLevel *lv = palloc(n * sizeof(PruneLevel));

		for (int j = 0; j < n; j++, rel = lnext(spec->prune_rels, rel))
			open_level(&lv[j], list_nth(levels, j), lfirst_oid(rel), ps->state);
		foreach_ptr(Const, elem, elems)
		{
			for (int j = 0; j < n; j++)
				if (lv[j].desc != NULL)
					bind_level(&lv[j], elem, ps);
			match_level(lv, 0, &keep);
		}
	}
	keep_parts(spec, keep);
}
