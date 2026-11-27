/*
 * agg/scope/unread.c - why a statement that holds a relation of a scope of an index of a table it scans reads no scope summary, for sift.explain_why (arm scope), when the planner of the scope
 * named no reason: the statement was planned as written for a reason that is not about the scope (a setting, no aggregate scan of the table, a shape the scope planner does not take).
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/index.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/scope/scope.h"
#include "agg/settings.h"
#include "core/catname.h"

/* A node of the aggregate that reads the table: SiftAggV6 and its variants; SiftAggV6Sip only filters the rows a join reads */
static bool
named(const char *name, const char *prefix)
{
	return strncmp(name, prefix, strlen(prefix)) == 0 && strcmp(name, "SiftAggV6Sip") != 0;
}

/* Does a node of the plan or of its children have a custom name with the prefix */
static bool
plan_has(const Plan *p, const char *prefix)
{
	List	   *kids = NIL;

	if (p == NULL)
		return false;
	if (IsA(p, CustomScan))
	{
		if (named(((const CustomScan *) p)->methods->CustomName, prefix))
			return true;
		kids = ((const CustomScan *) p)->custom_plans;
	}
	else if (IsA(p, Append))
		kids = ((const Append *) p)->appendplans;
	else if (IsA(p, MergeAppend))
		kids = ((const MergeAppend *) p)->mergeplans;
	else if (IsA(p, SubqueryScan))
		kids = list_make1(((const SubqueryScan *) p)->subplan);
	foreach_ptr(Plan, k, kids)
		if (plan_has(k, prefix))
			return true;
	return plan_has(p->lefttree, prefix) || plan_has(p->righttree, prefix);
}

static bool
stmt_has(const PlannedStmt *ps, const char *prefix)
{
	if (plan_has(ps->planTree, prefix))
		return true;
	foreach_ptr(Plan, sub, ps->subplans)
		if (plan_has(sub, prefix))
			return true;
	return false;
}

/* Does the statement scan a relation of a path of a scope the index declares, besides the table of the index */
static bool
holds_scope_relation(const PlannedStmt *ps, Relation index, Oid heap)
{
	const SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int ord = 0; specs != NULL && ord < specs->n; ord++)
	{
		SiftScopeLeg legs[2];
		const int	n = sift_scope_legs(index, ord, legs);

		foreach_node(RangeTblEntry, rte, ps->rtable)
			if (rte->rtekind == RTE_RELATION && rte->relid != heap)
				for (int l = 0; l < n; l++)
					if (rte->relid == legs[l].rel || rte->relid == legs[l].via_rel)
						return true;
	}
	return false;
}

/* The relations of the scopes of the indexes of the tables the statement scans, and the statement */
typedef struct Placed
{
	const PlannedStmt *ps;
	List	   *rels;
} Placed;

static void
scope_rels_of(Placed *pl, Relation index)
{
	const SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int ord = 0; specs != NULL && ord < specs->n; ord++)
	{
		SiftScopeLeg legs[2];
		const int	n = sift_scope_legs(index, ord, legs);

		for (int l = 0; l < n; l++)
		{
			pl->rels = list_append_unique_oid(pl->rels, legs[l].rel);
			if (OidIsValid(legs[l].via_rel))
				pl->rels = list_append_unique_oid(pl->rels, legs[l].via_rel);
		}
	}
}

static Index
scanrelid_of(const Plan *p)
{
	switch (nodeTag(p))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_CustomScan:
			return ((const Scan *) p)->scanrelid;
		default:
			return 0;
	}
}

static List *
children(const Plan *p)
{
	List	   *kids = list_make2(p->lefttree, p->righttree);

	if (IsA(p, CustomScan))
		kids = list_concat(kids, ((const CustomScan *) p)->custom_plans);
	else if (IsA(p, Append))
		kids = list_concat(kids, ((const Append *) p)->appendplans);
	else if (IsA(p, MergeAppend))
		kids = list_concat(kids, ((const MergeAppend *) p)->mergeplans);
	else if (IsA(p, BitmapAnd))
		kids = list_concat(kids, ((const BitmapAnd *) p)->bitmapplans);
	else if (IsA(p, BitmapOr))
		kids = list_concat(kids, ((const BitmapOr *) p)->bitmapplans);
	else if (IsA(p, SubqueryScan))
		kids = lappend(kids, ((const SubqueryScan *) p)->subplan);
	return kids;
}

/* Does the tree scan a scope relation, outside the sub-plans it runs */
static bool
scans_scope(const Placed *pl, const Plan *p)
{
	const Index rti = p != NULL ? scanrelid_of(p) : 0;

	if (p == NULL)
		return false;
	if (rti > 0 && list_member_oid(pl->rels, rt_fetch(rti, pl->ps->rtable)->relid))
		return true;
	if (IsA(p, CustomScan) && strcmp(((const CustomScan *) p)->methods->CustomName, "SiftAggV6") == 0)
	{
		const SiftV6Spec *spec = sift_v6_spec_unpack(((const CustomScan *) p)->custom_private, NULL);

		if (list_member_oid(pl->rels, spec->is_table ? spec->relid : IndexGetRelation(spec->relid, false)))
			return true;
	}
	if (IsA(p, CustomScan))
	{
		int			r = -1;

		while ((r = bms_next_member(((const CustomScan *) p)->custom_relids, r)) >= 0)
			if (list_member_oid(pl->rels, rt_fetch(r, pl->ps->rtable)->relid))
				return true;
	}
	foreach_ptr(Plan, k, children(p))
		if (scans_scope(pl, k))
			return true;
	return false;
}

/* A sub-select of the FROM list that is correlated to the relations before it, and scans a scope relation */
static bool
lateral_scope(const Placed *pl, const Plan *p)
{
	if (p == NULL)
		return false;
	if (IsA(p, SubqueryScan) && rt_fetch(((const Scan *) p)->scanrelid, pl->ps->rtable)->lateral && scans_scope(pl, ((const SubqueryScan *) p)->subplan))
		return true;
	foreach_ptr(Plan, k, children(p))
		if (lateral_scope(pl, k))
			return true;
	return false;
}

typedef struct SubLinkOf
{
	int			plan_id;
	int			type;
} SubLinkOf;

static bool
sublink_walker(Node *n, SubLinkOf *s)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) && ((SubPlan *) n)->plan_id == s->plan_id)
	{
		s->type = ((SubPlan *) n)->subLinkType;
		return true;
	}
	return expression_tree_walker(n, sublink_walker, s);
}

/* The kind of sub-select (ARRAY, EXISTS, ..) that runs sub-plan plan_id, in a node of the tree or of the sub-plans; -1: none found */
static int
sublink_type(const Placed *pl, const Plan *p, int plan_id)
{
	SubLinkOf	s = {plan_id, -1};

	if (p == NULL)
		return -1;
	if (sublink_walker((Node *) p->initPlan, &s) || sublink_walker((Node *) p->targetlist, &s) || sublink_walker((Node *) p->qual, &s) ||
		(IsA(p, NestLoop) || IsA(p, HashJoin) || IsA(p, MergeJoin) ? sublink_walker((Node *) ((const Join *) p)->joinqual, &s) : false))
		return s.type;
	foreach_ptr(Plan, k, children(p))
		if ((s.type = sublink_type(pl, k, plan_id)) >= 0)
			return s.type;
	return -1;
}

/* Where the scope relation sits when the plan reads it in a sub-plan of its own, a lateral sub-select or the scan of partitions: what the planner of the scope never saw (NULL: elsewhere) */
static const char *
placed_reason(const Placed *pl)
{
	int			id = 0;

	if (lateral_scope(pl, pl->ps->planTree))
		return "a lateral (correlated) sub-select reads the scope relation";
	if (scans_scope(pl, pl->ps->planTree))
		return NULL;
	foreach_ptr(Plan, sub, pl->ps->subplans)
	{
		int			type = -1;

		id++;
		if (!scans_scope(pl, sub))
			continue;
		type = sublink_type(pl, pl->ps->planTree, id);
		foreach_ptr(Plan, other, pl->ps->subplans)
			type = type >= 0 ? type : sublink_type(pl, other, id);
		if (type == ARRAY_SUBLINK)
			return "ARRAY(SELECT ..) over the scope relation is an InitPlan the planner runs before the scan, so this plan joins as written; the statement is also planned with IN (SELECT ..) "
				"in its place where the sub-select has no ORDER BY, LIMIT, DISTINCT or aggregate, and the cheaper plan runs (sift.explain_why of the IN form says why it is not read from the units)";
		return "a sub-select over the scope relation is a plan of its own (NOT IN, OR, a scalar sub-select, a MATERIALIZED CTE, the select list): only a join, IN or EXISTS that PostgreSQL "
			"pulls up into the join is read from the units";
	}
	return NULL;
}

const char *
sift_v6_scope_placed(const PlannedStmt *ps)
{
	const Oid	col_am = sift_catalog()->col_am;
	Placed		pl = {.ps = ps};
	bool		partition = false;

	if (!OidIsValid(col_am) || stmt_has(ps, "SiftAggV6Scope"))
		return NULL;
	foreach_node(RangeTblEntry, rte, ps->rtable)
	{
		Relation	heap;

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
			continue;
		heap = relation_open(sift_rel_must_exist(rte->relid), AccessShareLock);
		foreach_oid(ixoid, RelationGetIndexList(heap))
			if (get_rel_relam(ixoid) == col_am)
			{
				Relation	index = index_open(ixoid, AccessShareLock);
				const int	before = list_length(pl.rels);

				scope_rels_of(&pl, index);
				partition |= list_length(pl.rels) > before && heap->rd_rel->relispartition;
				index_close(index, AccessShareLock);
			}
		relation_close(heap, AccessShareLock);
	}
	if (pl.rels == NIL)
		return NULL;
	if (partition)
		return "a scan of partitions reads no scope";
	return placed_reason(&pl);
}

static const char *
unread_reason(const PlannedStmt *ps, Oid heap, Oid index)
{
	if (!sift_agg_scope)
		return "sift.agg_scope is off";
	if (!sift_enable_agg)
		return "sift.enable_agg is off";
	if (!stmt_has(ps, "SiftAgg"))
		return psprintf("no SiftAgg scan of \"%s\" is planned, so its join to the scope relation runs as written (the arms index and single say why the scan is not planned)",
						sift_rel_name(heap));
	return psprintf("the SiftAgg scan of \"%s\" does not take its join to the scope relation of index %s as a scope read", sift_rel_name(heap), sift_rel_name(index));
}

bool
sift_v6_scope_planned(const PlannedStmt *ps)
{
	return stmt_has(ps, "SiftAggV6Scope");
}

const char *
sift_v6_scope_unread(const PlannedStmt *ps)
{
	const Oid	col_am = sift_catalog()->col_am;

	if (!OidIsValid(col_am) || stmt_has(ps, "SiftAggV6Scope"))
		return NULL;
	foreach_node(RangeTblEntry, rte, ps->rtable)
	{
		Relation	heap;
		const char *why = NULL;

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
			continue;
		heap = relation_open(sift_rel_must_exist(rte->relid), AccessShareLock);
		foreach_oid(ixoid, RelationGetIndexList(heap))
		{
			Relation	index;

			if (why != NULL || get_rel_relam(ixoid) != col_am)
				continue;
			index = index_open(ixoid, AccessShareLock);
			if (holds_scope_relation(ps, index, rte->relid))
				why = unread_reason(ps, rte->relid, ixoid);
			index_close(index, AccessShareLock);
		}
		relation_close(heap, AccessShareLock);
		if (why != NULL)
			return why;
	}
	return NULL;
}
