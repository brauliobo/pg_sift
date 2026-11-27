/*
 * agg/scope/unread.c - why a statement that holds a relation of a scope of an index of a table it scans reads no scope summary, for sift.explain_why (arm scope), when the planner of the scope
 * named no reason: the statement was planned as written for a reason that is not about the scope (a setting, no aggregate scan of the table, a shape the scope planner does not take).
 */
#include "postgres.h"

#include "access/relation.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"
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
