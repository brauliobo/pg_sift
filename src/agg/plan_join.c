/*
 * agg/plan_join.c - the path with lookup joins: joins and the order of the lookups (the children are agg/plan_child.c)
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"
#include "agg/plan_join_int.h"

bool
unsupported_clause(AggCtx *c, Node *n)
{
	return n != NULL && (contain_volatile_functions(n) || has_exec_param_walker(n, c));
}

/* does the right side read a column (or aggregate, or CTE) of the query, or above it when skip is 1? */
bool
reads_outer(AggCtx *c, RangeTblEntry *e, int skip)
{
	/* the expressions of a function or VALUES list are in the query itself, the body of a sub-select one level below it */
	OuterRef	oref = {c->root, e->rtekind == RTE_SUBQUERY ? 0 : -1, skip};
	Node	   *expr = e->rtekind == RTE_VALUES ? (Node *) e->values_lists : (Node *) e->functions;
	bool		r;

	if (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION)
		return false;
	if (walk_memo_get(e, c->root, skip, &r))
		return r;
#ifdef SIFT_TESTING
	sift_agg_outer_walks++;
#endif
	if (e->rtekind == RTE_SUBQUERY)
		r = query_tree_walker(e->subquery, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
	else
		r = outer_ref_walker(expr, &oref) || (skip && ancestor_param_walker(expr, c->root));
	walk_memo_put(e, c->root, skip, r);
	return r;
}

/*
 * The right side of a join that is a plan: a sub-select PostgreSQL did not pull up (GROUP BY, LIMIT, DISTINCT, a set operation), a VALUES list
 * or a function that reads nothing of the query's other tables. Its rows are those of PostgreSQL's plan of it, read once when the scan starts
 * (SiftAggJoin.child). A correlated subquery keeps its native parameterized path and is rescanned per cached input tuple.
 */
static bool
add_plan_join(AggCtx *c, RangeTblEntry *e, Index rti, JoinCtx *jc, const char **what)
{
	RelOptInfo *rel = c->root->simple_rel_array[rti];

	switch (e->rtekind)
	{
		case RTE_RELATION:
			*what = e->relkind == RELKIND_PARTITIONED_TABLE ? "partitioned" : "filtered";
			break;
		case RTE_CTE:
		{
			PlannerInfo *owner = c->root;

			for (int level = 0; level < e->ctelevelsup; level++)
				owner = owner->parent_root;
			*what = "cte";
			/* A CTE of a query level that reads nothing of the queries around it has no changing outer-row inputs, wherever it is referenced. */
			if (e->self_reference || cte_owner_correlated(owner))
				return agg_refuse_on(JOIN_CHILD, "the CTE is recursive, or reads the queries around it");
			break;
		}
		case RTE_SUBQUERY:
			*what = "subquery";
			if (e->subquery->rowMarks != NIL || contain_volatile_functions((Node *) e->subquery))
				return agg_refuse_on(JOIN_CHILD, "the sub-select locks rows or calls a volatile function");
			break;
		case RTE_VALUES:
			*what = "values";
			if (contain_volatile_functions((Node *) e->values_lists))
				return agg_refuse_on(JOIN_CHILD, "the VALUES list calls a volatile function");
			break;
		default:
			*what = "function";
			if (contain_volatile_functions((Node *) e->functions))
				return agg_refuse_on(JOIN_CHILD, "the function in FROM is volatile");
			break;
	}
	/*
	 * A column of a query above the aggregate's own reaches the child as a parameter of that query's plan, which changes per row of it; the
	 * scan replays its groups when rescanned (agg_rescan) and never sees it. Only the query's own tables are an input of the child.
	 */
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || reads_outer(c, e, 1))
		return agg_refuse_on(JOIN_CHILD, "the child is not a base relation of this query level, or reads the queries around it");
	if (reads_outer(c, e, 0) || !bms_is_empty(rel->lateral_relids))
		return (e->rtekind == RTE_SUBQUERY || e->rtekind == RTE_FUNCTION) && planned_correlated_child(c, e, rel, jc);
	if ((jc->cpath = child_path(rel, false)) == NULL)
		return agg_refuse_on(JOIN_CHILD, "the child has no plan the scan can run once");
	if (!agg_lookup_fits(jc->cpath->rows, jc->cpath->pathtarget->width))
		return agg_refuse_limit(JOIN_CHILD, lookup_kb, sift_agg_lookup_max_kb, "the child is estimated at %.0f rows of width %d", jc->cpath->rows, jc->cpath->pathtarget->width);
	jc->j.child = 0;
	foreach_ptr(JoinCtx, o, c->joins)
		jc->j.child += o->cpath != NULL && o->j.params == NIL;
	jc->j.sig = plain_child_sig(c, e, rti, rel);
	return true;
}

/*
 * The ON conjuncts of a join that keeps the other table and read only the scanned table (and the rows it unnests) decide which of its
 * rows can match, as the planner would filter them before the join: they become conditions of the scan, which an EXISTS or a jsonpath test
 * can be too. The rest is the join's own.
 */
static List *
nullable_filters(AggCtx *c, FromScan *fs, List *quals, Index dim)
{
	List	   *rest = NIL;
	Relids		own = bms_make_singleton(c->relid),
				nullable = NULL;

	if (c->func_rti)
		own = bms_add_member(own, c->func_rti);
	/* an EXISTS may read any table of the nullable input: it filters the joined tuples */
	foreach_int(rti, fs->rtis)
		if (rti != dim)
			nullable = bms_add_member(nullable, rti);
	foreach_ptr(Node, q, quals)
	{
		Relids		v = pull_varnos(c->root, q);

		if (!bms_is_empty(v) && bms_is_subset(v, IsA(q, SubPlan) ? nullable : own) && !contain_volatile_functions(q))
			fs->scan_quals = lappend(fs->scan_quals, q);
		else
			rest = lappend(rest, q);
	}
	return rest;
}

/*
 * The right side of a semi or anti join that is a tree of joined tables (IN (SELECT .. FROM a LEFT JOIN b ..)): PostgreSQL's plan of the join it
 * made of the tree, read once when the scan starts, gives the keys. It must not read the rows of the query's other tables.
 */
static bool
add_tree_join(AggCtx *c, LeftJoin *lj, JoinCtx *jc)
{
	RelOptInfo *rel = find_join_rel(c->root, lj->tree);
	Path	   *best = NULL;

	if (rel == NULL)
		return agg_refuse_on(JOIN_CHILD, "PostgreSQL did not plan the tables on the right of the semi or anti join as a join of their own");
	if (!bms_is_empty(rel->lateral_relids) || contain_volatile_functions((Node *) c->root->parse->jointree))
		return agg_refuse_on(JOIN_CHILD, "the tables on the right of the semi or anti join read the query's other tables, or call a volatile function");
	foreach_ptr(Path, p, rel->pathlist)
		if (p->param_info == NULL && !p->parallel_aware && p->parallel_workers == 0 && !IsA(p, GatherPath) && !IsA(p, GatherMergePath) &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	if (best == NULL)
		return agg_refuse_on(JOIN_CHILD, "the tables on the right of the semi or anti join have no plan the scan can run once");
	if (!agg_lookup_fits(best->rows, best->pathtarget->width))
		return agg_refuse_limit(JOIN_CHILD, lookup_kb, sift_agg_lookup_max_kb, "the keys of the semi or anti join are estimated at %.0f rows of width %d", best->rows, best->pathtarget->width);
	jc->cpath = best;
	jc->tree = true;
	jc->j.child = 0;
	foreach_ptr(JoinCtx, o, c->joins)
		jc->j.child += o->cpath != NULL && o->j.params == NIL;
	return true;
}

/* a table or plan in the FROM list besides the scanned one: it joins by equality of its columns to leaves */
bool
add_join(AggCtx *c, FromScan *fs, Index rti)
{
	RangeTblEntry *e = rt_fetch(rti, c->root->parse->rtable);
	JoinCtx    *jc = palloc0(sizeof(JoinCtx));
	const char *what = NULL;
	bool		filtered = false;
	LeftJoin   *tree = NULL;

	jc->rti = rti;
	jc->j.child = -1;
	jc->j.fslot = -1;
	/* the rows of a table of a FULL JOIN that a condition of its sub-select or view left out are no rows of the join: its plan reads them */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		filtered |= lj->rti == rti && lj->kind == SJ_FULL && c->root->simple_rel_array[rti] != NULL &&
			c->root->simple_rel_array[rti]->baserestrictinfo != NIL;
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == rti && lj->tree != NULL)
			tree = lj;
	if (tree != NULL)
	{
		if (!add_tree_join(c, tree, jc))
			return agg_refuse(JOIN_CHILD);
		what = "join";
	}
	else if (e->rtekind == RTE_RELATION && e->relkind != RELKIND_PARTITIONED_TABLE && !filtered)
	{
		if (e->relkind != RELKIND_RELATION)
			return agg_refuse_f(TABLE_KIND, "the joined relation %s is not a plain table", get_rel_name(e->relid));
		jc->j.relid = e->relid;
	}
	else if (!add_plan_join(c, e, rti, jc, &what))
		return agg_refuse(JOIN_CHILD);
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == rti)
		{
			jc->kind = lj->kind;
			jc->clauses = lj->kind == SJ_RIGHT ? nullable_filters(c, fs, lj->quals, rti) : lj->quals;
		}
	jc->j.kind = jc->kind;
	jc->j.desc = psprintf("%s%s", what ? (e->rtekind == RTE_VALUES ? "values" : psprintf("%s (%s)", e->eref->aliasname, what)) : get_rel_name(e->relid),
						  jc->kind == SJ_LEFT ? " (left)" : jc->kind == SJ_SEMI ? " (semi)" : jc->kind == SJ_ANTI ? " (anti)" :
						  jc->kind == SJ_RIGHT ? " (preserved)" : jc->kind == SJ_FULL ? " (full)" : "");
	jc->combo = combo_index(c, new_combo(c, list_length(c->joins)));
	c->joins = lappend(c->joins, jc);
	return true;
}

/*
 * `column = value` or `column = ANY (array)` (IN (list)) among the table's own conditions (the value reads no column, is not volatile and takes no
 * executor parameter): the rows of the table that can match are those of an index scan on the column, when the table has a btree on it (the executor
 * looks, map_load). The conjunct stays in the filter; this only narrows what is read. A condition that keeps a large share of the table is read faster
 * by the heap scan than through the index (a random heap fetch for each row), so it is only for a condition the statistics of the table expect to keep
 * a small share.
 */
static bool
index_qual(AggCtx *c, JoinCtx *jc, Node *q, Oid *opno, Oid *coll, Node **col, Node **val, bool *any)
{
	Node	   *l,
			   *r;

	if (IsA(q, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) q)->useOr)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) q;

		*opno = sa->opno;
		*coll = sa->inputcollid;
		l = strip_relabel(linitial(sa->args));
		r = lsecond(sa->args);
		*any = true;
		if (join_of_var(c, l) != jc || clause_selectivity(c->root, q, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
			return false;
	}
	else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) q;

		*opno = op->opno;
		*coll = op->inputcollid;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		*any = false;
		if (join_of_var(c, l) != jc)
		{
			if (join_of_var(c, r) != jc)
				return false;
			*opno = get_commutator(*opno);
			l = r;
			r = strip_relabel(linitial(op->args));
		}
		if (clause_selectivity(c->root, q, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
			return false;
	}
	else
		return false;
	*col = l;
	*val = r;
	return OidIsValid(*opno) && op_mergejoinable(*opno, exprType(l)) && !unsupported_clause(c, r) && !contain_var_clause(r);
}

static void
join_index_quals(AggCtx *c, JoinCtx *jc, List *conds)
{
	SiftAggJoin *j = &jc->j;

	if (jc->cpath != NULL || sift_aggjoin_preserves(jc->kind))
		return;
	foreach_ptr(Node, q, conds)
	{
		Node	   *col,
				   *val;
		Oid			opno,
					coll;
		bool		any;

		if (j->nidx == SIFT_AGG_MAX_JIDX || !index_qual(c, jc, q, &opno, &coll, &col, &val, &any))
			continue;
		j->idxatt[j->nidx] = ((Var *) col)->varattno;
		j->idxop[j->nidx] = opno;
		j->idxcoll[j->nidx] = coll;
		j->idxany[j->nidx] = any;
		j->idxval[j->nidx++] = val;
	}
}

/* the rest of the ON clause of a joined table: a boolean slot of its combo, over the leaves and the columns of a candidate row */
static bool
join_filter(AggCtx *c, JoinCtx *jc, List *conds)
{
	int			ci,
				si;

	join_index_quals(c, jc, conds);

	if (!combo_slot_in(c, jc, normalize_expr(c, (Node *) make_ands_explicit(conds)), false, false, &ci, &si) || ci != jc->combo)
		return agg_refuse_on(JOIN_CONDITION, "the join filter is not an expression of the scanned table's leaves and the joined columns");
	jc->j.fslot = si;
	return true;
}

bool
preserved_join(AggCtx *c)
{
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			return true;
	return false;
}

/* Nulling join RTIs describe expression boundaries, not lookup inputs. */
static Relids
lookup_inputs(AggCtx *c, Node *q)
{
	Relids		vars = pull_varnos(c->root, q);

	for (int rti = -1; (rti = bms_next_member(vars, rti)) >= 0;)
		if (rt_fetch(rti, c->root->parse->rtable)->rtekind == RTE_JOIN)
			vars = bms_del_member(vars, rti);
	return vars;
}

/* A conjunct can probe this lookup when all its other inputs have already been produced. */
static bool
lookup_ready(AggCtx *c, JoinCtx *jc, List *quals, Relids done)
{
	Relids		available = bms_add_member(bms_copy(done), jc->rti);

	if (jc->j.params != NIL)
		return bms_is_subset(lookup_inputs(c, (Node *) jc->args), done);
	foreach_ptr(Node, q, quals)
	{
		Relids		vars = lookup_inputs(c, q);

		if (bms_is_member(jc->rti, vars) && bms_is_subset(vars, available))
			return true;
	}
	return false;
}

static bool
order_inner_lookups(AggCtx *c, List *pending, List *quals, List **ordered, Relids *done)
{
	while (pending != NIL)
	{
		JoinCtx    *next = NULL;

		foreach_ptr(JoinCtx, jc, pending)
			if (lookup_ready(c, jc, quals, *done))
			{
				next = jc;
				break;
			}
		if (next == NULL)
			return false;
		*ordered = lappend(*ordered, next);
		*done = bms_add_member(*done, next->rti);
		pending = list_delete_ptr(pending, next);
	}
	return true;
}

/* Inner lookups commute within a join boundary; outer/semi/anti joins keep their relative position. */
static bool
order_lookups(AggCtx *c, List *quals)
{
	List	   *pending = NIL,
			   *ordered = NIL,
			   *combos = NIL;
	Relids		done = bms_make_singleton(c->relid);
	int			child = 0;

	if (c->func_rti)
		done = bms_add_member(done, c->func_rti);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		if (jc->kind == SJ_INNER)
		{
			pending = lappend(pending, jc);
			continue;
		}
		if (!order_inner_lookups(c, pending, quals, &ordered, &done))
			return agg_refuse(JOIN_CONDITION);
		pending = NIL;
		foreach_ptr(Node, q, jc->clauses)
			if (!bms_is_subset(lookup_inputs(c, q), bms_add_member(bms_copy(done), jc->rti)))
				return agg_refuse_on(JOIN_CONDITION, "a join condition reads a table that no earlier lookup reaches");
		if (jc->j.params != NIL && !bms_is_subset(lookup_inputs(c, (Node *) jc->args), done))
			return agg_refuse_on(JOIN_CONDITION, "the parameters of a joined child read a table that no earlier lookup reaches");
		ordered = lappend(ordered, jc);
		done = bms_add_member(done, jc->rti);
	}
	if (!order_inner_lookups(c, pending, quals, &ordered, &done))
		return agg_refuse(JOIN_CONDITION);
	Assert(list_length(c->combos) == list_length(c->joins));
	foreach_ptr(JoinCtx, jc, ordered)
	{
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		jc->combo = list_length(combos);
		cb->join = jc->combo;
		combos = lappend(combos, cb);
		if (jc->cpath && jc->j.params == NIL)
			jc->j.child = child++;
	}
	c->joins = ordered;
	c->combos = combos;
	return true;
}

/* the columns of a joined sub-select that its plan outputs: those the query reads, and (the plan evaluates them) those of the conditions it keeps */
static Bitmapset *
child_kept_attrs(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *rel = find_base_rel(c->root, jc->rti);
	Bitmapset  *used = NULL;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &used);
	if (projects_columns(jc->cpath))
		foreach_ptr(RestrictInfo, ri, list_concat_copy(rel->baserestrictinfo, rel->joininfo))
			pull_varattnos((Node *) ri->clause, rel->relid, &used);
	return used;
}

/*
 * A column of a sub-select that only a condition pushed down into it read is a NULL in its plan (remove_unused_subquery_outputs), so the
 * spec, which reads the condition again, must not read it: the normal plan stays. The plan of a sub-select, VALUES list or function keeps
 * the columns of the conditions it evaluates; an Append or MergeAppend outputs the relation's target only.
 */
static bool
child_cols_kept(AggCtx *c, JoinCtx *jc)
{
	Bitmapset  *used = child_kept_attrs(c, jc);

	foreach_ptr(Var, v, jc->cvars)
		if (!bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, used))
			return false;
	return true;
}

/*
 * A condition over the columns of a joined sub-select that PostgreSQL pushed down into it (a run condition of a ranking window, a filter of
 * its scans): its plan enforces it, so its rows are those the condition keeps and the scan does not test it again. Such a condition is
 * recognized by a column that nothing else of the query keeps in the plan's output.
 */
static bool
pushed_into_child(AggCtx *c, JoinCtx *jc, Node *q)
{
	Bitmapset  *vars = NULL;

	if (jc->cpath == NULL || jc->j.params != NIL || !projects_columns(jc->cpath) || !bms_equal(lookup_inputs(c, q), bms_make_singleton(jc->rti)))
		return false;
	pull_varattnos(q, jc->rti, &vars);
	return bms_nonempty_difference(vars, child_kept_attrs(c, jc));
}

/* the conditions of the list that no plan of a joined sub-select has enforced already */
static List *
drop_pushed(AggCtx *c, List *quals)
{
	List	   *kept = NIL;

	foreach_ptr(Node, q, quals)
	{
		bool		pushed = false;

		foreach_ptr(JoinCtx, jc, c->joins)
			pushed |= pushed_into_child(c, jc, q);
		if (!pushed)
			kept = lappend(kept, q);
	}
	return kept;
}

/* every column of the query's own level is NULL, as in the row an outer join makes for a preserved row that nothing matched */
static Node *
null_row_mutator(Node *n, void *ctx)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0)
		return (Node *) makeNullConst(((Var *) n)->vartype, ((Var *) n)->vartypmod, ((Var *) n)->varcollid);
	if (IsA(n, PlaceHolderVar))
		return null_row_mutator((Node *) ((PlaceHolderVar *) n)->phexpr, ctx);
	return expression_tree_mutator(n, null_row_mutator, ctx);
}

/* does the condition hold for the row of NULL columns? Then it is no filter of the rows the scan reads, but of the joined rows */
static bool
null_row_passes(Node *q)
{
	Node	   *e;

	if (contain_volatile_functions(q) || checkExprHasSubLink(q) || contain_subplans(q))
		return false;
	e = eval_const_expressions(NULL, null_row_mutator(q, NULL));
	return IsA(e, Const) && !((Const *) e)->constisnull && DatumGetBool(((Const *) e)->constvalue);
}

/*
 * The conjuncts of the WHERE and of the ON clauses that read a joined table. Those of an inner join are its ON clause (the same rows
 * either way); those of an outer, semi or anti join filter its output rows (*post). In the ON clause of every join an equality of a
 * column with a leaf, or with an expression of leaves, is a key; the rest filters the table's rows for each tuple (join_filter).
 * *quals is left with the conjuncts that read no joined table.
 */
static bool
join_quals(AggCtx *c, List **quals, List **post, bool nullable_input)
{
	List	   *rest = NIL;

	foreach_ptr(Node, q, *quals)
	{
		Relids		v = pull_varnos(c->root, q);
		JoinCtx    *owner = NULL;

		foreach_ptr(JoinCtx, jc, c->joins)
			if (bms_is_member(jc->rti, v))
			{
				owner = jc;
			}
		if (owner == NULL)
			rest = lappend(rest, q);
		else if (owner->kind == SJ_INNER && (nullable_input || !preserved_join(c)) && !IsA(q, SubPlan))
			owner->clauses = lappend(owner->clauses, q);
		else
			*post = lappend(*post, q);
	}
	/*
	 * A WHERE that reads the scanned table only is evaluated after a preserved join: a row it drops after matching is no row of the join, so
	 * it is no filter of the rows the scan reads but of the matched tuples (post), and the rows nothing matched, which have NULL columns,
	 * must pass it (a condition they fail would drop them, which the scan does not).
	 */
	if (rest != NIL && !nullable_input && preserved_join(c))
	{
		foreach_ptr(Node, q, rest)
		{
			if (!null_row_passes(q))
				return agg_refuse_on(OUTER_JOIN, "a WHERE the rows of the preserved side that nothing matched fail is evaluated after the outer join");
			*post = lappend(*post, q);
		}
		rest = NIL;
	}
	*quals = rest;
	return true;
}

/* the two columns of `a = b` (the same type and collation, no cast); false for any other conjunct */
static bool
column_equality(Node *q, Var **a, Var **b, Oid *opno)
{
	OpExpr	   *op;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	*a = (Var *) strip_relabel(linitial(op->args));
	*b = (Var *) strip_relabel(lsecond(op->args));
	*opno = op->opno;
	return IsA(*a, Var) && IsA(*b, Var) && (*a)->varlevelsup == 0 && (*b)->varlevelsup == 0 && (*a)->varnullingrels == NULL &&
		(*b)->varnullingrels == NULL && (*a)->vartype == (*b)->vartype && (*a)->varcollid == (*b)->varcollid && op_mergejoinable(op->opno, (*a)->vartype);
}

/* the column of the scanned table that inner lookup column v equals in one of the conjuncts (NULL: none) */
static Var *
scanned_key(AggCtx *c, List *quals, Var *v, Oid opno)
{
	foreach_ptr(Node, q, quals)
	{
		Var		   *a,
				   *b;
		Oid			o;

		if (!column_equality(q, &a, &b, &o) || o != opno)
			continue;
		if (a->varno == c->relid && b->varno == v->varno && b->varattno == v->varattno)
			return a;
		if (b->varno == c->relid && a->varno == v->varno && a->varattno == v->varattno)
			return b;
	}
	return NULL;
}

/*
 * `scanned.k = A.id AND A.id = B.id` (A and B inner lookups) holds the same rows as `scanned.k = A.id AND scanned.k = B.id`: B is then probed with a
 * column of the scanned row, where its chain through A needs A resolved first. The keys of B become a conjunct of the scan (join_filters), as
 * when B is joined to the table itself: the units they miss are not read.
 */
static List *
rekey_chains(AggCtx *c, List *quals)
{
	List	   *out = list_copy(quals);
	bool		changed = true;

	while (changed)
	{
		changed = false;
		for (int i = 0; i < list_length(out); i++)
		{
			Var		   *a,
					   *b,
					   *key;
			Oid			opno;
			JoinCtx    *ja,
					   *jb;
			OpExpr	   *op;

			if (!column_equality(list_nth(out, i), &a, &b, &opno) || (ja = join_of_var(c, (Node *) a)) == NULL ||
				(jb = join_of_var(c, (Node *) b)) == NULL || ja->kind != SJ_INNER || jb->kind != SJ_INNER)
				continue;
			if (scanned_key(c, out, a, opno) == NULL)
			{
				Var		   *t = a;

				a = b;
				b = t;
			}
			if (scanned_key(c, out, b, opno) != NULL || (key = scanned_key(c, out, a, opno)) == NULL)
				continue;
			op = copyObject((OpExpr *) list_nth(out, i));
			linitial(op->args) = copyObject(key);
			lsecond(op->args) = copyObject(b);
			list_nth_cell(out, i)->ptr_value = op;
			changed = true;
		}
	}
	return out;
}

/* the equality of the elements x and y of two jsonb arrays, as a comparison of their type (NULL: the types differ, or have no equality) */
static Node *
element_equality(Node *x, Node *y)
{
	TypeCacheEntry *tc;

	if (exprType(x) != exprType(y) || exprCollation(x) != exprCollation(y))
		return NULL;
	tc = lookup_type_cache(exprType(x), TYPECACHE_EQ_OPR);
	if (!OidIsValid(tc->eq_opr))
		return NULL;
	return (Node *) make_opclause(tc->eq_opr, BOOLOID, false, (Expr *) x, (Expr *) y, InvalidOid, exprCollation(x));
}

/* `jsonb_build_array(x) = jsonb_build_array(y)`, in the form of the equality op of two arrays */
static Node *
element_array_equality(OpExpr *op, FuncExpr *l, FuncExpr *r, Node *x, Node *y)
{
	OpExpr	   *e = makeNode(OpExpr);
	FuncExpr   *a = makeNode(FuncExpr),
			   *b = makeNode(FuncExpr);

	*e = *op;
	*a = *l;
	*b = *r;
	a->args = list_make1(x);
	b->args = list_make1(y);
	e->args = list_make2(a, b);
	return (Node *) e;
}

/*
 * `jsonb_build_array(a1, ..) = jsonb_build_array(b1, ..)` holds when each pair of elements is equal, a NULL (the JSON null) being equal
 * to itself only. An element of the same type on both sides whose JSON image is its value (an integer, a date, a text of a deterministic
 * collation) that equals a column of the joined table is a key that equals NULLs; each of the others stays a condition of the table's rows.
 * False, and the clause as it is, when no element is a key. Without keys the rows of the table are all tested for every tuple.
 */
static bool
array_keys(AggCtx *c, JoinCtx *jc, Node *q, List **conds)
{
	OpExpr	   *op = (OpExpr *) q;
	FuncExpr   *l,
			   *r;
	List	   *rest = NIL;
	int			nkey = jc->j.nkey;
	ListCell   *la,
			   *lb;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2 || get_opcode(op->opno) != F_JSONB_EQ)
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	if (!IsA(l, FuncExpr) || !IsA(r, FuncExpr) || l->funcid != F_JSONB_BUILD_ARRAY_ANY || r->funcid != F_JSONB_BUILD_ARRAY_ANY ||
		l->funcvariadic || r->funcvariadic || list_length(l->args) != list_length(r->args))
		return false;
	forboth(la, l->args, lb, r->args)
	{
		Node	   *x = lfirst(la),
				   *y = lfirst(lb),
				   *eq = element_equality(x, y);

		if (eq == NULL || (!join_clause(c, jc, eq, true) && !join_expr_clause(c, jc, eq, true)))
			rest = lappend(rest, element_array_equality(op, l, r, x, y));
	}
	if (jc->j.nkey == nkey)
		return false;
	*conds = list_concat(*conds, rest);
	return true;
}

bool
setup_joins(AggCtx *c, List **quals, List **post, List **scan_quals, List **scan_post)
{
	List	   *probe_quals;

	*quals = drop_pushed(c, *quals);
	*scan_quals = drop_pushed(c, *scan_quals);
	*quals = rekey_chains(c, *quals);
	*scan_quals = rekey_chains(c, *scan_quals);
	foreach_ptr(JoinCtx, jc, c->joins)
		jc->clauses = drop_pushed(c, jc->clauses);
	probe_quals = preserved_join(c) ? list_copy(*scan_quals) : list_concat_copy(*quals, *scan_quals);
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			probe_quals = list_concat(probe_quals, list_copy(jc->clauses));
	if (!order_lookups(c, probe_quals))
		return agg_refuse(JOIN_CONDITION);
	if (!join_quals(c, quals, post, false) || !join_quals(c, scan_quals, scan_post, true))
		return agg_refuse(WHERE);
	/* an EXISTS in the ON clause of an inner join filters its tuples, as in the WHERE (inside the nullable input of a preserved join, if there is one) */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			foreach_ptr(Node, q, jc->clauses)
				if (IsA(q, SubPlan))
				{
					List	  **dest = preserved_join(c) ? scan_post : post;

					*dest = lappend(*dest, q);
					jc->clauses = foreach_delete_current(jc->clauses, q);
				}
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		List	   *conds = NIL;
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		foreach_ptr(Node, arg, jc->args)
		{
			Node       *expr = normalize_expr(c, arg);
			ComboWalk   w = {c};
			Bitmapset  *casts = bms_copy(c->jcast);

			combo_walker(expr, &w);
			if (member_arg(c, arg))		/* the child gets the JSON null as it is: the cast it makes of it raises where SQL does */
				c->jcast = casts;
			if (w.bad || (w.jc && w.jc->combo >= jc->combo))
				return agg_refuse_on(JOIN_CONDITION, "a join key expression reads a table joined after it, or a leaf the scan cannot evaluate");
			for (int p = 0; p < w.nl; p++)
			{
				if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
					return agg_refuse_limit(SHAPE_WIDTH, combo_leaves, SIFT_AGG_MAX_CLEAVES, "no room in a value combination for a leaf of the key expression (it holds at most that many, or has the leaf with another type)");
				use_matched_leaf(c, w.leaf[p]);
			}
			foreach_ptr(Var, v, w.vars)
				if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
					return agg_refuse_on(JOIN_CONDITION, "a key expression reads a column of another joined table that cannot be looked up first");
			jc->j.args = lappend(jc->j.args, expr);
		}
		foreach_ptr(Node, q, jc->clauses)
			if (jc->j.params != NIL || (!array_keys(c, jc, q, &conds) && !join_clause(c, jc, q, false) && !join_expr_clause(c, jc, q, false)))
				conds = lappend(conds, q);
		if ((jc->j.nkey == 0 && conds == NIL && jc->j.params == NIL) || (conds != NIL && !join_filter(c, jc, conds)))
			return agg_refuse_on(JOIN_CONDITION, "a join with no equality between its columns and the scanned table's leaves (a range, an inequality or ON true)");
	}
	return true;
}

/* the position (1..) of column att in the output of a joined plan */
static int
child_pos(List *vars, int att)
{
	foreach_ptr(Var, v, vars)
		if (v->varattno == att)
			return foreach_current_index(v) + 1;
	SIFT_INVARIANT(false, "sift: column %d is not an output of the joined plan", att);
}

#define AGG_FORCE_SCALE	1e-6	/* sift.agg_force: the price of a path against the normal plan's */

/* A path with the given number of workers: the spec travels with it, and the cheaper of the paths wins */
void
add_agg_path(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist, int nworkers, List *pathkeys)
{
	SiftAggPath *ap = (SiftAggPath *) newNode(sizeof(SiftAggPath), T_CustomPath);
	CustomPath *cp = &ap->path;

	s->nworkers = nworkers;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, s->est_groups);
	cp->path.pathkeys = pathkeys;
	ap->parent_pathkeys = c->parent_pathkeys;
	ap->parent_rti = c->parent_rti;
	cp->path.startup_cost += agg_order_cost(c, s, cp->path.rows);
	cp->path.total_cost += agg_order_cost(c, s, cp->path.rows);
	/* the groups after the n-th of the sort key are not emitted (a tie keeps more): the sort above sees about n of them */
	if (s->gtopn_k > 0 && cp->path.rows > 4.0 * s->gtopn_k)
		cp->path.rows = 2.0 * s->gtopn_k;
	cp->custom_private = list_make2(agg_spec_keep(s), scan_tlist);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		/* a path of c->root cannot be planned as a child of the node, which PostgreSQL does with the root of the statement: it is a sub-plan of its own level */
		bool		own_level = jc->cpath != NULL && jc->j.params == NIL && c->detached;

		ap->correlated = lappend(ap->correlated, jc->j.params != NIL ? ((SubqueryScanPath *) jc->cpath)->subpath : own_level ? jc->cpath : NULL);
		ap->subroots = lappend(ap->subroots, own_level ? c->root : jc->subroot);
		if (jc->cpath != NULL && jc->j.params == NIL && !own_level)
			cp->custom_paths = lappend(cp->custom_paths, jc->cpath);
	}
	if (c->spine != NULL)
		cp->custom_paths = lappend(cp->custom_paths, c->spine);
	agg_arm_path(&cp->path);
	/* free, but in the order of their prices: the paths with workers and the ones without still compete */
	if (sift_agg_force)
	{
		cp->path.startup_cost *= AGG_FORCE_SCALE;
		cp->path.total_cost *= AGG_FORCE_SCALE;
	}
	add_path(output_rel, &cp->path);
}

/*
 * The plans the joins read, as paths that output the columns the spec reads (the columns of the joined rows are positions of that output): the
 * path PostgreSQL chose for the rows of the sub-select, VALUES list or function, projected on them. The scan nodes project any column.
 */
/* every column of a CTE, VALUES list or sub-select, in order */
static List *
child_all_vars(AggCtx *c, JoinCtx *jc)
{
	RangeTblEntry *e = rt_fetch(jc->rti, c->root->parse->rtable);
	List	   *vars = NIL;

	if (e->rtekind == RTE_SUBQUERY)
	{
		foreach_node(TargetEntry, tle, e->subquery->targetList)
			if (!tle->resjunk)
				vars = lappend(vars, makeVar(jc->rti, tle->resno, exprType((Node *) tle->expr), exprTypmod((Node *) tle->expr), exprCollation((Node *) tle->expr), 0));
		return vars;
	}
	for (int i = 0; i < list_length(e->coltypes); i++)
		vars = lappend(vars, makeVar(jc->rti, i + 1, list_nth_oid(e->coltypes, i), list_nth_int(e->coltypmods, i),
									 list_nth_oid(e->colcollations, i), 0));
	return vars;
}

bool
child_paths(AggCtx *c, SiftAggSpec *s)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		SiftAggJoin *j = &s->join[foreach_current_index(jc)];
		PathTarget *t = create_empty_pathtarget();
		Path	   *p;
		Size		size;

		if (jc->cpath == NULL || jc->j.scoped)
			continue;
		if (!child_cols_kept(c, jc))
			return agg_refuse_on(JOIN_CHILD, "a column of the child that the query reads is not one the scan can pass on");
		if (jc->j.params != NIL)
			continue;
		/* the plan of a child with an identity outputs all its columns: the arms that share it number them alike */
		if (jc->j.sig != NULL)
			jc->cvars = child_all_vars(c, jc);
		/* the child's own columns, before any outer join nulls them: the plan below the projection outputs them so */
		foreach_ptr(Var, v, jc->cvars)
		{
			Var		   *own = copyObject(v);

			/* the plan of a tree of joins outputs the columns as the join above its outer joins sees them */
			if (jc->tree)
			{
				own = NULL;
				foreach_ptr(Expr, x, jc->cpath->pathtarget->exprs)
					if (IsA(x, Var) && ((Var *) x)->varno == v->varno && ((Var *) x)->varattno == v->varattno)
						own = copyObject((Var *) x);
				if (own == NULL)
					return agg_refuse_on(JOIN_CHILD, "a column of the keys' join is not an output of its plan");
			}
			else
				own->varnullingrels = NULL;
			add_column_to_pathtarget(t, (Expr *) own, 0);
		}
		set_pathtarget_cost_width(c->root, t);
		for (int k = 0; k < j->nkey; k++)
			j->keyatt[k] = child_pos(jc->cvars, j->keyatt[k]);
		for (int k = 0; k < j->ncol; k++)
			j->colatt[k] = child_pos(jc->cvars, j->colatt[k]);
		if (!projects_columns(jc->cpath))
		{
			jc->cpath = (Path *) create_projection_path(c->root, c->root->simple_rel_array[jc->rti], jc->cpath, t);
			continue;
		}
		size = IsA(jc->cpath, SubqueryScanPath) ? sizeof(SubqueryScanPath) : sizeof(Path);
		p = memcpy(palloc(size), jc->cpath, size);
		p->pathtarget = t;
		jc->cpath = p;
	}
	return true;
}
