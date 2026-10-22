/*
 * agg/plan_exists.c - existential predicates: any element, EXISTS, the dispatch to jsonb_path_exists (plan_path.c), @> and ?| (plan_contains.c),
 * and dependency recording
 */
#include "postgres.h"

#include "catalog/pg_opfamily.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/plan_exists_int.h"

/* ---------- existential predicates: some element of one array passes (SAP_ANY_ELEM) ---------- */

/*
 * The element predicate is compiled with the array in scope as the query's group (so its leaves are element leaves of that array,
 * not a second unnest) and with its own set of leaves; whatever it changes but the counters is undone.
 */
static AggCtx
scope_enter(AggCtx *c, int grp)
{
	AggCtx		saved = *c;

	c->group = grp;
	c->used = NULL;
	c->func_key = NULL;
	return saved;
}

static void
scope_leave(AggCtx *c, const AggCtx *saved, int grp)
{
	c->elem_used = bms_join(c->elem_used, c->used);
	c->used = bms_add_member(saved->used, grp);
	c->group = saved->group;
	c->func_rti = saved->func_rti;
	c->func = saved->func;
	c->func_kind = saved->func_kind;
	c->func_key = saved->func_key;
	c->rtable = saved->rtable;
	c->dpctx = saved->dpctx;
	c->oparams = saved->oparams;
}

/* some element of array leaf grp passes the predicate that build compiles over the leaves of its elements */
SiftAggPred *
any_elem(AggCtx *c, int grp, ElemBuild build, void *arg)
{
	SiftAggPred *p = new_pred(c, SAP_ANY_ELEM),
			   *e;
	int			n0 = c->npred;
	AggCtx		saved = scope_enter(c, grp);

	e = build(c, grp, arg);
	scope_leave(c, &saved, grp);
	if (e == NULL)
		return NULL;
	p->leaf = grp;
	p->nchild = 1;
	p->child = palloc(sizeof(SiftAggPred *));
	p->child[0] = e;
	c->elem_work += (c->npred - n0) * elems_of(c, grp);
	p->desc = psprintf("EXISTS %s[] (%s)", leaf_name(c, grp), e->desc);
	return p;
}

static int
child_named(AggCtx *c, int group, const char *name, bool astext)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && l->astext == astext && l->kind == SCL_SCALAR && strcmp(l->name, name) == 0)
			return i;
	}
	return -1;
}

/* a string no number or boolean prints as: the text of an astext leaf equals it for a JSON string only */
static bool
lit_is_plain_text(const Lit *lit)
{
	return lit->kind == 's' && (lit->len == 0 || (!isdigit((unsigned char) lit->s[0]) && lit->s[0] != '-')) &&
		!(lit->len == 4 && memcmp(lit->s, "true", 4) == 0) && !(lit->len == 5 && memcmp(lit->s, "false", 5) == 0);
}

/* element leaf (JSON member `name`) op lit with the semantics of a jsonpath / containment comparison of JSON values, or NULL when not provable */
static SiftAggPred *
lit_cmp(AggCtx *c, int grp, const char *name, int op, const Lit *lit)
{
	int			leaf = child_named(c, grp, name, false);
	SiftAggPred *p;
	Const	   *k;
	Oid			typid;

	/* the text leaf of any scalar answers an equality with a string of its own kind: the comparison of another JSON type is unknown, not equal */
	if (leaf < 0 && op == SCMP_EQ && lit_is_plain_text(lit))
		leaf = child_named(c, grp, name, true);
	if (leaf < 0)
		return NULL;
	typid = leaf_of(c, leaf)->typid;
	if (lit->kind == 's' && typid == TEXTOID && (op == SCMP_EQ || op == SCMP_NE) && lut_leaf_ok(c, leaf))
	{
		/* bytewise equality: the C collation is deterministic whatever the database collation is */
		Param	   *pm = makeNode(Param);
		char	   *str = pnstrdup(lit->s, lit->len);
		Oid			eq = get_opfamily_member(TEXT_BTREE_FAM_OID, TEXTOID, TEXTOID, BTEqualStrategyNumber);
		OpExpr	   *e;

		pm->paramkind = PARAM_EXEC;
		pm->paramtype = TEXTOID;
		pm->paramtypmod = -1;
		pm->paramcollid = C_COLLATION_OID;
		pm->location = -1;
		k = makeConst(TEXTOID, -1, C_COLLATION_OID, -1, PointerGetDatum(cstring_to_text_with_len(lit->s, lit->len)), false, false);
		e = (OpExpr *) make_opclause(op == SCMP_NE ? get_negator(eq) : eq, BOOLOID, false, (Expr *) pm, (Expr *) k, InvalidOid, C_COLLATION_OID);
		set_opfuncid(e);
		p = make_leaf_pred(c, SAP_LUT, leaf);
		p->expr = (Node *) e;
		p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), quote_literal_cstr(str));
		return p;
	}
	/* jsonb numbers are numerics: only an integer leaf and an integral literal compare exactly (a float8 leaf holds the rounded value) */
	if (lit->kind == 'n' && (typid == INT2OID || typid == INT4OID || typid == INT8OID))
	{
		bool		err;
		int64		v = sift_numeric_int8(lit->num, &err);

		if (err || !DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(lit->num),
													 DirectFunctionCall1(int8_numeric, Int64GetDatum(v)))))
			return NULL;
		k = makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(v), false, FLOAT8PASSBYVAL);
	}
	else if (lit->kind == 'b' && typid == BOOLOID)
		k = makeConst(BOOLOID, -1, InvalidOid, 1, BoolGetDatum(lit->b), false, true);
	else
		return NULL;
	p = make_leaf_pred(c, SAP_CMP, leaf);
	p->op = op;
	p->expr = (Node *) k;
	p->argtype = k->consttype;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), expr_text(c, (Node *) k));
	return p;
}

static SiftAggPred *
or_of(AggCtx *c, SiftAggPred *a, SiftAggPred *b)
{
	SiftAggPred *p;

	if (a == NULL || b == NULL)
		return NULL;
	p = new_junction(c, SAP_OR, 2);
	p->child[0] = a;
	p->child[1] = b;
	junction_done(p);
	return p;
}

static bool
source_can_jnull(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_jnull(get_rel_relam(c->indexoid), true) : sift_colsrc_can_jnull(get_rel_relam(c->heapoid), false);
}

/* the NULL slots of an element leaf by what they are (SAP_NULL ops); a JSON null and a missing key need the kinds of the source */
static SiftAggPred *
null_pred(AggCtx *c, int leaf, int op)
{
	static const char *const what[] = {"IS NULL", "IS NOT NULL", "IS JSON NULL", "IS MISSING"};
	SiftAggPred *p;

	/* a segment that did not keep the kinds cannot tell them: the normal plan, which reads the documents */
	if (op >= SNULL_JSON && (!source_can_jnull(c) || c->stats.nokinds[leaf]))
		return NULL;
	p = make_leaf_pred(c, SAP_NULL, leaf);
	p->op = op;
	p->desc = psprintf("%s %s", leaf_name(c, leaf), what[op]);
	return p;
}

static int
negate_op(int op)
{
	static const int neg[] = {SCMP_NE, SCMP_EQ, SCMP_GE, SCMP_GT, SCMP_LE, SCMP_LT};

	return neg[op];
}

/*
 * `@.name op lit` as a jsonpath filter has it, or with neg its negation `!(@.name op lit)`, on the element leaf of the member. The leaf
 * holds the members of its type and sets the others aside (overflow, an error for a scan that reads the leaf), so what is left is a value
 * of the type of the literal, a JSON null or a missing key (both NULL slots): a comparison is false for a missing key and for a JSON null
 * (true for != alone), so the negation is true for both (false for a JSON null under != ). Never unknown, which `!` could not negate.
 */
SiftAggPred *
lit_test(AggCtx *c, int grp, const char *name, int op, const Lit *lit, bool neg)
{
	int			leaf = child_named(c, grp, name, false);

	if (leaf < 0)
		return !neg && op == SCMP_EQ ? lit_cmp(c, grp, name, op, lit) : NULL;
	if (lit->kind == 'z')
	{
		if (op != SCMP_EQ && op != SCMP_NE)
			return NULL;
		/* a JSON null equals only a JSON null; every other value, a missing key apart, differs from it */
		if (op == SCMP_EQ)
			return neg ? or_of(c, null_pred(c, leaf, 1), null_pred(c, leaf, SNULL_MISSING)) : null_pred(c, leaf, SNULL_JSON);
		return null_pred(c, leaf, neg ? 0 : 1);
	}
	if (!neg && op != SCMP_NE)
		return lit_cmp(c, grp, name, op, lit);
	if (!neg)
		return or_of(c, lit_cmp(c, grp, name, SCMP_NE, lit), null_pred(c, leaf, SNULL_JSON));
	if (op == SCMP_NE)
		return or_of(c, lit_cmp(c, grp, name, SCMP_EQ, lit), null_pred(c, leaf, SNULL_MISSING));
	return or_of(c, lit_cmp(c, grp, name, negate_op(op), lit), null_pred(c, leaf, 0));
}

/* ---- EXISTS (SELECT 1 FROM <unnest of the array> AS t(..) WHERE <element conjunction>) ---- */

typedef struct ExistsArg
{
	Query	   *q;
	RangeTblFunction *f;
	FuncKind	kind;
	Index		rti;			/* the function's range table entry: the sub-select PostgreSQL pulled up leaves its own entry before it */
	List	   *quals;			/* the conditions of the sub-query, those of the pulled-up sub-selects included */
	List	   *params;			/* OuterParam: the columns of the outer row the sub-query's parameters stand for */
} ExistsArg;

/* the one table of a FROM tree of nested single-item FROM lists (what pulling up a sub-select leaves), the conditions of the lists added to *quals */
static RangeTblRef *
exists_leaf(Node *n, List **quals)
{
	FromExpr   *f;

	if (IsA(n, RangeTblRef))
		return (RangeTblRef *) n;
	if (!IsA(n, FromExpr) || list_length((f = (FromExpr *) n)->fromlist) != 1)
		return NULL;
	if (f->quals != NULL)
		*quals = list_concat(*quals, list_copy(IsA(f->quals, List) ? (List *) f->quals : list_make1(f->quals)));
	return exists_leaf(linitial(f->fromlist), quals);
}

/* the query of a correlated EXISTS sub-plan, if it is a plain SELECT of tables and conditions */
static Query *
exists_subquery(AggCtx *c, SubPlan *sp)
{
	Query	   *q;

	if (sp->subLinkType != EXISTS_SUBLINK || sp->testexpr || sp->useHashTable || sp->setParam || sp->parParam == NIL ||
		list_length(sp->parParam) != list_length(sp->args) || sp->plan_id < 1 ||
		sp->plan_id > list_length(c->root->glob->subroots))
		return NULL;
	q = ((PlannerInfo *) list_nth(c->root->glob->subroots, sp->plan_id - 1))->parse;
	if (q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->groupClause || q->havingQual ||
		q->distinctClause || q->limitCount || q->limitOffset || q->setOperations || q->cteList || q->rowMarks)
		return NULL;
	return q;
}

/* the query of a correlated EXISTS sub-plan, if it is the plain shape: one unnesting function and a WHERE; *rti is the function's entry */
static Query *
exists_query(AggCtx *c, SubPlan *sp, Index *rti, List **quals)
{
	Query	   *q = exists_subquery(c, sp);
	RangeTblEntry *rte;
	RangeTblRef *ref;

	if (q == NULL || (ref = exists_leaf((Node *) q->jointree, quals)) == NULL)
		return NULL;
	*rti = ref->rtindex;
	rte = rt_fetch(*rti, q->rtable);
	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 || contain_volatile_functions((Node *) *quals))
		return NULL;
	return q;
}

/* the outer expressions the sub-plan's parameters stand for */
static Node *
outer_mutator(Node *n, SubPlan *sp)
{
	if (n && IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		ListCell   *lp,
				   *la;

		forboth(lp, sp->parParam, la, sp->args)
			if (lfirst_int(lp) == ((Param *) n)->paramid)
				return copyObject(lfirst(la));
	}
	return expression_tree_mutator(n, outer_mutator, sp);
}

/* the conjunction as one predicate */
static SiftAggPred *
compile_conj(AggCtx *c, List *quals)
{
	SiftAggPred *p;

	if (quals == NIL)
	{
		p = new_pred(c, SAP_TRUE);
		p->desc = pstrdup("true");
		return p;
	}
	return list_length(quals) == 1 ? compile_pred(c, linitial(quals)) : compile_junction(c, SAP_AND, quals);
}

static SiftAggPred *
build_exists(AggCtx *c, int grp, void *arg)
{
	ExistsArg  *x = arg;

	c->func_rti = x->rti;
	c->func = x->f;
	c->func_kind = x->kind;
	c->rtable = x->q->rtable;
	c->dpctx = NIL;
	c->oparams = x->params;
	return compile_conj(c, x->quals);
}

/* the parameters of the sub-plan whose outer expression is a leaf, and the leaf (compile_elem_row takes the ones of the row) */
static List *
outer_params(AggCtx *c, SubPlan *sp)
{
	List	   *out = NIL;
	ListCell   *lp,
			   *la;

	forboth(lp, sp->parParam, la, sp->args)
	{
		int			leaf = match_leaf(c, lfirst(la));
		OuterParam *o;

		if (leaf < 0)
			continue;
		o = palloc(sizeof(OuterParam));
		o->id = lfirst_int(lp);
		o->leaf = leaf;
		out = lappend(out, o);
	}
	return out;
}

static SiftAggPred *
compile_exists(AggCtx *c, SubPlan *sp)
{
	ExistsArg	x = {0};
	Query	   *q = exists_query(c, sp, &x.rti, &x.quals);
	SiftAggPred *p;
	int			grp;

	if (q == NULL)
		return NULL;
	x.q = q;
	x.f = linitial_node(RangeTblFunction, rt_fetch(x.rti, q->rtable)->functions);
	x.kind = func_kind_of(x.f);
	if (x.kind == FK_NONE)
		return NULL;
	x.params = outer_params(c, sp);
	grp = group_leaf_of(c, outer_mutator(linitial(((FuncExpr *) x.f->funcexpr)->args), sp), func_leaf_kind(x.kind));
	if (grp < 0 || (p = any_elem(c, grp, build_exists, &x)) == NULL)
		return NULL;
	/* the call reads every column of every element, whatever its conditions test, and raises for a value its type does not take */
	if (x.kind == FK_RECORDSET)
	{
		RecFunc    *rf = palloc(sizeof(RecFunc));

		rf->grp = grp;
		rf->f = x.f;
		c->rec_funcs = lappend(c->rec_funcs, rf);
		p->pass_poison = true;
	}
	return p;
}

/* ---- EXISTS over the elements with a lookup, or a column of a joined table: a test of the joined tuples (SAP_ELEM_EXISTS) ---- */

typedef struct ExistsScan
{
	Query	   *q;
	Index		frti;			/* the unnesting function */
	Index		lrti;			/* the sub-select left joined to it (LEFT JOIN LATERAL .. ON true), 0 none */
	List	   *quals;
} ExistsScan;

/* the function of the FROM, and the lateral sub-select left joined to it; the conditions of the lists are added to quals */
static bool
exists_scan(Node *n, ExistsScan *x)
{
	if (IsA(n, RangeTblRef))
	{
		Index		rti = ((RangeTblRef *) n)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, x->q->rtable);

		if (rte->rtekind == RTE_FUNCTION && x->frti == 0)
			x->frti = rti;
		else if (rte->rtekind == RTE_SUBQUERY && rte->lateral && x->frti != 0 && x->lrti == 0)
			x->lrti = rti;
		else
			return false;
		return true;
	}
	if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;

		if (list_length(f->fromlist) != 1)
			return false;
		if (f->quals != NULL)
			x->quals = list_concat(x->quals, list_copy(IsA(f->quals, List) ? (List *) f->quals : list_make1(f->quals)));
		return exists_scan(linitial(f->fromlist), x);
	}
	if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;
		Const	   *on = (Const *) j->quals;

		return j->jointype == JOIN_LEFT && x->frti == 0 && (on == NULL || (IsA(on, Const) && !on->constisnull && DatumGetBool(on->constvalue))) &&
			exists_scan(j->larg, x) && x->frti != 0 && exists_scan(j->rarg, x) && x->lrti != 0;
	}
	return false;
}

/* the scoped lookup join of the sub-select left joined to the elements: its child plan, and the combo that its parameters are the inputs of */
static JoinCtx *
scoped_join(AggCtx *c, ExistsScan *x, Index offset)
{
	RangeTblEntry *rte = rt_fetch(x->lrti, x->q->rtable);
	JoinCtx    *jc = palloc0(sizeof(JoinCtx));
	ComboCtx   *cb;
	List	   *args;

	jc->rti = x->lrti + offset;
	jc->kind = SJ_LEFT;
	jc->j.kind = SJ_LEFT;
	jc->j.child = -1;
	jc->j.fslot = -1;
	jc->j.scoped = true;
	jc->j.desc = psprintf("%s (left, per element)", rte->eref->aliasname);
	if (!scoped_child(c, rte->subquery, offset, jc, &args))
		return NULL;
	cb = new_combo(c, list_length(c->joins));
	jc->combo = combo_index(c, cb);
	c->joins = lappend(c->joins, jc);
	foreach_ptr(Node, arg, args)
	{
		Node	   *expr = normalize_expr(c, arg);
		ComboWalk	w = {c};

		combo_walker(expr, &w);
		if (w.bad || (w.jc && w.jc->combo >= jc->combo))
			return NULL;
		for (int p = 0; p < w.nl; p++)
		{
			if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
				return NULL;
			use_matched_leaf(c, w.leaf[p]);
		}
		foreach_ptr(Var, v, w.vars)
			if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
				return NULL;
		jc->j.args = lappend(jc->j.args, lower_scoped(c, jc->combo, expr));
	}
	return jc;
}

/* the conjuncts that read the leaves of the elements only, as predicates of the element level; the others are returned in *rest */
static SiftAggPred *
element_conjuncts(AggCtx *c, List *quals, List **rest)
{
	List	   *preds = NIL;
	SiftAggPred *p;

	foreach_ptr(Node, q, quals)
	{
		SiftAggPred *e = NULL;

		if (bms_is_subset(pull_varnos(c->root, q), bms_make_singleton(c->func_rti)))
			e = compile_pred(c, normalize_expr(c, q));
		if (e != NULL && e->level == 1 && !e->late)
			preds = lappend(preds, e);
		else
			*rest = lappend(*rest, q);
	}
	if (preds == NIL)
		return NULL;
	if (list_length(preds) == 1)
		return linitial(preds);
	p = new_junction(c, SAP_AND, list_length(preds));
	foreach_ptr(SiftAggPred, e, preds)
		p->child[foreach_current_index(e)] = e;
	junction_done(p);
	return p;
}

/*
 * EXISTS (SELECT .. FROM jsonb_to_recordset(array) e [LEFT JOIN LATERAL (lookup of one row) m ON true] WHERE ..) that reads a column of a joined table
 * (or the lookup): PostgreSQL cannot pull it up, so it is a test of each joined tuple. The elements of its row go through a combo whose
 * inputs are the element's leaves, the row's and the lookup outputs the tuple holds, the lookup being a correlated child of the elements
 * (the join is `scoped`: no stage of the tuples applies it). The sub-query's range table is appended to the query's, so that its columns
 * and the query's are told apart; the conjuncts that read the leaves of the elements only are tested in bulk, before the combo.
 */
SiftAggPred *
compile_exists_post(AggCtx *c, SubPlan *sp)
{
	ExistsScan	x = {exists_subquery(c, sp)};
	Index		offset = list_length(c->root->parse->rtable);
	int			nj0 = list_length(c->joins),
				nc0 = list_length(c->combos),
				grp,
				ci,
				si;
	RangeTblFunction *f;
	FuncKind	kind;
	JoinCtx    *jc = NULL;
	List	   *quals,
			   *rest = NIL;
	AggCtx		saved;
	SiftAggPred *p = NULL;
	Node	   *expr;

	if (x.q == NULL || !exists_scan((Node *) x.q->jointree, &x))
		return NULL;
	f = linitial_node(RangeTblFunction, rt_fetch(x.frti, x.q->rtable)->functions);
	if (rt_fetch(x.frti, x.q->rtable)->funcordinality || list_length(rt_fetch(x.frti, x.q->rtable)->functions) != 1 ||
		(kind = func_kind_of(f)) == FK_NONE || contain_volatile_functions((Node *) x.quals))
		return NULL;
	grp = group_leaf_of(c, outer_mutator(linitial(((FuncExpr *) f->funcexpr)->args), sp), func_leaf_kind(kind));
	if (grp < 0)
		return NULL;
	quals = copyObject(x.quals);
	OffsetVarNodes((Node *) quals, offset, 0);
	quals = (List *) outer_mutator((Node *) quals, sp);
	saved = scope_enter(c, grp);
	c->func_rti = x.frti + offset;
	c->func = f;
	c->func_kind = kind;
	c->rtable = list_concat(list_copy(c->root->parse->rtable), list_copy(x.q->rtable));
	c->dpctx = NIL;
	c->oparams = NIL;
	if (x.lrti == 0 || (jc = scoped_join(c, &x, offset)) != NULL)
	{
		p = new_pred(c, SAP_ELEM_EXISTS);
		p->nchild = 0;
		p->leaf2 = grp;
		p->level = 2;
		p->child = palloc(sizeof(SiftAggPred *));
		if ((p->child[0] = element_conjuncts(c, quals, &rest)) != NULL)
			p->nchild = 1;
		expr = rest == NIL ? NULL : normalize_expr(c, (Node *) make_ands_explicit(rest));
		/*
		 * An expression that may raise stays with PostgreSQL: it evaluates the test for the tuples that reach its join and the elements before
		 * the first that passes, which the scan, applying the test after all the joins, cannot tell
		 */
		if (expr == NULL || raising_walker(expr, c) || (jc && raising_walker((Node *) rt_fetch(x.lrti, x.q->rtable)->subquery, c)) ||
			!combo_slot_in(c, jc, expr, false, jc == NULL, &ci, &si) || (jc && ci != jc->combo))
			p = NULL;
		else
		{
			SlotReq    *rq = slot_req(c, ci, si);

			p->leaf = ci;
			p->op = si;
			rq->expr = lower_scoped(c, ci, rq->expr);
			rq->lowered = true;
			p->desc = psprintf("EXISTS %s[] (%s)", leaf_name(c, grp), combo_text(c, ci, expr));
			c->elem_work += (p->nchild + 1) * elems_of(c, grp);
		}
	}
	scope_leave(c, &saved, grp);
	if (p == NULL)
	{
		c->joins = list_truncate(c->joins, nj0);
		c->combos = list_truncate(c->combos, nc0);
	}
	return p;
}

/*
 * The existential tests: EXISTS over the elements of an array, jsonb_path_exists / @? and @> on an array of objects. The result is
 * never NULL for EXISTS; the others are NULL for a NULL document, which the executor (like WHERE) treats as false: *target is that
 * document's expression, for the negation to prove it NOT NULL.
 */
SiftAggPred *
compile_existential(AggCtx *c, Node *n, Node **target)
{
	Oid			fid;

	*target = NULL;
	if (IsA(n, SubPlan))
		return compile_exists(c, (SubPlan *) n);
	fid = func_of(n);
	if (fid == F_JSONB_PATH_EXISTS || fid == F_JSONB_PATH_EXISTS_OPR)
	{
		*target = linitial(func_args(n));
		return compile_path_exists(c, n);
	}
	if (fid == F_JSONB_EXISTS || fid == F_JSONB_EXISTS_ANY || fid == F_JSONB_EXISTS_ALL)
	{
		*target = linitial(func_args(n));
		return compile_has_keys(c, fid, *target, lsecond(func_args(n)));
	}
	if (fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED)
	{
		bool		fwd = fid == F_JSONB_CONTAINS;

		*target = fwd ? linitial(func_args(n)) : lsecond(func_args(n));
		return compile_contains(c, *target, fwd ? lsecond(func_args(n)) : linitial(func_args(n)));
	}
	return NULL;
}

/* the document is a column with a NOT NULL constraint */
bool
target_not_null(AggCtx *c, Node *target)
{
	Var		   *v = (Var *) strip_relabel(target);

	return IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0 &&
		bms_is_member(v->varattno, c->rel->notnullattnums);
}

static bool
dep_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		record_plan_function_dependency(root, ((FuncExpr *) n)->funcid);
	else if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		record_plan_function_dependency(root, ((OpExpr *) n)->opfuncid);
	}
	else if (IsA(n, ScalarArrayOpExpr))
	{
		set_sa_opfuncid((ScalarArrayOpExpr *) n);
		record_plan_function_dependency(root, ((ScalarArrayOpExpr *) n)->opfuncid);
	}
	else if (IsA(n, CoerceViaIO) || IsA(n, ArrayCoerceExpr) || IsA(n, Aggref))
		;
	return expression_tree_walker(n, dep_walker, root);
}

void
record_deps(PlannerInfo *root, Node *n)
{
	dep_walker(n, root);
}

/* the tree of preds as one flat array, ids ascending, to count them */
void
pred_deps(PlannerInfo *root, SiftAggPred *p)
{
	if (p == NULL)
		return;
	record_deps(root, p->expr);
	for (int i = 0; i < p->nchild; i++)
		pred_deps(root, p->child[i]);
}

bool
out_walker(Node *n, OutWalk *w)
{
	if (n == NULL || w->bad)
		return false;
	foreach_ptr(Node, k, w->keys)
		if (equal(n, k))
			return false;
	if (IsA(n, Aggref) || IsA(n, GroupingFunc))
	{
		if (!list_member(w->aggs, n))
			w->aggs = lappend(w->aggs, n);
		return false;
	}
	if (IsA(n, Var) || IsA(n, PlaceHolderVar) || IsA(n, WindowFunc) || IsA(n, SubPlan) || IsA(n, SubLink))
	{
		w->bad = true;
		return false;
	}
	return expression_tree_walker(n, out_walker, w);
}
