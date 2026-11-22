/*
 * agg/plan/safety.c - the guards of agg/plan/safety.h. A relation is read through its own check of privileges and policies by PostgreSQL's scans; the aggregation reads the
 * columns of an index directly, so it is used only where no policy, barrier, parameter, cursor or partial index can make its rows differ from the rows of the normal plan.
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/rls.h"

#include "agg/plan/safety.h"

typedef struct RlsWalk
{
	const Query *query;			/* whose range table the entries are of: the one that holds their permission info */
} RlsWalk;

static bool query_has_rls(const Query *q);

/*
 * the role the policies of the entry apply to is the owner of the view it is read through, else the invoking one. A scan of the columns of an index refuses a table with row security
 * for the invoking role whoever owns the view (invoker_too); a read of the scope indexes alone is the view's own rows, and PostgreSQL applies the policies of the owner only.
 */
static bool
rte_has_rls(const Query *q, const RangeTblEntry *rte, bool invoker_too)
{
	Oid			view_owner = InvalidOid;

	if (rte == NULL)
		return true;
	if (rte->security_barrier || rte->securityQuals != NIL)
		return true;
	if (rte->rtekind != RTE_RELATION)
		return false;
	if (rte->perminfoindex > 0 && rte->perminfoindex <= list_length(q->rteperminfos))
		view_owner = getRTEPermissionInfo(q->rteperminfos, (RangeTblEntry *) rte)->checkAsUser;
	return check_enable_rls(rte->relid, view_owner, true) == RLS_ENABLED ||
		(invoker_too && OidIsValid(view_owner) && check_enable_rls(rte->relid, InvalidOid, true) == RLS_ENABLED);
}

static bool
rls_walker(Node *node, RlsWalk *w)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
		return rte_has_rls(w->query, (RangeTblEntry *) node, true);
	if (IsA(node, Query))
		return query_has_rls((Query *) node);
	return expression_tree_walker(node, rls_walker, w);
}

static bool
query_has_rls(const Query *q)
{
	RlsWalk		w = {.query = q};

	return q->hasRowSecurity || query_tree_walker((Query *) q, rls_walker, &w, QTW_EXAMINE_RTES_BEFORE);
}

bool
agg_query_rte_has_rls(const Query *q, const RangeTblEntry *rte)
{
	return rte_has_rls(q, rte, false);
}

bool
agg_rte_has_rls(PlannerInfo *root, const RangeTblEntry *rte)
{
	return root == NULL || root->parse == NULL || rte_has_rls(root->parse, rte, true);
}

bool
agg_query_has_rls(PlannerInfo *root)
{
	return root == NULL || root->parse == NULL || query_has_rls(root->parse);
}

static bool
param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return ((Param *) node)->paramkind == PARAM_EXTERN;
	return expression_tree_walker(node, param_walker, context);
}

bool
agg_node_has_extern_param(const Node *node)
{
	return param_walker((Node *) node, NULL);
}

bool
agg_eflags_rescannable(int eflags)
{
	return (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0;
}

bool
agg_index_complete(const IndexOptInfo *ii)
{
	return !ii->hypothetical && (ii->indpred == NIL || ii->predOK);
}

int
agg_index_col(const IndexOptInfo *ii, AttrNumber attno)
{
	for (int i = 0; i < ii->ncolumns; i++)
		if (ii->indexkeys[i] == attno && attno > 0)
			return i;
	return -1;
}
