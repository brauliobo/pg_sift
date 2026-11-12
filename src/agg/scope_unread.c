/*
 * agg/scope_unread.c - why a statement that holds a relation of a scope of an index of a table it scans is not read from the scope units, for sift.explain_why (arm scope), when no site of the
 * planner names a reason: the statement was planned as written for a reason that is not about the scope (no scan of the table, a column the index does not hold, a setting).
 */
#include "postgres.h"

#include "access/relation.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/plan_int.h"
#include "col/am.h"
#include "col/scope.h"

/* does the statement scan a relation of the scope declaration that the index of the table `heap` makes */
static bool
statement_holds_scope_relation(const PlannedStmt *ps, Oid heap, const SiftScopeSpecs *specs)
{
	foreach_node(RangeTblEntry, rte, ps->rtable)
		if (rte->rtekind == RTE_RELATION && rte->relid != heap && declared_scope(heap, specs, rte->relid) != NULL)
			return true;
	return false;
}

/* the reason of the first statement shape that no site of the planner names: every scope relation the statement holds is read from a scan of its table, or the planner says why not */
static const char *
unread_reason(const PlannedStmt *ps, Oid heap, Oid index)
{
	if (!sift_agg_scope)
		return "sift.agg_scope is off";
	if (!sift_enable_agg)
		return "sift.enable_agg is off";
	if (sift_col_index_stale(index))
		return psprintf("the scope of index %s was declared after its segments were written (ALTER INDEX): REINDEX INDEX %s writes the scope segments, and the index is not scanned until then",
						get_rel_name(index), get_rel_name(index));
	if (!uses_sift_agg(ps->planTree) && !any_sift_agg(ps->subplans))
		return psprintf("no SiftAgg scan of \"%s\" is planned, so its join to the scope relation runs as written (the arms index and single say why the scan is not planned)", get_rel_name(heap));
	return psprintf("the SiftAgg scan of \"%s\" does not take its join to the scope relation of index %s as a scope read", get_rel_name(heap), get_rel_name(index));
}

const char *
sift_agg_scope_unread(const PlannedStmt *ps)
{
	if (plan_reads_scope(ps->planTree))
		return NULL;
	foreach_ptr(Plan, sub, ps->subplans)
		if (plan_reads_scope(sub))
			return NULL;
	foreach_node(RangeTblEntry, rte, ps->rtable)
	{
		Relation	heap;
		const char *why = NULL;

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
			continue;
		heap = relation_open(rte->relid, AccessShareLock);
		foreach_oid(ixoid, RelationGetIndexList(heap))
		{
			Relation	index = index_open(ixoid, AccessShareLock);
			SiftScopeSpecs *specs = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;

			if (why == NULL && specs != NULL && statement_holds_scope_relation(ps, rte->relid, specs))
				why = unread_reason(ps, rte->relid, ixoid);
			index_close(index, NoLock);
		}
		relation_close(heap, AccessShareLock);
		if (why != NULL)
			return why;
	}
	return NULL;
}
