/*
 * agg/plan_rows.c - the path of Custom Scan (SiftAggRows): count(DISTINCT) over the rows of a plan that reads SiftAgg nodes
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "utils/fmgroids.h"
#include "utils/selfuncs.h"
#include "utils/lsyscache.h"

#include "agg/accum_int.h"
#include "agg/rows.h"
#include "agg/spec.h"
#include "agg/plan_int.h"

/* the Custom Scans (SiftAgg) a plan reads, in *arms; only a query the pushdown already reaches gets this node */
static void
siftagg_arms(Path *p, List **arms)
{
	check_stack_depth();
	switch (nodeTag(p))
	{
		case T_CustomPath:
			if (((CustomPath *) p)->methods == &agg_path_methods)
				*arms = lappend(*arms, p);
			else
				foreach_ptr(Path, c, ((CustomPath *) p)->custom_paths)
					siftagg_arms(c, arms);
			break;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			siftagg_arms(((JoinPath *) p)->outerjoinpath, arms);
			siftagg_arms(((JoinPath *) p)->innerjoinpath, arms);
			break;
		case T_AppendPath:
			foreach_ptr(Path, c, ((AppendPath *) p)->subpaths)
				siftagg_arms(c, arms);
			break;
		case T_GatherPath:
			siftagg_arms(((GatherPath *) p)->subpath, arms);
			break;
		case T_GatherMergePath:
			siftagg_arms(((GatherMergePath *) p)->subpath, arms);
			break;
		case T_SubqueryScanPath:
			siftagg_arms(((SubqueryScanPath *) p)->subpath, arms);
			break;
		case T_ProjectionPath:
			siftagg_arms(((ProjectionPath *) p)->subpath, arms);
			break;
		case T_MaterialPath:
			siftagg_arms(((MaterialPath *) p)->subpath, arms);
			break;
		case T_SortPath:
			siftagg_arms(((SortPath *) p)->subpath, arms);
			break;
		default:
			break;
	}
}

/*
 * The arms of a UNION ALL that read the same index run as the sets of one pass over its units (agg/fuse.c): the dearest pays its scan and
 * each other the groups it adds, measured at 3 and 7 percent of the first (the arms are planned apart, so each is priced for a pass of its own).
 */
static double
fused_credit(List *arms)
{
	double		total = 0,
				first = 0;

	foreach_ptr(Path, p, arms)
	{
		total += p->total_cost;
		first = Max(first, p->total_cost);
	}
	return (total - first) * (1.0 - AGG_FUSED_ARM);
}

/* the plain aggregates whose transition function the node runs for each row */
static bool
pg_aggregate_ok(Oid fid)
{
	switch (fid)
	{
		case F_SUM_INT2:
		case F_SUM_INT4:
		case F_SUM_INT8:
		case F_SUM_FLOAT4:
		case F_SUM_FLOAT8:
		case F_SUM_NUMERIC:
		case F_AVG_INT2:
		case F_AVG_INT4:
		case F_AVG_INT8:
		case F_AVG_FLOAT4:
		case F_AVG_FLOAT8:
		case F_AVG_NUMERIC:
			return true;
		default:
			return false;
	}
}

/* the column of the child plan's target that holds e: the one it has, or a new one */
static int
target_column(List **cols, Node *e)
{
	int			i = 0;

	foreach_ptr(Node, c, *cols)
	{
		if (equal(c, e))
			return i;
		i++;
	}
	*cols = lappend(*cols, e);
	return i;
}

/* the node's description of aggregate a; false when the node cannot run it */
static bool
rows_agg(Aggref *a, List **cols, SiftRowsAgg *out)
{
	Node	   *arg = NULL;

	if (a->agglevelsup != 0 || a->aggkind != AGGKIND_NORMAL || a->aggorder != NIL || a->aggvariadic || a->aggdirectargs != NIL ||
		a->aggsplit != AGGSPLIT_SIMPLE || list_length(a->args) > 1 || contain_volatile_functions((Node *) a))
		return agg_refuse_on(ROWS_SHAPE, "an aggregate with an outer level, ORDER BY, VARIADIC, direct arguments, a partial split, several arguments or a volatile call");
	if (a->args != NIL)
		arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
	out->fn = a->aggfnoid;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	out->argcol = out->filtercol = -1;
	out->argtype = arg ? exprType(arg) : InvalidOid;
	if (a->aggfnoid == F_COUNT_)
	{
		if (a->aggdistinct != NIL)
			return agg_refuse_on(ROWS_SHAPE, "count(*) with DISTINCT");
		out->kind = SRA_COUNT_STAR;
	}
	else if (a->aggfnoid == F_COUNT_ANY)
	{
		if (a->aggdistinct != NIL && out->argtype != INT2OID && out->argtype != INT4OID && out->argtype != INT8OID)
			return agg_refuse_on(ROWS_SHAPE, "count(DISTINCT) of a value that is not a smallint, integer or bigint");
		out->kind = a->aggdistinct != NIL ? SRA_DISTINCT : SRA_COUNT;
	}
	else if (pg_aggregate_ok(a->aggfnoid) && a->aggdistinct == NIL)
	{
		/* the node skips the NULL arguments, which these transition functions ignore (strict or not) */
		out->kind = SRA_PG;
	}
	else
		return agg_refuse_on(ROWS_SHAPE, "an aggregate that is not count, or one of the sums and averages the rows plan computes");
	if (arg)
		out->argcol = target_column(cols, arg);
	if (a->aggfilter)
		out->filtercol = target_column(cols, (Node *) a->aggfilter);
	return true;
}

/* the aggregates counting distinct values of one argument share a set, and its pairs carry a bit for each */
static bool
sets_fit(const SiftRowsSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
	{
		int			n = 0;

		if (s->agg[i].kind != SRA_DISTINCT)
			continue;
		for (int k = 0; k < s->naggs; k++)
			n += s->agg[k].kind == SRA_DISTINCT && s->agg[k].argcol == s->agg[i].argcol;
		if (n > SIFT_ROWS_MAX_SHARED)
			return agg_refuse_limit(ROWS_SHAPE, rows_shared, SIFT_ROWS_MAX_SHARED, "more count(DISTINCT) aggregates of one column (%d) than one pass shares", n);
	}
	return true;
}

static Plan *
rows_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(lsecond(best->custom_private));
	cs->custom_private = linitial(best->custom_private);
	cs->methods = &sift_agg_rows_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	return &cs->scan.plan;
}

static const CustomPathMethods rows_path_methods = {"SiftAggRows", rows_plan_path, NULL};

const CustomScanMethods sift_agg_rows_plan_methods = {"SiftAggRows", sift_agg_rows_create_state};

/*
 * The grouping of the rows of input_rel's best plan, when that plan reads SiftAgg nodes and an aggregate counts distinct integers: the
 * plan is projected to the group columns and what the aggregates read, which is all the node takes from it.
 */
void
sift_agg_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	Query	   *parse = root->parse;
	GroupPathExtraData *gextra = extra;
	Path	   *ip = input_rel->cheapest_total_path;
	List	   *arms = NIL;
	OutWalk		ow = {NIL, NIL, false};
	List	   *cols = NIL,
			   *scan_tlist = NIL;
	SiftRowsSpec s = {0};
	PathTarget *pt;
	Path	   *proj;
	CustomPath *cp;
	double		groups;
	int			ndistinct = 0;

	if (!sift_agg_rows || ip == NULL || ip->param_info != NULL || parse->groupClause == NIL || parse->hasDistinctOn || !parse->hasAggs)
		return;
	siftagg_arms(ip, &arms);
	if (arms == NIL)
		return;
	/* a SiftAgg scan that took the aggregate itself is the better plan */
	foreach_ptr(Path, p, output_rel->pathlist)
		if (IsA(p, CustomPath) && ((CustomPath *) p)->methods == &agg_path_methods)
			return;
	if (parse->groupingSets != NIL)
	{
		agg_refused(GROUPING_SETS);
		return;
	}
	s.nkeys = list_length(parse->groupClause);
	s.eqop = palloc(s.nkeys * sizeof(Oid));
	s.collid = palloc(s.nkeys * sizeof(Oid));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		int			j = foreach_current_index(sgc);
		RegProcedure lhs,
					rhs;

		if (!get_op_hash_functions(sgc->eqop, &lhs, &rhs))
		{
			agg_refused(ROWS_SHAPE);
			return;
		}
		s.eqop[j] = sgc->eqop;
		s.collid[j] = exprCollation(e);
		ow.keys = lappend(ow.keys, e);
		cols = lappend(cols, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, j + 1, NULL, false));
	}
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad || ow.aggs == NIL)
	{
		agg_refused(ROWS_SHAPE);
		return;
	}
	s.naggs = list_length(ow.aggs);
	s.agg = palloc0(s.naggs * sizeof(SiftRowsAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		int			i = foreach_current_index(a);

		if (!IsA(a, Aggref) || !rows_agg((Aggref *) a, &cols, &s.agg[i]))
		{
			agg_refused(ROWS_SHAPE);
			return;
		}
		ndistinct += s.agg[i].kind == SRA_DISTINCT;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
	}
	/* PostgreSQL's hash aggregate has no sort to avoid without a DISTINCT */
	if (ndistinct == 0 || !sets_fit(&s))
	{
		agg_refused(ROWS_SHAPE);
		return;
	}
	pt = create_empty_pathtarget();
	foreach_ptr(Node, e, cols)
		add_column_to_pathtarget(pt, (Expr *) e, 0);
	set_pathtarget_cost_width(root, pt);
	proj = (Path *) create_projection_path(root, input_rel, ip, pt);
	groups = estimate_num_groups(root, ow.keys, ip->rows, NULL, NULL);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.parallel_safe = false;
	cp->path.rows = groups;
	cp->path.startup_cost = proj->total_cost - fused_credit(arms) + proj->rows * cpu_operator_cost * (s.nkeys * 2 + s.naggs * 2 + ndistinct * 3);
	cp->path.total_cost = cp->path.startup_cost + groups * cpu_tuple_cost;
	/*
	 * The grouping of PostgreSQL's own plans is priced as one sort of the rows, and a DISTINCT aggregate sorts them again, each by its
	 * argument, which the estimate leaves out, as it does the spill of a sort that outgrows work_mem: this node is never the dearer of
	 * the two when a DISTINCT is counted over rows a SiftAgg scan returned.
	 */
	foreach_ptr(Path, p, output_rel->pathlist)
		if (p->total_cost * 0.95 < cp->path.total_cost)
		{
			cp->path.total_cost = p->total_cost * 0.95;
			cp->path.startup_cost = Min(cp->path.startup_cost, cp->path.total_cost);
		}
	cp->methods = &rows_path_methods;
	cp->custom_paths = list_make1(proj);
	cp->custom_private = list_make2(sift_agg_rows_pack(&s), scan_tlist);
	agg_arm_path(&cp->path);
	add_path(output_rel, &cp->path);
}
