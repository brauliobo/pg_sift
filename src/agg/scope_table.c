/*
 * agg/scope_table.c - a table of keys joined to the scope relation (agg/plan_scope.c)
 *
 * `s.key IN (SELECT key FROM my_keys WHERE ..)`, the EXISTS of it, `JOIN my_keys k ON k.key = s.key`: the keys of the scope read are the rows of the table, which the scan reads once when it
 * starts, as it reads the ARRAY(SELECT ..) of a statement that says so (agg/scope_keys.c): an InitPlan under the snapshot of the statement with the conditions of the table, so the keys are the
 * ones the join would see. The sub-select is built from the table's entry of the range table and its conditions and planned when the path becomes a plan (scope_keys_plan), so that a plan
 * that loses has no InitPlan.
 *
 * The proof that the keys are the join's, all of it at plan time:
 *  (a) the table is a plain or partitioned table read as itself: no view (the entry of a view's table says so), no row-level security, no inheritance, not the table the statement modifies;
 *      its conditions read it only and are not volatile, read no executor parameter or sub-select and cannot raise: the sub-select runs at the start of the scan, also when no row would reach it;
 *  (b) a semi join (IN, EXISTS) counts a row once whichever keys the table gives; a join counts it once for each pair, so the table must give at most one key: a unique index whose columns the
 *      conditions all equate to constants or parameters. A table that may repeat a key keeps the join, and so does one that may give several: a member whose pairs changed needs it (docs/COL-SCOPE.md).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/subselect.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"

/* does the query level, or a statement of its CTEs, modify the table */
static bool
modifies(PlannerInfo *p, Oid relid)
{
	Query	   *q = p->parse;

	if (q->resultRelation > 0 && planner_rt_fetch(q->resultRelation, p)->relid == relid)
		return true;
	foreach_node(CommonTableExpr, cte, q->cteList)
		if (IsA(cte->ctequery, Query) && ((Query *) cte->ctequery)->resultRelation > 0 &&
			rt_fetch(((Query *) cte->ctequery)->resultRelation, ((Query *) cte->ctequery)->rtable)->relid == relid)
			return true;
	return false;
}

/* why the table of keys is not one the scan may read once at its start with the answer of the join; NULL: it is */
static const char *
table_why(PlannerInfo *root, RangeTblEntry *rte)
{
	HeapTuple	tup;
	bool		rls;

	if (rte->rtekind != RTE_RELATION)
		return "the table of keys is a sub-select, a function or a CTE";
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_PARTITIONED_TABLE)
		return "the table of keys is a materialized view or a foreign table";
	if (rte->tablesample != NULL || (rte->inh && rte->relkind == RELKIND_RELATION))
		return "the table of keys is sampled, or an inheritance parent whose rows depend on its children";
	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	rls = ((Form_pg_class) GETSTRUCT(tup))->relrowsecurity;
	ReleaseSysCache(tup);
	if (rls || rte->securityQuals != NIL)
		return "row-level security is enabled on the table of keys";
	if (rte->perminfoindex > 0 && OidIsValid(getRTEPermissionInfo(root->parse->rteperminfos, rte)->checkAsUser))
		return "the table of keys is read through a view";
	for (PlannerInfo *p = root; p != NULL; p = p->parent_root)
		if (modifies(p, rte->relid))
			return "the table of keys is a table the statement modifies";
	return NULL;
}

/* the columns of the table the conditions equate to a constant or a parameter (by the equality of the column's type): atts, the count */
static int
bound_atts(Index krti, List *kquals, int *atts)
{
	int			n = 0;

	foreach_ptr(Node, q, kquals)
	{
		Node	   *l,
				   *r;

		if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
			continue;
		l = strip_relabel(linitial(((OpExpr *) q)->args));
		r = strip_relabel(lsecond(((OpExpr *) q)->args));
		if (IsA(r, Var))
		{
			Node	   *t = l;

			l = r;
			r = t;
		}
		if (IsA(l, Var) && ((Var *) l)->varno == krti && ((Var *) l)->varlevelsup == 0 && pseudo_const(r) && exprType(r) == ((Var *) l)->vartype &&
			lookup_type_cache(((Var *) l)->vartype, TYPECACHE_EQ_OPR)->eq_opr == ((OpExpr *) q)->opno)
			atts[n++] = ((Var *) l)->varattno;
	}
	return n;
}

/* the rows of the scope relation (item srti) that the keys leave: each key has the pairs PostgreSQL expects of the average key (`key = <a parameter>`), the least of them and the relation */
static double
key_pairs(PlannerInfo *root, Index srti, OpExpr *joinq, Var *kvar, double nkeys)
{
	OpExpr	   *e = copyObject(joinq);
	Param	   *prm = makeNode(Param);
	double		rows = root->simple_rel_array[srti]->rows;

	prm->paramkind = PARAM_EXEC;
	prm->paramtype = kvar->vartype;
	prm->paramtypmod = kvar->vartypmod;
	prm->paramcollid = kvar->varcollid;
	prm->location = -1;
	lfirst(list_nth_cell(e->args, equal(strip_relabel(linitial(joinq->args)), kvar) ? 0 : 1)) = prm;
	return Min(rows * nkeys * clause_selectivity(root, (Node *) e, srti, JOIN_INNER, NULL), rows);
}

/* the sub-select of the keys: SELECT key FROM <the table> WHERE <its conditions> AND key IS NOT NULL (a NULL key matches nothing in the join), over a range table of its own */
static Query *
keys_query(PlannerInfo *root, RangeTblEntry *rte, Var *kvar, List *kquals)
{
	Query	   *q = makeNode(Query);
	RangeTblEntry *r = copyObject(rte);
	RangeTblRef *ref = makeNode(RangeTblRef);
	NullTest   *nn = makeNode(NullTest);
	Var		   *key = copyObject(kvar);
	List	   *quals;

	nn->arg = (Expr *) copyObject(kvar);
	nn->nulltesttype = IS_NOT_NULL;
	nn->location = -1;
	quals = lappend(copyObject(kquals), nn);
	ChangeVarNodes((Node *) quals, kvar->varno, 1, 0);
	ChangeVarNodes((Node *) key, kvar->varno, 1, 0);
	ref->rtindex = 1;
	q->commandType = CMD_SELECT;
	q->canSetTag = true;
	q->targetList = list_make1(makeTargetEntry((Expr *) key, 1, NULL, false));
	q->rtable = list_make1(r);
	if (r->perminfoindex > 0)
	{
		q->rteperminfos = list_make1(copyObject(getRTEPermissionInfo(root->parse->rteperminfos, rte)));
		r->perminfoindex = 1;
	}
	q->jointree = makeFromExpr(list_make1(ref), (Node *) make_ands_explicit(quals));
	return q;
}

/* the keys as EXPLAIN shows them: ARRAY(SELECT k.key FROM keys k WHERE <its conditions>) */
static char *
keys_text(AggCtx *c, RangeTblEntry *rte, Var *kvar, List *kquals)
{
	const char *name = get_rel_name(rte->relid),
			   *alias = rte->eref->aliasname;

	return psprintf("ARRAY(SELECT %s FROM %s%s%s%s)", expr_text(c, (Node *) kvar), name, strcmp(alias, name) == 0 ? "" : " ", strcmp(alias, name) == 0 ? "" : alias,
					kquals == NIL ? "" : psprintf(" WHERE %s", expr_text(c, (Node *) make_ands_explicit(kquals))));
}

/*
 * The table of keys the equality joinq (the item srti's key column to column kvar of another table) names, as the keys of the scope conjunct sc. pool: the conditions of the statement
 * the table may be in; once: the scope relation is the semi join's (a row counts once). The pass takes the table out of the FROM list with the scope relation (t->krti). NULL, or why the
 * join stays.
 */
const char *
scope_key_table(AggCtx *c, FromScan *fs, List *pool, Node *joinq, Var *kvar, Index srti, bool once, SiftAggScope *sc, ScopeTake *t)
{
	PlannerInfo *root = c->root;
	Index		krti = kvar->varno;
	RangeTblEntry *rte = planner_rt_fetch(krti, root);
	RelOptInfo *rel = root->simple_rel_array[krti];
	LeftJoin   *klj = NULL;
	List	   *kquals = NIL;
	const char *why = table_why(root, rte);
	int		   *atts;
	int			n;
	bool		one;

	if (why != NULL)
		return why;
	foreach_ptr(LeftJoin, o, fs->lefts)
	{
		if (o->rti == krti && o->kind != SJ_SEMI)
			return "the table of keys is the nullable side of an outer join";
		if (o->rti == krti)
			klj = o;
		else
			foreach_ptr(Node, q, o->quals)
				if (bms_is_member(krti, pull_varnos(root, q)))
					return "the table of keys is joined to another table";
	}
	foreach_ptr(Node, q, pool)
	{
		Relids		v = pull_varnos(root, q);

		if (!bms_is_member(krti, v) || equal(q, joinq))
			continue;
		if (!bms_is_subset(v, bms_make_singleton(krti)))
			return "the table of keys is joined to another table";
		if (contain_volatile_functions(q) || raising_walker(q, c))
			return "a condition of the table of keys is volatile, reads a sub-select or an executor parameter, or may raise an error";
		kquals = list_append_unique(kquals, q);
	}
	if (read_above(root, krti))
		return "the statement reads a column of the table of keys";
	foreach_int(o, fs->rtis)
		if (o != krti && root->simple_rel_array[o] != NULL && bms_is_member(krti, root->simple_rel_array[o]->lateral_relids))
			return "another item of the FROM list reads the table of keys";
	atts = palloc((list_length(kquals) + 1) * sizeof(int));
	n = bound_atts(krti, kquals, atts);
	one = n > 0 && sift_agg_unique_key(rte->relid, n, 0, atts, NULL);
	atts[n++] = kvar->varattno;
	if (!once && !one)
		return klj == NULL && !sift_agg_unique_key(rte->relid, n, 0, atts, NULL) ?
			"the table of keys may hold a key more than once, and the join counts a row for each of them" :
			"the table of keys may give several keys, and a row counts once for each pair: a member whose pairs changed needs the join, which counts them";
	sc->nkeys = one ? 1 : (int) Min(Max(rel->rows, 1.0), 1000000.0);
	sc->keys_query = keys_query(root, rte, kvar, kquals);
	sc->param = (Node *) makeNode(Param);
	((Param *) sc->param)->paramkind = PARAM_EXEC;
	((Param *) sc->param)->paramtype = get_array_type(kvar->vartype);
	sc->param_desc = keys_text(c, rte, kvar, kquals);
	t->krti = krti;
	t->pairs = key_pairs(root, srti, (OpExpr *) joinq, kvar, sc->nkeys);
	c->nsubplan++;
	c->sub_serial = true;
	c->subplan_cost += rel->cheapest_total_path->total_cost;
	return NULL;
}

/*
 * The spec of a path that is becoming a plan, with the output of the table of keys' sub-select as the param of its scope: the sub-select is planned now, an InitPlan of the query like the one of an
 * ARRAY(SELECT ..) the statement holds, which PostgreSQL attaches to the top of the plan. The spec the planner holds is not changed (a path may become a plan more than once).
 */
const SiftAggSpec *
scope_keys_plan(PlannerInfo *root, const SiftAggSpec *s)
{
	SubLink    *sl;
	Param	   *prm;
	SiftAggSpec *spec;

	if (s->scope == NULL || s->scope->keys_query == NULL)
		return s;
	sl = makeNode(SubLink);
	sl->subLinkType = ARRAY_SUBLINK;
	sl->subselect = (Node *) s->scope->keys_query;
	sl->location = -1;
	prm = copyObject(castNode(Param, SS_process_sublinks(root, (Node *) sl, false)));
	prm->paramid += SIFT_AGG_INIT_PARAM;
	spec = memcpy(palloc(sizeof(SiftAggSpec)), s, sizeof(SiftAggSpec));
	spec->scope = memcpy(palloc(sizeof(SiftAggScope)), s->scope, sizeof(SiftAggScope));
	spec->scope->param = (Node *) prm;
	spec->scope->keys_query = NULL;
	return spec;
}
