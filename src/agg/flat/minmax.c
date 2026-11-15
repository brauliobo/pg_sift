/*
 * agg/flat/minmax.c - a correlated min() or max() as the first row in the order of its argument
 * `(SELECT max(f.t) FROM f WHERE f.k = x.k)` is `(SELECT f.t FROM f WHERE f.k = x.k AND f.t IS NOT NULL ORDER BY f.t DESC LIMIT 1)`, which is how PostgreSQL plans it
 * with a btree (planagg.c) but only for the query at the top: the sub-select that reads a column of the query around it keeps the aggregate, a pass over the rows
 * of the key. As a top-N probe it reads the group's first entry of an order array, or one unit.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/* the sort operator of min() or max(), the less or greater than of the btree order of its argument; InvalidOid for any other aggregate */
static Oid
minmax_sortop(Aggref *a)
{
	HeapTuple	tup;
	Oid			sortop;

	if (a->agglevelsup != 0 || a->aggkind != AGGKIND_NORMAL || a->aggstar || a->aggfilter != NULL || a->aggorder != NIL || a->aggdistinct != NIL ||
		list_length(a->args) != 1 || get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE)
		return InvalidOid;
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for aggregate %u", a->aggfnoid);
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	return sortop;
}

/* is the sub-select one table's min() or max() that reads a column of the query around it, and the aggregate if so */
static Aggref *
minmax_aggregate(SubLink *s)
{
	Query	   *sub;
	RangeTblEntry *t;
	Aggref	   *a;

	if (s->subLinkType != EXPR_SUBLINK)
		return NULL;
	sub = castNode(Query, s->subselect);
	if (sub->commandType != CMD_SELECT || !sub->hasAggs || sub->hasSubLinks || sub->hasWindowFuncs || sub->hasTargetSRFs || sub->hasForUpdate ||
		sub->setOperations != NULL || sub->groupClause != NIL || sub->groupingSets != NIL || sub->havingQual != NULL || sub->sortClause != NIL ||
		sub->distinctClause != NIL || sub->limitOffset != NULL || sub->limitCount != NULL || sub->cteList != NIL || sub->rowMarks != NIL ||
		list_length(sub->targetList) != 1 || sub->jointree == NULL || list_length(sub->jointree->fromlist) != 1 ||
		!IsA(linitial(sub->jointree->fromlist), RangeTblRef) || list_length(sub->rtable) != 1 || !contain_vars_of_level((Node *) sub, 1) ||
		contain_volatile_functions((Node *) sub))
		return NULL;
	t = linitial_node(RangeTblEntry, sub->rtable);
	if (t->rtekind != RTE_RELATION || t->relkind != RELKIND_RELATION || t->tablesample != NULL || !has_column_source(t->relid))
		return NULL;
	if (!IsA(linitial_node(TargetEntry, sub->targetList)->expr, Aggref))
		return NULL;
	a = (Aggref *) linitial_node(TargetEntry, sub->targetList)->expr;
	if (!OidIsValid(minmax_sortop(a)) || contain_vars_of_level((Node *) a, 1))
		return NULL;
	return a;
}

static void
minmax_rewrite(SubLink *s, Aggref *a)
{
	Query	   *sub = castNode(Query, s->subselect);
	TargetEntry *te = linitial_node(TargetEntry, sub->targetList);
	Expr	   *arg = linitial_node(TargetEntry, a->args)->expr;
	Oid			sortop = minmax_sortop(a),
				eqop;
	bool		reverse;
	SortGroupClause *sgc = makeNode(SortGroupClause);
	NullTest   *known = makeNode(NullTest);

	eqop = get_equality_op_for_ordering_op(sortop, &reverse);
	sgc->tleSortGroupRef = te->ressortgroupref = 1;
	sgc->sortop = sortop;
	sgc->eqop = eqop;
	sgc->nulls_first = reverse;
	sgc->hashable = op_hashjoinable(eqop, exprType((Node *) arg));
	known->arg = copyObject(arg);
	known->nulltesttype = IS_NOT_NULL;
	known->location = -1;
	te->expr = arg;
	sub->hasAggs = false;
	sub->sortClause = list_make1(sgc);
	sub->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
	sub->jointree->quals = make_and_qual(sub->jointree->quals, (Node *) known);
}

typedef struct Minmax
{
	bool		apply;
	bool		found;
} Minmax;

static bool
minmax_walker(Node *n, Minmax *c)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, minmax_walker, c, QTW_IGNORE_JOINALIASES);
	if (IsA(n, SubLink))
	{
		SubLink    *s = (SubLink *) n;
		Aggref	   *a;

		/* the inner levels first: the rewrite leaves the sub-select without an aggregate to find, and it is not a query level that holds sublinks */
		expression_tree_walker(n, minmax_walker, c);
		if ((a = minmax_aggregate(s)) != NULL)
		{
			c->found = true;
			if (c->apply)
				minmax_rewrite(s, a);
		}
		return false;
	}
	return expression_tree_walker(n, minmax_walker, c);
}

/* the correlated min() and max() sub-selects of the query (and of the ones it holds) as the first row in order; false where there is none */
bool
sift_agg_minmax_sublinks(Query *q, bool apply)
{
	Minmax		c = {apply, false};

	query_tree_walker(q, minmax_walker, &c, QTW_IGNORE_JOINALIASES);
	return c.found;
}
