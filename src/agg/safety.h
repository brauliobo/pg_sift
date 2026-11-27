/*
 * agg/safety.h - the guards every planner of the aggregation shares before it reads a table's own files instead of the plan PostgreSQL would run: row-level security and
 * security barriers (the scan reads raw columns, so a policy or a barrier view would be bypassed), parameters of a generic plan (the value is not known when the plan is made),
 * cursors that scroll or hold (the node must buffer what it gives), and partial or expression indexes (the index does not hold every row, or its columns are not table columns).
 * One answer for the whole of agg/: a planner never tests securityQuals, hasRowSecurity, a Param or indpred itself.
 */
#ifndef SIFT_AGG_V6_SAFETY_H
#define SIFT_AGG_V6_SAFETY_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/primnodes.h"

/* A policy, a security barrier or FORCE ROW LEVEL SECURITY applies to the relation or the sub-select the entry reads, for the invoking role or for the owner of the view it is read through */
extern bool agg_rte_has_rls(PlannerInfo *root, const RangeTblEntry *rte);

/* The relation is read with no policy, barrier or security level on it: none of its clauses (rel->baserestrictinfo) is one the planner keeps behind a clause of a lower level, the way it keeps the clause of the user of a view behind those of the view */
extern bool agg_rel_security_ok(PlannerInfo *root, RelOptInfo *rel);

/* The same for an entry of the range table of q (a sub-select of the query), whose permission info is q's, for the role PostgreSQL applies the policies of (the owner of the view the entry is read through) alone: for a read of a scope, which scans no column of the table */
extern bool agg_query_rte_has_rls(const Query *q, const RangeTblEntry *rte);

/* Any entry of the query, of its sub-selects, CTEs and sub-links, is one of those, or the rewriter marked the query as one that has row security; a query without a parse tree has */
extern bool agg_query_has_rls(PlannerInfo *root);

/* A PARAM_EXTERN is in the tree: a generic plan keeps it, so no plan-time value, zone proof or constant may stand for it */
extern bool agg_node_has_extern_param(const Node *node);
/* A Param of any kind is in the tree: an expression the scan evaluates by itself has no value for it */
extern bool agg_node_has_param(const Node *node);

/* The expression calls a function that raises for some values of its input (a zero divisor, a negative root, a text that is no number): a plan that evaluates it for rows PostgreSQL's plan never reaches may raise where it does not */
extern bool agg_node_may_raise(Node *n);

/* An ANY sub-plan PostgreSQL hashes, uncorrelated, that may be started when the scan first needs it: not volatile, it cannot raise, locks no row, reads nothing of an outer query */
extern bool lut_subplan_ok(PlannerInfo *root, SubPlan *sp);

/* The output of an uncorrelated InitPlan of the same kind: an ARRAY sub-select, or a scalar one that gives one row whatever the data */
extern bool initplan_ok(PlannerInfo *root, Param *prm);

/* The executor flags of a node that is scanned again, backwards or from a mark (SCROLL cursors, merge joins, rescans): what it gives must be kept to be given again */
extern bool agg_eflags_rescannable(int eflags);

/* The index holds every row the query reads: not hypothetical, and a partial index only when the query implies its predicate (predOK) */
extern bool agg_index_complete(const IndexOptInfo *ii);

/* The 0-based place of the table column attno among the columns of the index, -1 when the index holds it in no column (an expression column is no table column) */
extern int	agg_index_col(const IndexOptInfo *ii, AttrNumber attno);

#endif							/* SIFT_AGG_V6_SAFETY_H */
