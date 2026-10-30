/*
 * agg/order/paths.c - the Top-N paths and the min/max paths
 * Which ORDER BY ... LIMIT queries the scan answers, how a path for them is made and
 * priced, and the rows that tie with the last.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"

#include "agg/agg.h"
#include "agg/topn.h"
#include "agg/order/int.h"

/* The scan with the given number of workers (agg/topn.h: each keeps a heap, the leader merges them); the cheaper of the paths wins */
static void
topn_path(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, List *scan_tlist, double rows, int nworkers, int nordered, double wanted)
{
	SiftAggPath *ap = (SiftAggPath *) newNode(sizeof(SiftAggPath), T_CustomPath);
	CustomPath *cp = &ap->path;
	double		matches = Max(c->rel->rows, 1.0);
	Cost		extra;

	s->nworkers = nworkers;
	/* the joins are tables read by the scan itself: no child plan in a query level of its own */
	for (int i = 0; i < list_length(c->joins); i++)
	{
		ap->correlated = lappend(ap->correlated, NULL);
		ap->subroots = lappend(ap->subroots, NULL);
	}
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = c->topn_target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = nordered < list_length(root->sort_pathkeys) ? list_copy_head(root->sort_pathkeys, nordered) : root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min(rows, Max(input_rel->rows, 1.0)));
	extra = matches * c->topn_xcost;
	if (c->topn_array && nworkers == 0)
		extra += (1.0 + c->topn_cmpx) * cpu_operator_cost * rows;	/* the walk of the order array: a merge step for each entry, no heap of rows */
	else
		extra += c->topn_sorted ? topn_sort_cost(matches, list_length(root->parse->sortClause), c->topn_keybytes, c->topn_cmpx) :
			topn_heap_cost(matches * c->topn_frac, rows, c->topn_cmpx);
	cp->path.startup_cost += extra;
	cp->path.total_cost += extra;
	cp->custom_private = list_make3(agg_spec_keep(s), scan_tlist, c->level_params);
	agg_arm_path(&cp->path);
	/* TOPN_TIES: the rows come in the order of the first keys only; the Sort puts the others in order, and the Limit above it takes the first */
	if (nordered < list_length(root->sort_pathkeys))
		add_path(output_rel, enable_incremental_sort ? (Path *) create_incremental_sort_path(root, output_rel, &cp->path, root->sort_pathkeys, nordered, wanted) :
				 (Path *) create_sort_path(root, output_rel, &cp->path, root->sort_pathkeys, wanted));
	else
		add_path(output_rel, &cp->path);
}

/* The sub-selects a top-N takes are those PostgreSQL pulled up into semi joins (IN, EXISTS): their keys are a conjunct of the scan, like an inner join's */
static bool
topn_lefts_ok(List *lefts)
{
	foreach_ptr(LeftJoin, lj, lefts)
		if (lj->kind != SJ_SEMI)
			return agg_refuse_on(TOPN_SHAPE, "an outer or anti join of the scanned table");
	return true;
}

/* A query with a LIMIT, an ORDER BY and no groups: the reason it is not a top-N of the rows of one table is recorded for sift.explain_refusals */
static bool
topn_query_ok(PlannerInfo *root, double *count, double *offset)
{
	Query	   *parse = root->parse;

	if (parse->hasTargetSRFs || parse->hasRecursive || root->rowMarks != NIL)
		return agg_refuse(QUERY_KIND);
	if (!topn_bounds(parse, root, count, offset))
	{
		agg_refused_on(TOPN_SHAPE, "LIMIT and OFFSET are not constants or parameters, leave no row, or the limit has ties");
		return false;
	}
	if (parse->distinctClause || parse->setOperations)
	{
		agg_refused_on(TOPN_SHAPE, "the query has DISTINCT or a set operation");
		return false;
	}
	foreach_node(WindowClause, wc, parse->windowClause)
		if (!window_whole_set(wc))
		{
			agg_refused_on(TOPN_SHAPE, "a window has PARTITION BY, ORDER BY or a frame that is not all the rows");
			return false;
		}
	return true;
}

/* The rel the rows come from: the window stage has no relids of its own, and its paths read the rel of the scan (or join) below the WindowAgg nodes */
static RelOptInfo *
window_input(RelOptInfo *rel)
{
	Path	   *p = rel->cheapest_total_path;

	while (p != NULL && bms_is_empty(p->parent->relids))
	{
		if (IsA(p, WindowAggPath))
			p = ((WindowAggPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else
			p = NULL;
	}
	return p ? p->parent : NULL;
}

typedef struct LevelParams
{
	PlannerInfo *root;
	List	   *found;			/* the Params as PostgreSQL has them: the plan declares them (custom_exprs), so it is rescanned when they change */
} LevelParams;

/*
 * The Params of the clauses that an enclosing query level sets for each row it passes down (the plan_params of the levels above: a LATERAL
 * reference, a correlated sub-select) are marked as constants the scan evaluates when it starts: it starts over when the plan is rescanned.
 */
static Node *
mark_level_params(Node *n, LevelParams *lp)
{
	Param	   *p = (Param *) n;

	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && p->paramkind == PARAM_EXEC)
		for (PlannerInfo *r = lp->root->parent_root; r != NULL; r = r->parent_root)
			foreach_node(PlannerParamItem, pi, r->plan_params)
				if (pi->paramId == p->paramid)
				{
					p = copyObject(p);
					lp->found = list_append_unique(lp->found, copyObject(p));
					p->location = SIFT_AGG_LEVEL_LOC;
					return (Node *) p;
				}
	return expression_tree_mutator(n, mark_level_params, lp);
}

/* topn_key for a key after the first, whose failure is not the plan's: the Sort above takes it */
static bool
topn_probe(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt, Oid *cmp, Oid *collid, SiftTopXform *xf, List **exprs)
{
	bool		note = sift_explain_refusals,
				ok;

	sift_explain_refusals = false;
	ok = topn_key(c, sgc, leaf, desc, txt, cmp, collid, xf, exprs);
	sift_explain_refusals = note;
	return ok;
}

static bool
topn_ties(AggCtx *c, PlannerInfo *root, int nlead, const int *leaf, const SiftTopXform *xf, double wanted, int *slack)
{
	SortGroupClause *first = linitial_node(SortGroupClause, root->parse->sortClause);
	Node	   *x = strip_relabel((Node *) get_sortgroupclause_expr(first, root->processed_tlist));
	double		nd = DEFAULT_NUM_DISTINCT,
				nulls = 0,
				ties;
	HeapTuple	tp;

	if (list_length(root->sort_pathkeys) != list_length(root->parse->sortClause) || root->parse->hasWindowFuncs)
		return agg_refuse_on(TOPN_SHAPE, "a sort key the scan does not hold, with a window function or a repeated sort key");
	for (int j = 0; j < nlead; j++)
	{
		Oid			typ = leaf[j] >= 0 ? leaf_of(c, leaf[j])->typid : InvalidOid;

		if (xf[j].kind != TXK_NONE || (typ != INT2OID && typ != INT4OID && typ != INT8OID && typ != DATEOID && typ != TIMESTAMPOID && typ != TIMESTAMPTZOID))
			return agg_refuse_on(TOPN_SHAPE, "a sort key the scan does not hold follows keys that are not integers, dates or timestamps of a column");
	}
	if (IsA(x, Var) && (tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false))) != NULL)
	{
		Form_pg_statistic st = (Form_pg_statistic) GETSTRUCT(tp);

		if (st->stadistinct != 0)
			nd = st->stadistinct > 0 ? st->stadistinct : -st->stadistinct * Max(c->rel->tuples, 1.0);
		nulls = st->stanullfrac * Max(c->rel->rows, 1.0);
		ReleaseSysCache(tp);
	}
	ties = Max(c->rel->rows, 1.0) / Max(nd, 1.0);
	/* the NULLs that come first tie with each other, and they are the rows the n-th is among when there are more than the n */
	if (first->nulls_first && nulls >= wanted)
		ties = Max(ties, nulls);
	if (ties > TOPN_TIES_MAX)
		return agg_refuse_on(TOPN_SHAPE, "the rows that tie on the first sort key, by its statistics, are too many to keep for the keys that follow");
	*slack = Max(8, (int) ceil(2.0 * ties));
	return true;
}

/*
 * PostgreSQL plans min(k) and max(k) as the first row, in the order of k, of the rows where k is not NULL (planagg.c): a query of one table with ORDER BY k LIMIT 1 whose
 * paths are those of the scan, planned before the ordered rel exists. The top-N is offered there for such a query, so that a scan which reads the order array of k
 * (one entry of the group, not its units) competes with the aggregate that reads the units. The subroot of the planner is a level below the query (parent_root), so the query
 * itself must be the top one: a sub-select of another query keeps the aggregate (the scan that starts over for each row of the outer query is the lookup tables' to plan,
 * agg/plan_child.c).
 */
void
sift_agg_minmax_paths(PlannerInfo *root, RelOptInfo *rel)
{
	Query	   *parse = root->parse;
	Node	   *key;

	if (!sift_enable_agg || !sift_enable_topn || rel->reloptkind != RELOPT_BASEREL || root->parent_root == NULL || root->parent_root->parent_root != NULL || root->upper_targets[UPPERREL_ORDERED] != NULL || parse->limitOffset != NULL ||
		parse->limitCount == NULL || !IsA(parse->limitCount, Const) || list_length(parse->sortClause) != 1 || parse->hasAggs || parse->jointree->quals == NULL ||
		!IsA(parse->jointree->quals, List))
		return;
	key = (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist);
	foreach_ptr(Node, q, (List *) parse->jointree->quals)
		if (IsA(q, NullTest) && ((NullTest *) q)->nulltesttype == IS_NOT_NULL && equal(((NullTest *) q)->arg, key))
		{
			sift_agg_upper_paths(root, UPPERREL_ORDERED, rel, rel, NULL);
			return;
		}
}

void
topn_paths(PlannerInfo *root, RelOptInfo *window_rel, RelOptInfo *output_rel)
{
	RelOptInfo *input_rel = window_rel;
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	StringInfoData txt;
	List	   *scan_tlist = NIL,
			   *funcs = NIL;
	int			nworkers,
				nout,
				flags;
	uint64		tlo,
				thi;
	int		   *leaf,
			   *attno,
			   *oleaf;
	bool	   *desc,
			   *nulls_first,
				deep,
				skip_offset = true;
	Oid		   *sortop,
			   *cmp,
			   *collid;
	SiftTopXform *xf,
				first;
	List	   *exprs;
	double		count = 0,
				offset = 0,
				rows;
	int			nkey = list_length(parse->sortClause),
				nlead = nkey,
				slack = 0;
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED] ? root->upper_targets[UPPERREL_ORDERED] : window_rel->reltarget;

	c->topn_target = target;
	if (!sift_enable_agg || !sift_enable_topn || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || parse->limitCount == NULL ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual)
		return;
	agg_arm_candidate();
	if (!topn_query_ok(root, &count, &offset))
		return;
	if (parse->hasWindowFuncs && (input_rel = window_input(window_rel)) == NULL)
	{
		agg_refused_on(TOPN_SHAPE, "the window functions are not over the rows of one scan");
		return;
	}
	if (!agg_base(c, root, input_rel, &fs))
		return;
	if (c->func_rti != 0 || !topn_lefts_ok(fs.lefts) ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) :
		  sift_colsrc_can_tids(get_rel_relam(agg_src_table(c)), false)))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	if (root->parent_root != NULL && agg_child_planning == 0)
	{
		LevelParams lp = {root, NIL};

		fs.quals = (List *) mark_level_params((Node *) fs.quals, &lp);
		c->level_params = lp.found;
	}
	s = agg_spec_new(c);
	if (c->nparts == 0)
		c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs))
		return;
	if (s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	initStringInfo(&txt);
	leaf = palloc(nkey * sizeof(int));
	desc = palloc(nkey * sizeof(bool));
	nulls_first = palloc(nkey * sizeof(bool));
	sortop = palloc(nkey * sizeof(Oid));
	cmp = palloc(nkey * sizeof(Oid));
	collid = palloc(nkey * sizeof(Oid));
	xf = palloc0(nkey * sizeof(SiftTopXform));
	c->topn_cmpx = c->topn_keybytes = c->topn_xcost = 0;
	c->topn_xserial = false;
	c->topn_array = false;
	c->topn_runs = false;
	exprs = NIL;
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		/* a key the scan cannot hold ends the keys it orders by: the Sort above puts the rest in order (topn_ties) */
		if (j > 0 && !topn_probe(c, sgc, &leaf[j], &desc[j], &txt, &cmp[j], &collid[j], &xf[j], &exprs))
		{
			nlead = j;
			break;
		}
		if (j == 0 && !topn_key(c, sgc, &leaf[j], &desc[j], &txt, &cmp[j], &collid[j], &xf[j], &exprs))
		{
			agg_refused(TOPN_SHAPE);
			return;
		}
		nulls_first[j] = sgc->nulls_first;
		sortop[j] = sgc->sortop;
		if (OidIsValid(cmp[j]))
		{
			Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
			bool		locale = cmp[j] == F_BTTEXTCMP && !sift_collate_is_c(collid[j]);

			c->topn_cmpx = Max(c->topn_cmpx, (locale ? TOPN_CMP_LOCALE : TOPN_CMP_BYTES) + (xf[j].kind != TXK_NONE ? TOPN_CMP_XFORM * (locale ? 3 : 1) : 0));
			c->topn_keybytes += topn_key_width(c, e, exprType(e));
		}
	}
	if (nlead < nkey && !topn_ties(c, root, nlead, leaf, xf, count + offset, &slack))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (contain_subplans((Node *) target->exprs) || !topn_columns(c, target, &scan_tlist, attno, &funcs))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	nout = list_length(scan_tlist);
	oleaf = palloc(Max(nout, 1) * sizeof(int));
	topn_out_leaves(c, scan_tlist, nlead, leaf, xf, oleaf);
	if (!topn_windows(c, s, funcs, &scan_tlist))
		return;
	if (!spec_finish(c, s, root, input_rel, NIL))
		return;
	if (!topn_joins_ok(c, s))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	/* the scan starts over for each value of the parameters: its other inputs (joined plans, partitions, window functions, expressions) are read once */
	if (c->level_params != NIL &&
		(c->joins != NIL || c->nparts > 0 || parse->hasWindowFuncs || s->naggs > 0 || has_exec_param_walker((Node *) root->processed_tlist, NULL)))
	{
		agg_refused_on(TOPN_SHAPE, "a condition reads a parameter that an enclosing query sets, and the scan has joins, partitions, window functions or expressions of it");
		return;
	}
	rows = count + offset;
	/* the Limit node throws the first OFFSET rows away unread: when the output is the scan tuple, they are not read from the heap (not when a Sort is between) */
	skip_offset = nlead == nkey;
	foreach_ptr(Expr, e, target->exprs)
		skip_offset &= IsA(e, Var) || IsA(e, WindowFunc);
	c->topn_fetch = Min(skip_offset ? count : rows, Max(input_rel->rows, 1.0));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	/* the heap holds the bound, or the scan sorts every row that passes: a heap of many rows costs more than the sort, so both are priced */
	flags = (skip_offset ? TOPN_SKIP_OFFSET : 0) | (s->naggs > 0 ? TOPN_TOTALS : 0) | (topn_raw_key(c, leaf[0], xf[0].kind) ? TOPN_RAWKEY : 0) |
		(nlead < nkey ? TOPN_TIES | slack << TOPN_SLACK_SHIFT : 0);
	rows += slack;
	/* the order array gives the rows without a heap of them: only the LIMIT is held, an OFFSET past the cap is walked entry by entry (sift_topn_gorder) */
	deep = rows > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && skip_offset && nlead == 1 && count <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && topn_array_ok(c, s, leaf, xf, nlead, collid[0]);
	if (nlead <= sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) &&
		((rows <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && rows * c->topn_keybytes <= work_mem * 1024.0) || deep))
	{
		c->topn_sorted = false;
		c->topn_array = topn_array_ok(c, s, leaf, xf, nlead, collid[0]);
		/* totals read every unit: no zone leaves one out */
		c->topn_frac = s->naggs > 0 ? 1.0 :
			topn_fraction(c, topn_xform((Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist), &first, NULL),
						  leaf[0], input_rel->rows, (int64) rows);
		c->topn_runs = !c->topn_array && topn_runs_ok(c, s, leaf, xf);
		if (c->topn_array && topn_heads_win(c, s, leaf[0], rows))
		{
			c->topn_array = false;
			flags |= TOPN_NOGORD;
		}
		s->topn = sift_topn_spec(nlead, leaf, desc, nulls_first, sortop, cmp, collid, xf, exprs, nout, attno, oleaf, flags, copyObject(parse->limitCount), copyObject(parse->limitOffset));
		topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0, nlead, count + offset);
		/* the lookup tables of the joins are loaded by the process that runs the scan: no workers; the totals of the workers are not merged */
		nworkers = !deep && c->joins == NIL && c->level_params == NIL && s->naggs == 0 && !c->topn_xserial && nlead == nkey && input_rel->consider_parallel && output_rel->consider_parallel ? parallel_workers(c, s) : 0;
		if (nworkers > 0)
			topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, nworkers, nlead, count + offset);
		if (rows <= TOPN_SORT_ROWS)
			return;
	}
	/* the sort of every row has the keys the scan holds only: the Sort above would need a path of its own for the rest */
	if (nlead < nkey)
		return;
	c->topn_sorted = true;
	c->topn_array = false;
	c->topn_runs = false;
	c->topn_frac = 1.0;
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, sortop, cmp, collid, xf, exprs, nout, attno, oleaf, flags | TOPN_SORT, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0, nkey, count + offset);
}
