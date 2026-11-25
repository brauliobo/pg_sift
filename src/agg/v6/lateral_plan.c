/*
 * agg/v6/lateral_plan.c - a lateral sub-select cut to one row as a dimension of the node (agg/v6/lateral_plan.h).
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
#include "utils/lsyscache.h"

#include "agg/v6/safety.h"
#include "agg/agg.h"
#include "agg/v6/plan/int.h"
#include "agg/v6/lateral_plan.h"
#include "agg/v6/source_rel.h"
#include "agg/v6/expr/pg.h"
#include "agg/v6/unique_key.h"
#include "core/compat.h"

/* the query levels above the sub-select that its expressions read: its own columns are level 0 */
typedef struct Levels
{
	bool		local;
	bool		up1;
	bool		up2;
	bool		deeper;
} Levels;

static bool
levels_walker(Node *n, Levels *r)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		const int	up = ((const Var *) n)->varlevelsup;

		r->local |= up == 0;
		r->up1 |= up == 1;
		r->up2 |= up == 2;
		r->deeper |= up > 2;
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		r->deeper = true;
		return false;
	}
	return expression_tree_walker(n, levels_walker, r);
}

static Levels
levels_of(Node *n)
{
	Levels		r = {0};

	levels_walker(n, &r);
	return r;
}

/* the columns the sub-select reads outside itself, as columns of the planner of the node: those one level up are of home (map names their relation in root), those two up are of root */
typedef struct Lift
{
	const Index *map;
	bool		ok;
} Lift;

static Node *
lift_columns(Node *n, Lift *l)
{
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, lift_columns, l);
	v = copyObject((Var *) n);
	if (l->map != NULL && v->varlevelsup == 1)
	{
		v->varno = v->varnosyn = l->map[v->varno];
		l->ok &= v->varno != 0;
	}
	v->varlevelsup = 0;
	v->varnullingrels = NULL;
	return (Node *) v;
}

/* the equalities that tie the sub-select's table to the relations before it, and the conditions of the table alone */
typedef struct Ties
{
	List	   *keys;			/* the Var of the table of each equality */
	List	   *probes;			/* the expression of the outer relations it is equal to, in the terms of the planner of the node */
	List	   *rest;
	const Index *map;
} Ties;

static bool
tie_sides(Ties *t, Node *inner, Node *outer)
{
	const Var  *key = sift_v6_plain_var(inner);
	Levels		r = levels_of(outer);
	Lift		l = {t->map, true};
	Node	   *probe;

	if (key == NULL || key->varno != 1 || key->varattno <= 0 || !(r.up1 || r.up2) || r.local || r.deeper || (r.up2 && t->map == NULL) || contain_volatile_functions(outer))
		return false;
	probe = lift_columns(outer, &l);
	if (sift_v6_plain_var(probe) != NULL)
		probe = (Node *) sift_v6_plain_var(probe);
	t->keys = lappend(t->keys, copyObject((Var *) key));
	t->probes = lappend(t->probes, probe);
	return l.ok;
}

static bool
tie_clause(Ties *t, Node *qual)
{
	const OpExpr *op = (const OpExpr *) qual;
	Levels		r = levels_of(qual);

	if (r.deeper)
		return false;
	if (!r.up1 && !r.up2)
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

/* the table of a sub-select that is one table, read through its plain relation, cut to its first row by LIMIT 1, whose quals tie it to the outer query by equalities; NULL for any other */
static const RangeTblEntry *
one_row_table(const Query *q)
{
	const RangeTblEntry *rte;

	if (q->commandType != CMD_SELECT || q->setOperations != NULL || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->hasForUpdate || q->hasRecursive ||
		q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->distinctClause != NIL || q->cteList != NIL || q->rowMarks != NIL || q->limitOffset != NULL ||
		q->limitOption != LIMIT_OPTION_COUNT || !limit_is_one(q->limitCount) ||
		list_length(q->rtable) != 1 || list_length(q->jointree->fromlist) != 1 || q->jointree->quals == NULL || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = linitial_node(RangeTblEntry, q->rtable);
	return sift_v6_rte_stored(rte) && rte->tablesample == NULL && !agg_query_rte_has_rls(q, rte) && !has_subclass(rte->relid) &&
		!contain_volatile_functions((Node *) q->targetList) && !contain_volatile_functions((Node *) q->jointree) ? rte : NULL;
}

static bool
split_ties(const Query *q, Ties *t)
{
	foreach_ptr(Node, qual, make_ands_implicit((Expr *) q->jointree->quals))
		if (!tie_clause(t, qual))
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

/* the table holds one row for each key */
static bool
unique_key(const RangeTblEntry *rte, const Ties *t)
{
	int			atts[SIFT_STAR_MAX_KEYS];
	const int	nkeys = Min(list_length(t->keys), SIFT_STAR_MAX_KEYS);

	for (int k = 0; k < nkeys; k++)
		atts[k] = ((const Var *) list_nth(t->keys, k))->varattno;
	return sift_v6_unique_key(rte->relid, nkeys, 0, atts, NULL);
}

/* the column is read above the sub-select, or by a condition of its own relation (which the plan of the dimension applies to the rows of a key) */
static bool
needed_column(const RelOptInfo *lateral, AttrNumber attno)
{
	List	   *vars = list_copy(lateral->reltarget->exprs);

	foreach_node(RestrictInfo, ri, lateral->baserestrictinfo)
		vars = list_concat(vars, pull_var_clause((Node *) ri->clause, 0));
	foreach_ptr(Node, n, vars)
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
		else
		{
			if (te->ressortgroupref == 0 && !needed_column(lateral, te->resno))
			{
				Node	   *e = (Node *) te->expr;

				te->expr = (Expr *) makeNullConst(exprType(e), exprTypmod(e), exprCollation(e));
			}
			visible = lappend(visible, te);
		}
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

/* the columns of a condition of the sub-select's relation as the relations of root: those of home are map[relid]; nothing else (a parameter) is a column of a relation */
typedef struct Remap
{
	const Index *map;
	bool		ok;
} Remap;

static Node *
remap_columns(Node *n, Remap *m)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		m->ok &= v->varlevelsup == 0 && m->map[v->varno] != 0;
		v->varno = v->varnosyn = m->map[v->varno];
		v->varnullingrels = NULL;
		return (Node *) v;
	}
	if (IsA(n, Param) || IsA(n, SubPlan) || IsA(n, PlaceHolderVar))
		m->ok = false;
	return expression_tree_mutator(n, remap_columns, m);
}

/* the conditions of the relation of the sub-select: the plan of the dimension applies them to the row it keeps for a key, as the sub-select's LIMIT left it */
static bool
dimension_quals(RelOptInfo *dim, const RelOptInfo *lateral, const Index *map)
{
	Remap		m = {map, true};

	foreach_node(RestrictInfo, ri, lateral->baserestrictinfo)
	{
		RestrictInfo *c = ri;

		if (map != NULL)
		{
			c = makeNode(RestrictInfo);
			*c = *ri;
			c->clause = (Expr *) remap_columns((Node *) ri->clause, &m);
		}
		dim->baserestrictinfo = lappend(dim->baserestrictinfo, c);
	}
	return m.ok;
}

/* the relation of the sub-select as the node reads it: relation relid, not lateral, planned by sub, with the key columns after the outputs */
static RelOptInfo *
dimension_rel(PlannerInfo *root, const RelOptInfo *lateral, Index relid, PlannerInfo *sub, const Ties *t, SiftV6LateralDim *out)
{
	RelOptInfo *dim = makeNode(RelOptInfo);
	PathTarget *target = create_empty_pathtarget();
	int			nvisible = 0;
	int			k = 0;

	*dim = *lateral;
	foreach_node(TargetEntry, te, sub->processed_tlist)
		nvisible += !te->resjunk;
	foreach_ptr(Node, n, lateral->reltarget->exprs)
	{
		Var		   *col;

		if (!IsA(n, Var))
			return NULL;
		col = copyObject((Var *) n);
		col->varno = col->varnosyn = relid;
		col->varnullingrels = NULL;
		add_column_to_pathtarget(target, (Expr *) col, 0);
	}
	foreach_node(Var, key, t->keys)
	{
		Var		   *col = makeVar(relid, nvisible - list_length(t->keys) + k + 1, key->vartype, key->vartypmod, key->varcollid, 0);

		add_column_to_pathtarget(target, (Expr *) col, 0);
		out->key[k] = col;
		out->probe[k] = list_nth(t->probes, k);
		k++;
	}
	set_pathtarget_cost_width(root, target);
	dim->relid = relid;
	dim->relids = bms_make_singleton(relid);
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
sift_v6_lateral_dim(PlannerInfo *root, PlannerInfo *home, const Index *map, RelOptInfo *lateral, Index relid, SiftV6LateralDim *out)
{
	const RangeTblEntry *lrte = planner_rt_fetch(lateral->relid, home);
	const RangeTblEntry *rte;
	Ties		t = {.map = map};
	Levels		r = {0};
	Query	   *dq;
	PlannerInfo *sub;
	Path	   *path;

	if (lateral->rtekind != RTE_SUBQUERY || lateral->lateral_relids == NULL || (rte = one_row_table(lrte->subquery)) == NULL || !split_ties(lrte->subquery, &t))
		return false;
	dq = dimension_query(lateral, lrte->subquery, &t, unique_key(rte, &t));
	query_tree_walker(dq, levels_walker, &r, 0);
	if (r.up1 || r.up2 || r.deeper)
		return false;
	sub = sift_subquery_planner(root->glob, dq, root, lrte->eref->aliasname);
	out->nkeys = list_length(t.keys);
	out->rel = dimension_rel(root, lateral, relid, sub, &t, out);
	if (out->rel == NULL || !dimension_quals(out->rel, lateral, map))
		return false;
	path = dimension_path(out->rel, sub);
	out->rel->rows = path->rows;
	out->rel->pathlist = list_make1(path);
	out->rel->cheapest_startup_path = out->rel->cheapest_total_path = path;
	out->rel->cheapest_parameterized_paths = list_make1(path);
	return true;
}

bool
sift_v6_lateral_join_dim(PlannerInfo *root, PlannerInfo *home, const Index *map, RelOptInfo *lateral, Index relid, SiftV6JoinShape *sh, int at)
{
	SiftV6JoinDim *d = &sh->dim[at];
	SiftV6LateralDim ld;

	if (!sift_v6_lateral_dim(root, home, map, lateral, relid, &ld))
		return false;
	d->rel = ld.rel;
	d->lateral = true;
	for (int k = 0; k < ld.nkeys; k++)
	{
		const int	role = sift_v6_join_expr_role(sh, ld.probe[k]);

		if (role == ROLE_NONE || role >= at || (!IsA(ld.probe[k], Var) && !sift_v6_pgexpr_plan_ok(NULL, (const Expr *) ld.probe[k])) || !sift_v6_join_add_key(d, ld.probe[k], ld.key[k]))
			return false;
	}
	return true;
}

void
sift_v6_lateral_adopt(PlannerInfo *root, const List *dim_paths)
{
	foreach_ptr(Path, path, dim_paths)
	{
		RelOptInfo *dim = path->parent;
		RelOptInfo *slot;
		Alias	   *eref;

		if (dim->relid == 0 || dim->rtekind != RTE_SUBQUERY)
			continue;
		slot = root->simple_rel_array[dim->relid];
		if (slot != NULL && slot->subroot == dim->subroot)
			continue;
		root->simple_rel_array[dim->relid] = dim;
		eref = planner_rt_fetch(dim->relid, root)->eref;
		foreach_node(TargetEntry, te, dim->subroot->processed_tlist)
			if (!te->resjunk && te->resno > list_length(eref->colnames))
				eref->colnames = lappend(eref->colnames, makeString(te->resname));
	}
}
