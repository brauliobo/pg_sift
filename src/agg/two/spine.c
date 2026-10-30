/*
 * agg/two/spine.c - the keys and the spine of a two level aggregate
 * The key each arm groups by, whether the arms agree, and the FROM and WHERE of the
 * spine that lists the groups.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/two/int.h"


/* a column every arm has as NULL is a key no arm groups by: no group has an id for it, and the scan emits NULL */
static void
null_key(SortGroupClause *sgc, Node *e, SiftAggKey *k)
{
	/* the key has no program: no arm selects it, so it is never evaluated */
	k->kind = SAK_NUM;
	k->leaf = -1;
	k->typid = exprType(e);
	k->typmod = exprTypmod(e);
	k->collid = exprCollation(e);
	k->eqop = sgc->eqop;
	k->desc = "NULL";
}

/* the spine's Var: a column of the spine at the query's own level */
static bool
spine_var(const TwoLevel *t, Node *n)
{
	return t->join != NULL && IsA(n, Var) && ((Var *) n)->varno == t->spine_rti && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0;
}

/* the column of the spine's plan that spine Var v is */
static int
spine_col(TwoLevel *t, Var *v)
{
	foreach_ptr(Var, x, t->spine_cols)
		if (x->varattno == v->varattno)
			return foreach_current_index(x);
	t->spine_cols = lappend(t->spine_cols, makeVar(t->spine_rti, v->varattno, v->vartype, v->vartypmod, v->varcollid, 0));
	return list_length(t->spine_cols) - 1;
}

/* a group column of the spine is a key no arm computes: the ids are those of the spine's values */
static bool
spine_key(TwoLevel *t, SortGroupClause *sgc, Var *v, SiftAggKey *k)
{
	SiftAggSpine *sp = t->spine;
	RegProcedure lhs,
				rhs;

	if (sp->nout >= SIFT_AGG_MAX_KEYS || !get_op_hash_functions(sgc->eqop, &lhs, &rhs))
		return false;
	sp->outcol[sp->nout] = spine_col(t, v);
	sp->outkey[sp->nout++] = t->s->nkeys;
	k->kind = SAK_SPINE;
	k->leaf = -1;
	k->typid = v->vartype;
	k->typmod = v->vartypmod;
	k->collid = v->varcollid;
	k->eqop = sgc->eqop;
	k->desc = psprintf("spine.%s", get_rte_attribute_name(rt_fetch(t->spine_rti, t->root->parse->rtable), v->varattno));
	return true;
}

/* x with what each arm's own planning gave its sub-plans (the number, the name, the costs, the Param ids) left out; the sub-selects are collected to be compared */
static Node *
sub_shape(Node *n, SubShape *w)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, SubPlan))
	{
		SubPlan    *sp = makeNode(SubPlan);
		List	   *outer = w->params;

		*sp = *(SubPlan *) n;
		if (sp->plan_id < 1 || sp->plan_id > list_length(w->glob->subroots))
			w->bad = true;
		else
			w->subs = lappend(w->subs, ((PlannerInfo *) list_nth(w->glob->subroots, sp->plan_id - 1))->parse);
		w->params = sp->paramIds;
		sp->testexpr = sub_shape(sp->testexpr, w);
		w->params = outer;
		sp->plan_id = 0;
		sp->plan_name = NULL;
		sp->paramIds = NIL;
		sp->startup_cost = sp->per_call_cost = 0;
		return (Node *) sp;
	}
	n = expression_tree_mutator(n, sub_shape, w);
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		foreach_int(id, w->params)
			if (id == ((Param *) n)->paramid)
				((Param *) n)->paramid = foreach_current_index(id);
	return n;
}

/*
 * Whether two arms group by the same expression. Each arm plans a sub-select of the expression (x IN (SELECT ..)) as a sub-plan of its own, so the
 * expressions are equal when they differ in nothing but what that planning numbered, and the sub-selects themselves are equal.
 */
static bool
same_key(const TwoLevel *t, Node *a, Node *b)
{
	SubShape	wa = {t->root->glob, NIL, NIL, false},
				wb = wa;

	return equal(a, b) ||
		(contain_subplans(a) && equal(sub_shape(a, &wa), sub_shape(b, &wb)) && !wa.bad && !wb.bad && equal(wa.subs, wb.subs));
}

/* a group column of the arms: every arm groups by the same expression or has NULL */
static bool
arm_key(TwoLevel *t, SortGroupClause *sgc, Node *e, SiftAggKey *key)
{
	SiftAggSpec *s = t->s;
	Node	   *def = NULL;
	int			col;

	/* a bucket column of the spine that is NULL, which the planner folded into the query */
	if (t->join != NULL && null_const(e))
	{
		null_key(sgc, e, key);
		return true;
	}
	if (!IsA(e, Var) || ((Var *) e)->varno != t->srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > t->ncols)
		return agg_refuse_on(TWO_KEYS, "a group key is not a column of the arms");
	col = ((Var *) e)->varattno - 1;
	for (int k = 0; k < t->narms; k++)
	{
		Node	   *x = list_nth(t->arms[k].cols, col);

		if (null_const(x))
			continue;
		if (!list_member(t->arms[k].group, x) || equal(x, t->arms[k].id) || (def != NULL && !same_key(t, x, def)))
			return agg_refuse_f(TWO_KEYS, "output column %d of arm %d is not an expression every arm groups by (or NULL)%s", col + 1, k + 1,
								contain_subplans(x) ? ": it holds a sub-select that is not the same in every arm" : "");
		def = x;
		t->arms[k].used = lappend(t->arms[k].used, x);
		t->arms[k].kexpr = lappend(t->arms[k].kexpr, x);
		t->arms[k].kidx = lappend_int(t->arms[k].kidx, s->nkeys);
		t->arms[k].out->keymask |= (uint32) 1 << s->nkeys;
	}
	if (def == NULL)
	{
		null_key(sgc, e, key);
		return true;
	}
	return compile_key(&t->ctx, sgc, def, key) || agg_refuse_f(TWO_KEYS, "the key of output column %d is not one the scan reads", col + 1);
}

/* the outer keys: columns that every arm groups by or has as NULL, and the spine's */
bool
two_keys(TwoLevel *t)
{
	AggCtx	   *c = &t->ctx;
	SiftAggSpec *s = t->s;
	Query	   *parse = t->root->parse;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	for (int k = 0; k < t->narms; k++)
		nkeyslots += list_length(t->arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, t->root->processed_tlist),
				   *x = strip_ph(e);

		if (!(spine_var(t, x) ? spine_key(t, sgc, (Var *) x, &s->key[s->nkeys]) : arm_key(t, sgc, x, &s->key[s->nkeys])))
			return false;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		t->ow.keys = lappend(t->ow.keys, e);
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry((Expr *) e, list_length(t->scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, t->arms, t->narms, &t->scan_tlist, parse->groupingSets != NIL))
		return false;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &t->scan_tlist);
	return true;
}

/* the outer aggregates: those of the target list and of HAVING */
bool
two_aggs(TwoLevel *t, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	SiftAggSpec *s = t->s;

	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &t->ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &t->ow);
	if (t->ow.bad)
		return false;
	s->agg = palloc0(Max(list_length(t->ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, t->ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(t->root->parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(&t->ctx, s, (Aggref *) a, t->srti, t->arms, t->narms, &s->agg[s->naggs])))
			return IsA(a, Aggref) ? agg_refuse_f(TWO_AGGS, "%s(%s) over the arms%s is not one the two level scan computes", get_func_name(((Aggref *) a)->aggfnoid),
												  ((Aggref *) a)->aggdistinct ? "DISTINCT .." : "..", ((Aggref *) a)->aggfilter ? " with a FILTER" : "") : false;
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry((Expr *) a, list_length(t->scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	return true;
}

/* the spine's range table entry: a table, or a sub-select over one with a WHERE that PostgreSQL pulled up (its rows are the rel's filtered ones); 0 if it is not */
static Index
spine_item(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? ((RangeTblRef *) n)->rtindex : 0;
}

/* a column of the arms that is NULL in every one of them (a column of a rollup whose arms have none) tested IS NULL: true of each row, matched or not */
static bool
null_in_arms(Node *q, Index arm, List *arms)
{
	NullTest   *nt;
	Node	   *arg;
	Var		   *v;

	if (!IsA(q, NullTest))
		return false;
	nt = (NullTest *) q;
	arg = (Node *) nt->arg;
	/* the column of a sub-select that was pulled up arrives wrapped, to be NULL above the join that makes it so */
	while (arg != NULL && IsA(arg, PlaceHolderVar))
		arg = (Node *) ((PlaceHolderVar *) arg)->phexpr;
	if (nt->nulltesttype != IS_NULL || nt->argisrow || arg == NULL || !IsA(arg, Var))
		return false;
	v = (Var *) arg;
	if (v->varno != arm || v->varlevelsup != 0)
		return false;
	foreach_ptr(PlannerInfo, ar, arms)
	{
		TargetEntry *te = get_tle_by_resno(ar->parse->targetList, v->varattno);

		if (te == NULL || !IsA(te->expr, Const) || !((Const *) te->expr)->constisnull)
			return false;
	}
	return true;
}

/*
 * The WHERE of the query reads the spine only (PostgreSQL moved the WHERE of a spine sub-select up): the rows of the spine are the filtered ones. A test of
 * a column that is NULL in every arm filters nothing and is no condition.
 */
static bool
spine_where(PlannerInfo *root, Index spine, Index arm, List *arms)
{
	foreach_ptr(Node, q, (List *) root->parse->jointree->quals)
		if (!bms_equal(pull_varnos(root, q), bms_make_singleton(spine)) && !null_in_arms(q, arm, arms))
			return false;
	return true;
}

/*
 * The FROM item is the arms, or `spine JOIN arms ON equalities` (either order) or `spine LEFT JOIN arms ON equalities`; the WHERE would
 * filter the joined rows, so it reads the spine only. A subquery PostgreSQL did not pull up and a plan of a few rows are what the spine can be.
 */
bool
two_from(TwoLevel *t)
{
	Query	   *parse = t->root->parse;
	Node	   *from = linitial(parse->jointree->fromlist);
	JoinExpr   *j;
	List	   *gates = NIL;

	if (list_length(parse->jointree->fromlist) != 1)
		return false;
	if (IsA(from, RangeTblRef))
	{
		t->srti = ((RangeTblRef *) from)->rtindex;
		return true;
	}
	while (IsA(from, FromExpr) && list_length(((FromExpr *) from)->fromlist) == 1 && ((FromExpr *) from)->quals == NULL)
		from = linitial(((FromExpr *) from)->fromlist);
	if (!IsA(from, JoinExpr))
		return false;
	j = (JoinExpr *) from;
	if (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT)
		return false;
	/* the preserved side of a LEFT JOIN is the left one: the spine */
	for (int side = 0; side < (j->jointype == JOIN_LEFT ? 1 : 2); side++)
	{
		Node	   *a = side == 0 ? j->rarg : j->larg,
				   *b = side == 0 ? j->larg : j->rarg;
		Index		spine = spine_item(b);
		List	   *arms = NIL;

		if (IsA(a, RangeTblRef) && spine != 0 && rt_fetch(((RangeTblRef *) a)->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
			arms = outer_arms(t->root, ((RangeTblRef *) a)->rtindex, &gates);
		if (arms != NIL && spine_where(t->root, spine, ((RangeTblRef *) a)->rtindex, arms))
		{
			t->join = j;
			t->srti = ((RangeTblRef *) a)->rtindex;
			t->spine_rti = spine;
			t->spine = palloc0(sizeof(SiftAggSpine));
			t->spine->left = j->jointype == JOIN_LEFT;
			return true;
		}
	}
	return false;
}

static bool
arm_var(const TwoLevel *t, Node *n)
{
	return IsA(n, Var) && ((Var *) n)->varno == t->srti && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0 &&
		((Var *) n)->varattno <= t->ncols;
}

/* the types whose equality is that of their JSON images, which the jsonb arrays of a join compare */
static bool
spine_type(Oid typid, Oid collid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		case TEXTOID:
		case VARCHAROID:
			return !OidIsValid(collid) || get_collation_isdeterministic(collid);
		default:
			return false;
	}
}

/* `arm column = spine column or constant` (either order; opno: the equality operator, InvalidOid for the elements of jsonb arrays): a pair of the join */
static bool
spine_eq(TwoLevel *t, Oid opno, Node *a, Node *b, bool nullsafe)
{
	struct SiftAggSpinePair *q;
	Node	   *x = strip_relabel(strip_ph(a)),
			   *y = strip_relabel(strip_ph(b));
	TypeCacheEntry *tc;

	if (!arm_var(t, x))
	{
		Node	   *z = x;

		x = y;
		y = z;
	}
	if (!arm_var(t, x) || !(spine_var(t, y) || IsA(y, Const)) || t->spine->npair >= SIFT_AGG_MAX_JKEYS ||
		exprType(x) != exprType(y) || exprCollation(x) != exprCollation(y) || !spine_type(exprType(x), exprCollation(x)))
		return false;
	tc = lookup_type_cache(exprType(x), TYPECACHE_EQ_OPR);
	if (!OidIsValid(tc->eq_opr) || (OidIsValid(opno) && opno != tc->eq_opr))
		return false;
	q = &t->spine->pair[t->spine->npair];
	q->col = IsA(y, Var) ? spine_col(t, (Var *) y) : -1;
	q->konst = IsA(y, Const) ? y : NULL;
	q->nullsafe = nullsafe;
	q->typid = exprType(x);
	q->collid = exprCollation(x);
	q->eqop = tc->eq_opr;
	t->pair_att[t->spine->npair++] = ((Var *) x)->varattno;
	return true;
}

/* a conjunct of the join: an equality, `IS NOT DISTINCT FROM`, or the equality of two jsonb arrays (each element null-safe) */
static bool
spine_conjunct(TwoLevel *t, Node *q)
{
	if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) q;
		Node	   *l = linitial(op->args),
				   *r = lsecond(op->args);
		ListCell   *la,
				   *lb;

		if (get_opcode(op->opno) != F_JSONB_EQ)
			return spine_eq(t, op->opno, l, r, false);
		if (!IsA(l, FuncExpr) || !IsA(r, FuncExpr) || ((FuncExpr *) l)->funcid != F_JSONB_BUILD_ARRAY_ANY ||
			((FuncExpr *) r)->funcid != F_JSONB_BUILD_ARRAY_ANY || list_length(((FuncExpr *) l)->args) != list_length(((FuncExpr *) r)->args))
			return false;
		forboth(la, ((FuncExpr *) l)->args, lb, ((FuncExpr *) r)->args)
			if (!spine_eq(t, InvalidOid, lfirst(la), lfirst(lb), true))
				return false;
		return true;
	}
	if (IsA(q, BoolExpr) && ((BoolExpr *) q)->boolop == NOT_EXPR && IsA(linitial(((BoolExpr *) q)->args), DistinctExpr))
	{
		DistinctExpr *d = linitial_node(DistinctExpr, ((BoolExpr *) q)->args);

		return list_length(d->args) == 2 && spine_eq(t, d->opno, linitial(d->args), lsecond(d->args), true);
	}
	return false;
}

/* the spine: the pairs of the join resolved to the keys of each arm, and the plan of its rows (the one child of the path) */
bool
two_spine(TwoLevel *t)
{
	SiftAggSpine *sp = t->spine;
	PlannerInfo *root = t->root;
	RelOptInfo *rel = root->simple_rel_array[t->spine_rti];
	RangeTblEntry *e = rt_fetch(t->spine_rti, root->parse->rtable);
	AggCtx		outer = {.root = root};
	PathTarget *target = create_empty_pathtarget();
	Path	   *p;

	foreach_ptr(Node, q, t->join->quals == NULL ? NIL : IsA(t->join->quals, List) ? (List *) t->join->quals : list_make1(t->join->quals))
		if (!spine_conjunct(t, q))
			return agg_refuse_on(SPINE_SHAPE, "a condition of the join is no equality of an arm column and a spine column or constant");
	if (sp->npair == 0)
		return agg_refuse_on(SPINE_SHAPE, "the join has no condition");
	for (int i = 0; i < sp->npair; i++)
		for (int k = 0; k < t->narms; k++)
		{
			Node	   *x = list_nth(t->arms[k].cols, t->pair_att[i] - 1);

			sp->pair[i].key[k] = null_const(x) ? -1 : arm_key_of(&t->arms[k], x);
			if (sp->pair[i].key[k] < 0 && !null_const(x))
				return agg_refuse_f(SPINE_SHAPE, "the join column %d of arm %d is not a key the arm groups by", i + 1, k + 1);
		}
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || !bms_is_empty(rel->lateral_relids) || rel->cheapest_total_path == NULL ||
		t->spine_cols == NIL)
		return agg_refuse_on(SPINE_SHAPE, "the spine is not a relation the scan can read once (lateral, or no column of it is read)");
	/* the rows are read once, when the scan starts: a condition of the spine that changes between rescans cannot be */
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		if (contain_volatile_functions((Node *) rinfo->clause) || has_exec_param_walker((Node *) rinfo->clause, NULL))
			return agg_refuse_on(SPINE_SHAPE, "a condition of the spine is volatile or changes between rescans");
	switch (e->rtekind)
	{
		case RTE_RELATION:
			if (e->relkind != RELKIND_RELATION)
				return false;
			break;
		case RTE_CTE:
			{
				PlannerInfo *owner = root;

				for (Index up = e->ctelevelsup; up > 0; up--)
					owner = owner->parent_root;
				if (e->self_reference || cte_owner_correlated(owner))
					return agg_refuse_on(SPINE_SHAPE, "the spine is a CTE that changes between rescans");
			}
			break;
		case RTE_VALUES:
		case RTE_FUNCTION:
		case RTE_SUBQUERY:
			if (reads_outer(&outer, e, 1) || contain_volatile_functions(e->rtekind == RTE_VALUES ? (Node *) e->values_lists :
																		 e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery))
				return agg_refuse_on(SPINE_SHAPE, "the spine reads the query's other tables or calls a volatile function");
			break;
		default:
			return agg_refuse_on(SPINE_SHAPE, "the spine is not a table, VALUES list, function, sub-select or CTE");
	}
	foreach_ptr(Var, v, t->spine_cols)
		add_column_to_pathtarget(target, (Expr *) v, 0);
	set_pathtarget_cost_width(root, target);
	p = (Path *) create_projection_path(root, rel, rel->cheapest_total_path, target);
	if (!agg_lookup_fits(p->rows, p->pathtarget->width))
		return agg_refuse_limit(SPINE_ROWS, lookup_kb, sift_agg_lookup_max_kb, "the spine is estimated at %.0f rows of width %d", p->rows, p->pathtarget->width);
	t->ctx.spine = p;
	sp->child = 0;
	sp->ncol = list_length(t->spine_cols);
	return true;
}

/* the join PostgreSQL made of the tables of the arms' FROM, the input of the arms' own aggregates */
RelOptInfo *
arm_input(PlannerInfo *base)
{
	Relids		all = base->all_query_rels;

	return bms_membership(all) == BMS_SINGLETON ? base->simple_rel_array[bms_singleton_member(all)] : find_join_rel(base, all);
}
