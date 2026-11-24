/*
 * agg/v6/v6_lateral_plan.c - a lateral sub-select cut to one row as a dimension of the node (agg/v6/v6_lateral_plan.h).
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_lateral_plan.h"
#include "core/compat.h"

typedef struct Refs
{
	bool		local;
	bool		outer;
	bool		other;
} Refs;

static bool
refs_walker(Node *n, Refs *r)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		const int	up = ((const Var *) n)->varlevelsup;

		r->local |= up == 0;
		r->outer |= up == 1;
		r->other |= up > 1;
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		r->other = true;
		return false;
	}
	return expression_tree_walker(n, refs_walker, r);
}

static Refs
refs_of(Node *n)
{
	Refs		r = {0};

	refs_walker(n, &r);
	return r;
}

/* the equalities that tie the sub-select's table to the relations before it, and the conditions of the table alone */
typedef struct Ties
{
	List	   *keys;			/* the Var of the table of each equality */
	List	   *probes;			/* the expression of the outer relations it is equal to, as of the outer query */
	List	   *rest;
} Ties;

static bool
tie_sides(Ties *t, Node *inner, Node *outer)
{
	const Var  *key = sift_v6_plain_var(inner);
	const Var  *column = sift_v6_plain_var(outer);
	Refs		r = refs_of(outer);
	Node	   *probe;

	if (key == NULL || key->varno != 1 || key->varlevelsup != 0 || key->varattno <= 0 || !r.outer || r.local || r.other || contain_volatile_functions(outer))
		return false;
	probe = (Node *) copyObject(column != NULL ? (const Node *) column : outer);
	IncrementVarSublevelsUp(probe, -1, 0);
	t->keys = lappend(t->keys, (Var *) copyObject(key));
	t->probes = lappend(t->probes, probe);
	return true;
}

static bool
tie_of(Ties *t, Node *qual)
{
	const OpExpr *op = (const OpExpr *) qual;
	Refs		r = refs_of(qual);

	if (r.other)
		return false;
	if (!r.outer)
	{
		t->rest = lappend(t->rest, qual);
		return true;
	}
	return IsA(qual, OpExpr) && list_length(op->args) == 2 && op_hashjoinable(op->opno, exprType(linitial(op->args))) &&
		(tie_sides(t, linitial(op->args), lsecond(op->args)) || tie_sides(t, lsecond(op->args), linitial(op->args)));
}

/* the parser leaves the LIMIT as the cast of its constant to bigint */
static bool
limit_is_one(Node *limit)
{
	const Const *c = (const Const *) (limit == NULL ? NULL : eval_const_expressions(NULL, limit));

	return c != NULL && IsA(c, Const) && !c->constisnull && c->consttype == INT8OID && DatumGetInt64(c->constvalue) == 1;
}

/* one table, read through its plain relation, cut to its first row by LIMIT 1, whose quals tie it to the outer query by equalities */
static bool
one_row_of_a_table(const Query *q)
{
	const RangeTblEntry *rte;

	if (q->commandType != CMD_SELECT || q->setOperations != NULL || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->hasForUpdate || q->hasRecursive ||
		q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->distinctClause != NIL || q->cteList != NIL || q->rowMarks != NIL || q->limitOffset != NULL ||
		q->limitOption != LIMIT_OPTION_COUNT || !limit_is_one(q->limitCount) ||
		list_length(q->rtable) != 1 || list_length(q->jointree->fromlist) != 1 || q->jointree->quals == NULL || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return false;
	rte = linitial_node(RangeTblEntry, q->rtable);
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && rte->tablesample == NULL && rte->securityQuals == NIL && !has_subclass(rte->relid) &&
		!contain_volatile_functions((Node *) q->targetList) && !contain_volatile_functions((Node *) q->jointree);
}

static bool
split_ties(const Query *q, Ties *t)
{
	foreach_ptr(Node, qual, make_ands_implicit((Expr *) q->jointree->quals))
		if (!tie_of(t, qual))
			return false;
	return t->keys != NIL && list_length(t->keys) <= SIFT_STAR_MAX_KEYS;
}

static SortGroupClause *
sort_clause(Query *q, TargetEntry *tle)
{
	SortGroupClause *sgc = makeNode(SortGroupClause);

	get_sort_group_operators(exprType((Node *) tle->expr), true, true, false, &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
	sgc->tleSortGroupRef = assignSortGroupRef(tle, q->targetList);
	return sgc;
}

static bool
unique_key(const Query *q, const Ties *t)
{
	int			atts[SIFT_STAR_MAX_KEYS];
	int			n = 0;

	foreach_node(Var, key, t->keys)
		atts[n++] = key->varattno;
	return sift_agg_unique_key(linitial_node(RangeTblEntry, q->rtable)->relid, n, 0, atts, NULL);
}

static bool
needed_column(const RelOptInfo *lateral, AttrNumber attno)
{
	foreach_ptr(Node, n, lateral->reltarget->exprs)
		if (IsA(n, Var) && ((Var *) n)->varattno == attno)
			return true;
	return false;
}

/* the outputs the statement does not read are NULL, as PostgreSQL plans a sub-select it reads some columns of */
static List *
visible_outputs(const RelOptInfo *lateral, Query *q, List **junk)
{
	List	   *visible = NIL;

	foreach_node(TargetEntry, te, q->targetList)
	{
		if (te->resjunk)
			*junk = lappend(*junk, te);
		else if (te->ressortgroupref == 0 && !needed_column(lateral, te->resno))
		{
			Node	   *e = (Node *) te->expr;

			te->expr = (Expr *) makeNullConst(exprType(e), exprTypmod(e), exprCollation(e));
			visible = lappend(visible, te);
		}
		else
			visible = lappend(visible, te);
	}
	return visible;
}

/* the rows of a key that the table repeats are cut to the first one: DISTINCT ON the keys, in the order of the sub-select, then of the table (the row a scan with no ORDER BY meets first) */
static void
first_row_of_a_key(Query *q, List *keys, List *junk)
{
	TargetEntry *tid = makeTargetEntry((Expr *) makeVar(1, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0), 0, "sift_tid", true);
	List	   *order = NIL;

	q->targetList = list_concat(q->targetList, lappend(junk, tid));
	foreach_ptr(TargetEntry, te, keys)
		order = lappend(order, sort_clause(q, te));
	q->distinctClause = list_copy(order);
	q->hasDistinctOn = true;
	q->sortClause = lappend(list_concat(order, q->sortClause), sort_clause(q, tid));
}

/* the sub-select without the equalities and the LIMIT: its outputs, then a column for each key */
static Query *
dimension_query(const RelOptInfo *lateral, const Query *sub, const Ties *t, bool unique)
{
	Query	   *q = (Query *) copyObject(sub);
	List	   *junk = NIL;
	List	   *keys = NIL;
	int			k = 0;
	int			resno = 0;

	q->targetList = visible_outputs(lateral, q, &junk);
	q->jointree->quals = t->rest == NIL ? NULL : (Node *) make_ands_explicit(copyObject(t->rest));
	q->limitCount = NULL;
	foreach_node(Var, key, t->keys)
	{
		TargetEntry *te = makeTargetEntry((Expr *) copyObject((Node *) key), 0, psprintf("sift_key%d", ++k), false);

		q->targetList = lappend(q->targetList, te);
		keys = lappend(keys, te);
	}
	if (unique)
		q->sortClause = NIL;
	else
		first_row_of_a_key(q, keys, junk);
	foreach_node(TargetEntry, te, q->targetList)
		te->resno = ++resno;
	return q;
}

static Path *
dimension_path(RelOptInfo *dim, PlannerInfo *sub)
{
	RelOptInfo *final = fetch_upper_rel(sub, UPPERREL_FINAL, NULL);
	SubqueryScanPath *p = makeNode(SubqueryScanPath);

	set_cheapest(final);
	p->path.pathtype = T_SubqueryScan;
	p->path.parent = dim;
	p->path.pathtarget = dim->reltarget;
	p->subpath = final->cheapest_total_path;
	p->path.rows = p->subpath->rows;
	p->path.startup_cost = p->subpath->startup_cost;
	p->path.total_cost = p->subpath->total_cost + cpu_tuple_cost * p->path.rows;
	return (Path *) p;
}

/* the relation of the sub-select as the node reads it: not lateral, planned by sub, with the key columns after the outputs */
static RelOptInfo *
dimension_rel(PlannerInfo *root, const RelOptInfo *lateral, PlannerInfo *sub, const Ties *t, SiftV6LateralDim *out)
{
	RelOptInfo *dim = makeNode(RelOptInfo);
	PathTarget *target = create_empty_pathtarget();
	int			nvisible = 0;
	int			k = 0;

	*dim = *lateral;
	foreach_node(TargetEntry, te, sub->processed_tlist)
		nvisible += !te->resjunk;
	foreach_ptr(Node, n, lateral->reltarget->exprs)
		add_column_to_pathtarget(target, (Expr *) n, 0);
	foreach_node(Var, key, t->keys)
	{
		Var		   *col = makeVar(lateral->relid, nvisible - list_length(t->keys) + k + 1, key->vartype, key->vartypmod, key->varcollid, 0);

		add_column_to_pathtarget(target, (Expr *) col, 0);
		out->key[k] = col;
		out->probe[k] = list_nth(t->probes, k);
		k++;
	}
	set_pathtarget_cost_width(root, target);
	dim->reltarget = target;
	dim->lateral_relids = NULL;
	dim->lateral_vars = NIL;
	dim->subroot = sub;
	dim->subplan_params = NIL;
	dim->baserestrictinfo = NIL;
	dim->pathlist = NIL;
	dim->partial_pathlist = NIL;
	return dim;
}

bool
sift_v6_lateral_dim(PlannerInfo *root, RelOptInfo *lateral, SiftV6LateralDim *out)
{
	const RangeTblEntry *rte = planner_rt_fetch(lateral->relid, root);
	Ties		t = {0};
	Query	   *dq;
	PlannerInfo *sub;
	Path	   *path;

	if (lateral->rtekind != RTE_SUBQUERY || lateral->lateral_relids == NULL || !one_row_of_a_table(rte->subquery) || !split_ties(rte->subquery, &t))
		return false;
	dq = dimension_query(lateral, rte->subquery, &t, unique_key(rte->subquery, &t));
	sub = sift_subquery_planner(root->glob, dq, root, rte->eref->aliasname);
	out->nkeys = list_length(t.keys);
	out->rel = dimension_rel(root, lateral, sub, &t, out);
	path = dimension_path(out->rel, sub);
	out->rel->rows = path->rows;
	out->rel->pathlist = list_make1(path);
	out->rel->cheapest_startup_path = out->rel->cheapest_total_path = path;
	out->rel->cheapest_parameterized_paths = list_make1(path);
	return true;
}

void
sift_v6_lateral_adopt(PlannerInfo *root, const List *dim_paths)
{
	foreach_ptr(Path, path, dim_paths)
	{
		RelOptInfo *dim = path->parent;
		Alias	   *eref;

		if (dim->relid == 0 || dim->rtekind != RTE_SUBQUERY || root->simple_rel_array[dim->relid] == dim)
			continue;
		root->simple_rel_array[dim->relid] = dim;
		eref = planner_rt_fetch(dim->relid, root)->eref;
		foreach_node(TargetEntry, te, dim->subroot->processed_tlist)
			if (!te->resjunk && te->resno > list_length(eref->colnames))
				eref->colnames = lappend(eref->colnames, makeString(te->resname));
	}
}
