/*
 * agg/lateral_plan.c - a lateral sub-select cut to one row as a dimension of the node (agg/lateral_plan.h).
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

#include "agg/safety.h"
#include "agg/agg.h"
#include "agg/plan/int.h"
#include "agg/lateral_plan.h"
#include "agg/source_rel.h"
#include "agg/expr/pg.h"
#include "agg/unique_key.h"
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
	List	   *colls;			/* the collation each equality compares by */
	List	   *rest;
	List	   *gates;			/* the conditions of the outer relations alone, in the terms of the planner of the node: a row that fails one has no row of the sub-select */
	const Index *map;
} Ties;

static bool
tie_sides(Ties *t, Node *inner, Node *outer, Oid inputcollid)
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
	t->colls = lappend_oid(t->colls, inputcollid);
	return l.ok;
}

static bool
gate_clause(Ties *t, Node *qual, Levels r)
{
	Lift		l = {t->map, true};
	const bool	outer_only = !r.local && !(r.up2 && t->map == NULL) && !contain_volatile_functions(qual);

	if (outer_only)
		t->gates = lappend(t->gates, lift_columns(qual, &l));
	return outer_only && l.ok;
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
	if (IsA(qual, OpExpr) && list_length(op->args) == 2 && op_hashjoinable(op->opno, exprType(linitial(op->args))) &&
		(tie_sides(t, linitial(op->args), lsecond(op->args), op->inputcollid) || tie_sides(t, lsecond(op->args), linitial(op->args), op->inputcollid)))
		return true;
	return gate_clause(t, qual, r);
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
		!contain_volatile_functions((Node *) q->targetList) && !contain_volatile_functions((Node *) q->jointree) &&
		!agg_node_may_raise((Node *) q->targetList) && !agg_node_may_raise((Node *) q->jointree) ? rte : NULL;
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

/*
 * an output the statement reads takes a value of the row: without an ORDER BY of the sub-select, the row LIMIT 1 keeps of a key the table repeats is then the answer, the one
 * PostgreSQL's own plan of the sub-select meets first
 */
static bool
reads_row_value(const RelOptInfo *lateral, const Query *q)
{
	foreach_node(TargetEntry, te, q->targetList)
		if (!te->resjunk && needed_column(lateral, te->resno) && contain_var_clause((Node *) te->expr))
			return true;
	return false;
}

/* the scan under a path of the plan of a sub-select, through its LIMIT and its projections */
static const Path *
scan_of(const Path *p)
{
	while (IsA(p, LimitPath) || IsA(p, ProjectionPath))
		p = IsA(p, LimitPath) ? ((const LimitPath *) p)->subpath : ((const ProjectionPath *) p)->subpath;
	return p;
}

/* the cheapest path of the plan of the sub-select: the one PostgreSQL runs for each row of the outer side */
static const Path *
native_path(const RelOptInfo *lateral)
{
	const Path *best = NULL;

	foreach_ptr(Path, p, fetch_upper_rel(lateral->subroot, UPPERREL_FINAL, NULL)->pathlist)
		if (best == NULL || p->total_cost < best->total_cost)
			best = p;
	return best;
}

/*
 * The order in which PostgreSQL's plan of the sub-select meets the rows of a key: the heap's (a sequential scan: heap is set), or that of an ordered index, ascending on each
 * column, whose leading columns are the keys (an index scan meets the versions of a key in the order of their entries, which updates leave apart from the order of the heap).
 * NULL with heap unset for any other scan: a bitmap or an unordered index
 */
static const IndexOptInfo *
native_order(const RelOptInfo *lateral, const Ties *t, bool *heap)
{
	const Path *scan = scan_of(native_path(lateral));
	const IndexOptInfo *ix = IsA(scan, IndexPath) ? ((const IndexPath *) scan)->indexinfo : NULL;
	const int	nkeys = list_length(t->keys);

	*heap = scan->pathtype == T_SeqScan;
	if (ix == NULL || ix->sortopfamily == NULL || ((const IndexPath *) scan)->indexscandir != ForwardScanDirection || ix->nkeycolumns < nkeys)
		return NULL;
	for (int c = 0; c < ix->nkeycolumns; c++)
	{
		bool		tie = false;

		foreach_node(Var, key, t->keys)
			tie |= key->varattno == ix->indexkeys[c];
		if (ix->indexkeys[c] == 0 || ix->reverse_sort[c] || ix->nulls_first[c] || tie != (c < nkeys))
			return NULL;
	}
	return ix;
}

/* the entry of the column of the table among the outputs, or a junk one for it */
static TargetEntry *
column_entry(Query *q, AttrNumber attno)
{
	Oid			type,
				coll;
	int32		typmod;

	foreach_node(TargetEntry, o, q->targetList)
		if (IsA(o->expr, Var) && ((Var *) o->expr)->varattno == attno && ((Var *) o->expr)->varlevelsup == 0)
			return o;
	get_atttypetypmodcoll(rt_fetch(1, q->rtable)->relid, attno, &type, &typmod, &coll);
	return makeTargetEntry((Expr *) makeVar(1, attno, type, typmod, coll, 0), 0, "sift_order", true);
}

/*
 * the rows of a key that the table repeats are cut to the first one, DISTINCT ON the keys: in the order of the sub-select, then of the table (the row a sequential scan meets first),
 * or in the order of the columns of ix, which a scan of ix gives with the versions of a key in the order of their entries (the planner of the dimension is given that scan)
 */
static void
first_row_of_a_key(Query *q, List *keys, List *junk, const IndexOptInfo *ix)
{
	TargetEntry *tid = makeTargetEntry((Expr *) makeVar(1, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0), 0, "sift_tid", true);
	List	   *order = NIL;

	q->targetList = list_concat(q->targetList, junk);
	if (ix == NULL)
	{
		q->targetList = lappend(q->targetList, tid);
		foreach_ptr(TargetEntry, te, keys)
			order = lappend(order, sort_clause(q, te));
		q->distinctClause = list_copy(order);
		q->sortClause = lappend(list_concat(order, q->sortClause), sort_clause(q, tid));
	}
	else
	{
		for (int c = 0; c < ix->nkeycolumns; c++)
		{
			TargetEntry *te = column_entry(q, ix->indexkeys[c]);

			if (!list_member_ptr(q->targetList, te))
				q->targetList = lappend(q->targetList, te);
			order = lappend(order, sort_clause(q, te));
		}
		q->distinctClause = list_copy_head(order, list_length(keys));
		q->sortClause = order;
	}
	q->hasDistinctOn = true;
}

/* the sub-select without the equalities and the LIMIT: its outputs, then a column for each key (the first row of a key in the order of ix when it is given) */
static Query *
dimension_query(const RelOptInfo *lateral, const Query *sub, const Ties *t, bool unique, const IndexOptInfo *ix)
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
		first_row_of_a_key(q, keys, junk, ix);
	foreach_node(TargetEntry, te, q->targetList)
		te->resno = ++resno;
	return q;
}

/* the path of the planned dimension: its cheapest, or with ix the scan of ix (under the projection of the relation) that meets the rows of a key in the order of its entries, cut to the first row of each key (NULL: none) */
static Path *
first_rows_path(PlannerInfo *sub, const IndexOptInfo *ix, int nkeys)
{
	RelOptInfo *final = fetch_upper_rel(sub, UPPERREL_FINAL, NULL);

	set_cheapest(final);
	if (ix == NULL)
		return final->cheapest_total_path;
	foreach_ptr(Path, p, find_base_rel(sub, 1)->pathlist)
		if (IsA(scan_of(p), IndexPath) && ((const IndexPath *) scan_of(p))->indexinfo->indexoid == ix->indexoid && p->param_info == NULL &&
			((const IndexPath *) scan_of(p))->indexscandir == ForwardScanDirection && list_length(p->pathkeys) >= nkeys)
			return (Path *) create_projection_path(sub, final, (Path *) sift_create_sorted_unique_path(sub, final, p, nkeys, final->cheapest_total_path->rows),
												   sub->upper_targets[UPPERREL_FINAL]);
	return NULL;
}

/* the scan of the planned dimension: parallel safe as its plan is, which has no LIMIT (the first row of each key, ties split by the row) and gives the same rows in any process */
static Path *
dimension_path(RelOptInfo *dim, Path *subpath)
{
	SubqueryScanPath *p = makeNode(SubqueryScanPath);

	p->path.pathtype = T_SubqueryScan;
	p->path.parent = dim;
	p->path.pathtarget = dim->reltarget;
	p->path.parallel_safe = subpath->parallel_safe;
	p->subpath = subpath;
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
		out->coll[k] = list_nth_oid(t->colls, k);
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
	const IndexOptInfo *ix = NULL;
	bool		unique,
				heap = true;

	if (lateral->rtekind != RTE_SUBQUERY || lateral->lateral_relids == NULL || (rte = one_row_table(lrte->subquery)) == NULL || !split_ties(lrte->subquery, &t))
		return false;
	unique = unique_key(rte, &t);
	if (!unique && lrte->subquery->sortClause == NIL && reads_row_value(lateral, lrte->subquery))
		ix = native_order(lateral, &t, &heap);
	if (ix == NULL && !heap)
		return false;
	dq = dimension_query(lateral, lrte->subquery, &t, unique, ix);
	query_tree_walker(dq, levels_walker, &r, 0);
	if (r.up1 || r.up2 || r.deeper)
		return false;
	sub = sift_subquery_planner(root->glob, dq, root, lrte->eref->aliasname);
	out->nkeys = list_length(t.keys);
	out->gates = t.gates;
	out->rel = dimension_rel(root, lateral, relid, sub, &t, out);
	if (out->rel == NULL || !dimension_quals(out->rel, lateral, map))
		return false;
	if ((path = first_rows_path(sub, ix, out->nkeys)) == NULL)
		return false;
	path = dimension_path(out->rel, path);
	out->rel->rows = path->rows;
	out->rel->pathlist = list_make1(path);
	out->rel->cheapest_startup_path = out->rel->cheapest_total_path = path;
	out->rel->cheapest_parameterized_paths = list_make1(path);
	return true;
}

/* the probe of the first key where the outer row passes the gates, else NULL, which matches no row of the sub-select: a row that fails a gate has none, as the sub-select gives it no row */
static const Node *
gated_probe(const Node *probe, List *gates)
{
	CaseExpr   *c = makeNode(CaseExpr);
	CaseWhen   *w = makeNode(CaseWhen);

	w->expr = make_ands_explicit(gates);
	w->result = (Expr *) copyObject(probe);
	w->location = -1;
	c->casetype = exprType(probe);
	c->casecollid = exprCollation(probe);
	c->args = list_make1(w);
	c->defresult = (Expr *) makeNullConst(c->casetype, exprTypmod(probe), c->casecollid);
	c->location = -1;
	return (const Node *) c;
}

bool
sift_v6_lateral_join_dim(PlannerInfo *root, PlannerInfo *home, const Index *map, RelOptInfo *lateral, Index relid, SiftV6JoinShape *sh, int at)
{
	SiftV6JoinDim *d = &sh->dim[at];
	SiftV6LateralDim ld;

	if (!sift_v6_lateral_dim(root, home, map, lateral, relid, &ld))
		return false;
	if (ld.gates != NIL)
		ld.probe[0] = gated_probe(ld.probe[0], ld.gates);
	d->rel = ld.rel;
	d->lateral = true;
	for (int k = 0; k < ld.nkeys; k++)
	{
		const int	role = sift_v6_join_expr_role(sh, ld.probe[k]);

		if (role == ROLE_NONE || role >= at || (!IsA(ld.probe[k], Var) && !sift_v6_pgexpr_plan_ok(NULL, (const Expr *) ld.probe[k])) || !sift_v6_join_add_key(d, ld.probe[k], ld.key[k], ld.coll[k]))
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
