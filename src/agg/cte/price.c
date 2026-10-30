/*
 * agg/cte/price.c - the plan of a statement as the CTE choice prices it
 * The aggregates of a plan, the CTEs it reads, and the rows and the work of the shared plan.
 * The plan itself is made by variants.c.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"

#include "agg/agg.h"
#include "agg/cte/int.h"
#include "agg/rows.h"

static void cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs);

/*
 * What the shared plan spends on the work the scans take over: the plans of the CTEs that the inline plan has no more (once each), and the
 * aggregates that read them (the node a scan replaces; a read with no aggregate above it counts as itself).
 */
typedef struct SharedWork
{
	PlannedStmt *stmt;
	List	   *names;
	Bitmapset  *seen;
	List	   *arms;
	double		cost;
	double		read_rows;		/* the rows of elements the scans of the inline plan read: what the aggregates of the shared plan have to unnest */
	double		charged;		/* what the aggregates are charged for the rows their estimate lost (not in cost) */
	int			unnesting;		/* the aggregates charged */
	double		lift;			/* how many times derived_cte_rows raised the rows of the CTE scans of the shared plan */
} SharedWork;

/* the plans a plan reads besides its left and right tree */
static List *
child_plans(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_Append:
			return ((Append *) plan)->appendplans;
		case T_MergeAppend:
			return ((MergeAppend *) plan)->mergeplans;
		case T_SubqueryScan:
			return list_make1(((SubqueryScan *) plan)->subplan);
		case T_CustomScan:
			return ((CustomScan *) plan)->custom_plans;
		default:
			return NIL;
	}
}

/*
 * The SiftAgg nodes of the plan (they hold what is below them). The arms below a SiftAggRows node run as the sets of one pass over the index
 * (agg/fuse.c), so they cost what plan_rows.c credits them; the node's own work and the join below it are the plan above the scans, as they
 * are in the shared plan.
 */
void
sift_agg_arms(Plan *plan, ScanArms *a)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
	{
		double		pct = 0;

		a->total += plan->total_cost;
		a->first = Max(a->first, plan->total_cost);
		a->vet += agg_vet_of(plan->total_cost, &pct);
		a->vet_pct = Max(a->vet_pct, pct);
		a->n++;
		return;
	}
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_rows_plan_methods)
	{
		ScanArms	below = {0};
		double		fused;

		foreach_ptr(Plan, p, ((CustomScan *) plan)->custom_plans)
			sift_agg_arms(p, &below);
		fused = below.total - (below.total - below.first) * (1.0 - AGG_FUSED_ARM);
		a->total += fused;
		a->first = Max(a->first, below.first);
		a->vet += below.total > 0 ? below.vet * fused / below.total : 0;
		a->vet_pct = Max(a->vet_pct, below.vet_pct);
		a->n += below.n;
		return;
	}
	foreach_ptr(Plan, p, child_plans(plan))
		sift_agg_arms(p, a);
	sift_agg_arms(plan->lefttree, a);
	sift_agg_arms(plan->righttree, a);
}

void
sift_agg_arms_list(List *plans, ScanArms *a)
{
	foreach_ptr(Plan, p, plans)
		sift_agg_arms(p, a);
}

bool
uses_sift_agg(Plan *plan)
{
	ScanArms	a = {0};

	sift_agg_arms(plan, &a);
	return a.n > 0;
}

bool
any_sift_agg(List *plans)
{
	ScanArms	a = {0};

	sift_agg_arms_list(plans, &a);
	return a.n > 0;
}

/* the tables the scans of a plan read: the table of the index and those of the lookups its joins make */
void
arm_tables(Plan *plan, List **oids)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
	{
		SiftAggSpec *spec = sift_aggspec_unpack(((CustomScan *) plan)->custom_private);

		*oids = list_append_unique_oid(*oids, spec->heapoid);
		for (int i = 0; i < spec->njoin; i++)
			if (OidIsValid(spec->join[i].relid))
				*oids = list_append_unique_oid(*oids, spec->join[i].relid);
		return;
	}
	foreach_ptr(Plan, p, child_plans(plan))
		arm_tables(p, oids);
	arm_tables(plan->lefttree, oids);
	arm_tables(plan->righttree, oids);
}

/* the plans (their ids among the subplans of the statement) the expressions of a node run: its InitPlans (scalar subqueries) and the SubPlans of its target list and quals; not the CTEs, which a CteScan names */
static bool
subplan_ids_walker(Node *n, List **ids)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) && ((SubPlan *) n)->subLinkType != CTE_SUBLINK)
		*ids = lappend_int(*ids, ((SubPlan *) n)->plan_id);
	return expression_tree_walker(n, subplan_ids_walker, ids);
}

static List *
subplan_ids(Plan *plan)
{
	List	   *ids = NIL;

	subplan_ids_walker((Node *) plan->initPlan, &ids);
	subplan_ids_walker((Node *) plan->targetlist, &ids);
	subplan_ids_walker((Node *) plan->qual, &ids);
	return ids;
}

static void
cte_refs_list(PlannedStmt *stmt, List *plans, Bitmapset **refs)
{
	foreach_ptr(Plan, p, plans)
		cte_refs(stmt, p, refs);
}

static void
cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CteScan) && !bms_is_member(((CteScan *) plan)->ctePlanId, *refs))
	{
		*refs = bms_add_member(*refs, ((CteScan *) plan)->ctePlanId);
		cte_refs(stmt, list_nth(stmt->subplans, ((CteScan *) plan)->ctePlanId - 1), refs);
	}
	foreach_int(id, subplan_ids(plan))
		if (!bms_is_member(id, *refs))
		{
			*refs = bms_add_member(*refs, id);
			cte_refs(stmt, list_nth(stmt->subplans, id - 1), refs);
		}
	switch (nodeTag(plan))
	{
		case T_Append:
			cte_refs_list(stmt, ((Append *) plan)->appendplans, refs);
			break;
		case T_MergeAppend:
			cte_refs_list(stmt, ((MergeAppend *) plan)->mergeplans, refs);
			break;
		case T_SubqueryScan:
			cte_refs(stmt, ((SubqueryScan *) plan)->subplan, refs);
			break;
		case T_CustomScan:
			cte_refs_list(stmt, ((CustomScan *) plan)->custom_plans, refs);
			break;
		default:
			break;
	}
	cte_refs(stmt, plan->lefttree, refs);
	cte_refs(stmt, plan->righttree, refs);
}

/*
 * the most rows PostgreSQL expects of a scan of a table that has a column index, in a plan and its subplans (the CTEs); loops: how many times the plan
 * is run (a scan on the inner side of a nested loop reads its rows for each row of the outer side)
 */
static double
scan_rows(PlannedStmt *stmt, Plan *plan, double loops)
{
	double		rows = 0;
	double		left;
	double		right;

	if (plan == NULL)
		return 0;
	if ((IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan)) &&
		has_column_source(rt_fetch(((Scan *) plan)->scanrelid, stmt->rtable)->relid))
		rows = plan->plan_rows * loops;
	foreach_ptr(Plan, p, child_plans(plan))
	{
		double		below = scan_rows(stmt, p, loops);

		rows = Max(rows, below);
	}
	left = scan_rows(stmt, plan->lefttree, loops);
	right = scan_rows(stmt, plan->righttree, IsA(plan, NestLoop) && plan->lefttree != NULL ? loops * Max(plan->lefttree->plan_rows, 1.0) : loops);
	return Max(rows, Max(left, right));
}

/* the size of the request for the verdict (verdict.c): the rows of its largest scan of an indexed table */
double
cte_size(PlannedStmt *stmt)
{
	double		rows = scan_rows(stmt, stmt->planTree, 1.0);

	foreach_ptr(Plan, sub, stmt->subplans)
	{
		double		below = scan_rows(stmt, sub, 1.0);

		rows = Max(rows, below);
	}
	return Max(rows, 1.0);
}

/*
 * PostgreSQL charges the plan the cost of every CTE it computes, also of one that no node reads: a SiftAgg node that aggregates the
 * arms of a UNION ALL in one scan of the index (or reads the table of the CTE itself) leaves the CTE unread, and it is not run.
 */
double
plan_cost_without_unread_ctes(PlannedStmt *stmt)
{
	Bitmapset  *refs = NULL;
	double		cost = stmt->planTree->total_cost;
	int			id = 0;

	cte_refs(stmt, stmt->planTree, &refs);
	foreach_ptr(Plan, sub, stmt->subplans)
		if (!bms_is_member(++id, refs) && sub != NULL)
			cost -= sub->total_cost;
	return Max(cost, 0.0);
}

/* does a plan below this node unnest an array of the row (a lateral function)? */
static bool
unnests(PlannedStmt *stmt, Plan *plan)
{
	if (plan == NULL)
		return false;
	if (IsA(plan, FunctionScan) && rt_fetch(((Scan *) plan)->scanrelid, stmt->rtable)->lateral)
		return true;
	foreach_ptr(Plan, p, child_plans(plan))
		if (unnests(stmt, p))
			return true;
	return unnests(stmt, plan->lefttree) || unnests(stmt, plan->righttree);
}

/*
 * The aggregate of the shared plan that unnests the rows of a CTE is priced for the rows PostgreSQL expects of it, which fall to a default (a
 * row, the 100 rows of a function) where the scans of the inline plan read tens of thousands: the plan that keeps the CTE has to produce them.
 * The square root of the excess is charged, as input_rows_ratio charges the arm a scan compares with (agg/cost/zone.c). An aggregate that
 * does not unnest keeps its estimate, which the statistics of its tables count.
 */
static double
arm_rows_charge(SharedWork *w, Plan *agg)
{
	double		f;

	if (w->read_rows <= 0 || agg->lefttree == NULL || !unnests(w->stmt, agg->lefttree))
		return 0;
	f = sqrt(w->read_rows * w->lift / Max(agg->lefttree->plan_rows, 1.0));
	return f > 1.0 ? agg->total_cost * (f - 1.0) : 0;
}

static void
shared_work(SharedWork *w, Plan *plan, Plan *agg)
{
	if (plan == NULL)
		return;
	if (IsA(plan, Agg))
		agg = plan;
	if (IsA(plan, CteScan))
	{
		int			id = ((CteScan *) plan)->ctePlanId;
		RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, w->stmt->rtable);
		bool		named = false;
		Plan	   *body = list_nth(w->stmt->subplans, id - 1);

		foreach_ptr(char, name, w->names)
			named |= strcmp(name, rte->ctename) == 0;
		if (named)
		{
			if (agg == NULL)
				w->cost += plan->total_cost;
			else if (!list_member_ptr(w->arms, agg))
			{
				double		extra = arm_rows_charge(w, agg);

				w->arms = lappend(w->arms, agg);
				w->cost += agg->total_cost;
				w->charged += extra;
				w->unnesting += extra > 0;
			}
		}
		if (!bms_is_member(id, w->seen))
		{
			w->seen = bms_add_member(w->seen, id);
			if (named)
				w->cost += body->total_cost;
			shared_work(w, body, NULL);
		}
	}
	/* the aggregates of a scalar subquery read the CTE too, and cost what the scans of the inline plan replace */
	foreach_int(id, subplan_ids(plan))
		if (!bms_is_member(id, w->seen))
		{
			w->seen = bms_add_member(w->seen, id);
			shared_work(w, list_nth(w->stmt->subplans, id - 1), NULL);
		}
	foreach_ptr(Plan, p, child_plans(plan))
		shared_work(w, p, agg);
	shared_work(w, plan->lefttree, agg);
	shared_work(w, plan->righttree, agg);
}

/*
 * The charge stands when the scans replace most of the aggregates that unnest the CTE. The rest stays an ordinary plan in the inline plan, as
 * underpriced there as in the shared one, and the copies of the CTE body it reads in the inline plan decide the comparison.
 */
double
shared_work_cost(PlannedStmt *stmt, List *names, double read_rows, double lift, int nscans, double *charged, int *unnesting)
{
	SharedWork	w = {stmt, names, NULL, NIL, 0, read_rows, 0, 0, lift};

	shared_work(&w, stmt->planTree, NULL);
	*charged = 2 * nscans > w.unnesting ? w.charged : 0;
	*unnesting = w.unnesting;
	return w.cost + *charged;
}

/*
 * The ordinary reads of tables that have a column index in the plan, other than through a SiftAgg node, as work: the cost of the scan times the rows
 * of the outer side of the nested loops it is the inner side of. A scan that is an input of a merge join is read for as much of it as the join needs
 * (the join is priced for that part): its work is at most what the join costs over its other input, never less than one tuple.
 */
static double
plain_reader(PlannedStmt *stmt, Plan *plan, double loops, double bound)
{
	double		work = 0;

	if (plan == NULL || (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods))
		return 0;
	if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan))
	{
		RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, stmt->rtable);

		if (rte->rtekind == RTE_RELATION && has_column_source(rte->relid))
			work += Min(plan->total_cost, bound) * loops;
	}
	foreach_ptr(Plan, p, child_plans(plan))
		work += plain_reader(stmt, p, loops, HUGE_VAL);
	if (IsA(plan, MergeJoin))
		return work + plain_reader(stmt, plan->lefttree, loops, Max(plan->total_cost - plan->righttree->total_cost, cpu_tuple_cost)) +
			plain_reader(stmt, plan->righttree, loops, Max(plan->total_cost - plan->lefttree->total_cost, cpu_tuple_cost));
	work += plain_reader(stmt, plan->lefttree, loops, HUGE_VAL);
	return work + plain_reader(stmt, plan->righttree, IsA(plan, NestLoop) ? loops * Max(plan->lefttree->plan_rows, 1.0) : loops, HUGE_VAL);
}

/* does the plan read a table that has a column index other than through a SiftAgg node, for more than the share of its cost? */
bool
plain_reader_stmt(PlannedStmt *stmt, double share)
{
	double		work = plain_reader(stmt, stmt->planTree, 1.0, HUGE_VAL);

	foreach_ptr(Plan, p, stmt->subplans)
		work += plain_reader(stmt, p, 1.0, HUGE_VAL);
	return work > share * stmt->planTree->total_cost;
}
