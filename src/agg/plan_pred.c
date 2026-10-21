/*
 * agg/plan_pred.c - predicate compiler: comparisons, IN lists, lookup tables, junctions and booleans over leaves
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

/* ---------- predicates ---------- */

TypeClass
type_class(Oid t)
{
	switch (t)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return TC_INT;
		case DATEOID:
			return TC_DATE;
		case TIMESTAMPOID:
			return TC_TS;
		case TIMESTAMPTZOID:
			return TC_TSTZ;
		case FLOAT4OID:
		case FLOAT8OID:
			return TC_FLOAT;
		case BOOLOID:
			return TC_BOOL;
		case NUMERICOID:
			return TC_NUMERIC;
		default:
			return TC_NONE;
	}
}

/* the comparison a builtin operator is: SiftCmpOp, or -1 */
int
builtin_cmp(Oid opno, Oid *lt, Oid *rt)
{
	char	   *name;
	int			op = -1;

	if (opno >= FirstGenbkiObjectId)
		return -1;
	name = get_opname(opno);
	if (name == NULL)
		return -1;
	if (strcmp(name, "=") == 0)
		op = SCMP_EQ;
	else if (strcmp(name, "<>") == 0)
		op = SCMP_NE;
	else if (strcmp(name, "<") == 0)
		op = SCMP_LT;
	else if (strcmp(name, "<=") == 0)
		op = SCMP_LE;
	else if (strcmp(name, ">") == 0)
		op = SCMP_GT;
	else if (strcmp(name, ">=") == 0)
		op = SCMP_GE;
	if (op >= 0)
		op_input_types(opno, lt, rt);
	return op;
}

int
commute(int op)
{
	switch (op)
	{
		case SCMP_LT:
			return SCMP_GT;
		case SCMP_LE:
			return SCMP_GE;
		case SCMP_GT:
			return SCMP_LT;
		case SCMP_GE:
			return SCMP_LE;
		default:
			return op;
	}
}

SiftAggPred *
new_pred(AggCtx *c, SiftAggPredKind kind)
{
	SiftAggPred *p = palloc0(sizeof(SiftAggPred));

	p->kind = kind;
	p->id = c->npred++;
	p->leaf = -1;
	p->leaf2 = -1;
	p->desc = pstrdup("");
	return p;
}

/* a placeholder and the guard of an absent nullable input show as the expression they stand for (the guard's Param is no parameter the deparser knows) */
static Node *
display_expr_mutator(Node *n, void *context)
{
	Node	   *arm;

	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
		return display_expr_mutator((Node *) ((PlaceHolderVar *) n)->phexpr, context);
	if ((arm = null_guarded_arm(n)) != NULL)
		return display_expr_mutator(arm, context);
	return expression_tree_mutator(n, display_expr_mutator, context);
}

/* the expression as EXPLAIN shows it, its Vars resolved through the query's range table */
char *
expr_text(AggCtx *c, Node *n)
{
	if (c->dpctx == NIL)
	{
		PlannedStmt *stmt = makeNode(PlannedStmt);
		Result	   *plan = makeNode(Result);

		stmt->rtable = c->rtable ? c->rtable : c->root->parse->rtable;
		c->dpctx = deparse_context_for_plan_tree(stmt, select_rtable_names_for_explain(stmt->rtable, NULL));
		/* the Params of a sub-plan are looked up in the plan the expression belongs to; the InitPlans of the query give
		 * their output Params (the deparser asserts on a Param that nothing generates) */
		plan->plan.initPlan = c->root->init_plans;
		c->dpctx = set_deparse_context_plan(c->dpctx, (Plan *) plan, NIL);
	}
	return deparse_expression(display_expr_mutator(n, NULL), c->dpctx, true, false);
}

const char *
cmp_text(int op)
{
	static const char *const t[] = {"=", "<>", "<", "<=", ">", ">="};

	return t[op];
}

/* the leaf an expression read as a stored value (match_leaf): a column of jsonb_to_recordset is read as the column of the record wherever the expression is */
void
use_matched_leaf(AggCtx *c, int leaf)
{
	c->rec_pending = leaf + 1;
	use_leaf(c, leaf);
}

void
use_leaf(AggCtx *c, int leaf)
{
	if (c->rec_pending == leaf + 1)
		c->rec_ok = bms_add_member(c->rec_ok, leaf);
	else
		c->rec_bad = bms_add_member(c->rec_bad, leaf);
	c->rec_pending = 0;
	c->used = bms_add_member(c->used, leaf);
	c->read = bms_add_member(c->read, leaf);
	if (leaf_of(c, leaf)->parent >= 0 && leaf_of(c, leaf)->parent != c->group)
		c->other_group = true;
}

SiftAggPred *
make_leaf_pred(AggCtx *c, SiftAggPredKind kind, int leaf)
{
	SiftAggPred *p = new_pred(c, kind);

	p->leaf = leaf;
	p->level = leaf_level(c, leaf);
	use_leaf(c, leaf);
	return p;
}

/* numeric expression op operand; two expressions are compared as the sign of their comparison (SNX_CMP3) against 0 */
static SiftAggPred *
compile_ncmp(AggCtx *c, int cmp, Node *l, Node *r)
{
	SiftAggPred *p = new_pred(c, SAP_NCMP);
	Node	   *lhs = l,
			   *rhs = r,
			   *operand;
	int			dig,
				s2,
				d2;

	if (!pseudo_const(r) && pseudo_const(l))
	{
		lhs = r;
		rhs = l;
		cmp = commute(cmp);
	}
	operand = rhs;
	p->num.step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	if (!nx_walk(c, lhs, &p->num, &p->num.scale, &dig))
		return NULL;
	if (!pseudo_const(rhs))
	{
		if (!nx_walk(c, rhs, &p->num, &s2, &d2) || !nx_addsub(&p->num, SNX_CMP3, &p->num.scale, &dig, s2, d2))
			return NULL;
		operand = (Node *) makeConst(NUMERICOID, -1, InvalidOid, -1, NumericGetDatum(int64_to_numeric(0)), false, false);
	}
	if ((p->level = nx_level(c, &p->num)) < 0)
		return NULL;
	p->op = cmp;
	p->expr = operand;
	p->argtype = NUMERICOID;
	p->late = nx_raises(&p->num);
	p->desc = psprintf("%s %s %s", expr_text(c, lhs), cmp_text(cmp), expr_text(c, rhs));
	return p;
}

/* the leaf of the outer row that a parameter of the EXISTS sub-plan stands for, -1 when n is not one */
static int
outer_leaf(AggCtx *c, Node *n)
{
	n = strip_relabel(n);
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		foreach_ptr(OuterParam, o, c->oparams)
			if (o->id == ((Param *) n)->paramid)
				return o->leaf;
	return -1;
}

/* a stored scalar that the comparison of an element with a row reads as integers or doubles: the type classes of both sides are the same */
static bool
elem_row_leaf(AggCtx *c, int leaf, TypeClass tc)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && !l->lossy && !l->wide && type_class(l->typid) == tc &&
		((l->vtype == SCV_I32 || l->vtype == SCV_I64) ? tc != TC_FLOAT : tc == TC_FLOAT && l->vtype == SCV_F64);
}

/*
 * `element leaf op column of the row` in the test of an EXISTS (the column reaches the sub-query as a parameter of the sub-plan): a test of the
 * element leaf and of the row leaf of the same type class (an int4 member against a bigint column, not against a float8 one), true for the
 * elements whose row passes the comparison, which is false when either side is NULL. An expression of the column, or of the element, is not a leaf.
 */
static SiftAggPred *
compile_elem_row(AggCtx *c, int cmp, Node *l, Node *r, Oid lt, Oid rt)
{
	SiftAggPred *p;
	TypeClass	tc = type_class(lt);
	int			el,
				rl;

	if (c->oparams == NIL)
		return NULL;
	if (outer_leaf(c, l) >= 0)
	{
		Node	   *t = l;

		l = r;
		r = t;
		cmp = commute(cmp);
	}
	if ((rl = outer_leaf(c, r)) < 0 || (el = match_leaf(c, l)) < 0 || tc == TC_NUMERIC || tc == TC_BOOL || type_class(rt) != tc ||
		leaf_of(c, el)->parent != c->group || leaf_of(c, rl)->parent >= 0 || !elem_row_leaf(c, el, tc) || !elem_row_leaf(c, rl, tc))
		return NULL;
	p = new_pred(c, SAP_ELEM_ROW);
	p->leaf = el;
	p->leaf2 = rl;
	p->op = cmp;
	p->level = 1;
	use_leaf(c, el);
	use_leaf(c, rl);
	p->desc = psprintf("%s %s %s", leaf_name(c, el), cmp_text(cmp), leaf_name(c, rl));
	return p;
}

/* leaf op operand, for a leaf the comparison kernels read */
static SiftAggPred *
compile_cmp(AggCtx *c, OpExpr *op)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(op->opno, &lt, &rt),
				leaf;
	Node	   *l,
			   *r;
	SiftAggPred *p;
	SiftSrcLeaf *lf;

	if (cmp < 0 || list_length(op->args) != 2)
		return NULL;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if ((p = compile_elem_row(c, cmp, l, r, lt, rt)) != NULL)
		return p;
	if ((leaf = match_leaf(c, l)) >= 0 && pseudo_const(r))
		;
	else if ((leaf = match_leaf(c, r)) >= 0 && pseudo_const(l))
	{
		Node	   *t = l;

		l = r;
		r = t;
		cmp = commute(cmp);
		lt = rt;
	}
	else
		return lt == NUMERICOID && rt == NUMERICOID ? compile_ncmp(c, cmp, l, r) : NULL;
	lf = leaf_of(c, leaf);
	if (lf->kind != SCL_SCALAR || lf->vtype == SCV_CODE)
		return NULL;
	if (type_class(lf->typid) == TC_NONE || type_class(lf->typid) != type_class(exprType(r)) ||
		type_class(exprType(l)) != type_class(lf->typid))
		return NULL;
	p = make_leaf_pred(c, SAP_CMP, leaf);
	p->op = cmp;
	p->expr = r;
	p->argtype = exprType(r);
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(cmp), expr_text(c, r));
	p->sel = clause_selectivity(c->root, (Node *) op, c->relid, JOIN_INNER, NULL);
	return p;
}

#define AGG_MAX_NUM_IN		64		/* constants of a numeric IN list: one comparison each */

/* numeric leaf IN (constants) is the OR of its equalities, NOT IN the AND of its inequalities: every operand is scaled like a comparison's */
static SiftAggPred *
compile_num_in(AggCtx *c, ScalarArrayOpExpr *sa, int leaf, Node *arr)
{
	List	   *elems = NIL;
	SiftAggPred *p;
	int			i = 0;

	if (IsA(arr, Const))
	{
		Const	   *k = (Const *) arr;
		Datum	   *el;
		bool	   *nl;
		int			ne;

		if (k->constisnull)
			return new_pred(c, SAP_FALSE);
		deconstruct_array(DatumGetArrayTypeP(k->constvalue), NUMERICOID, -1, false, TYPALIGN_INT, &el, &nl, &ne);
		for (int j = 0; j < ne; j++)
			elems = lappend(elems, makeConst(NUMERICOID, -1, InvalidOid, -1, el[j], nl[j], false));
	}
	else if (IsA(arr, ArrayExpr))
		elems = ((ArrayExpr *) arr)->elements;
	else
		return NULL;
	if (elems == NIL)
		return new_pred(c, sa->useOr ? SAP_FALSE : SAP_TRUE);
	if (list_length(elems) > AGG_MAX_NUM_IN)
		return NULL;
	p = new_pred(c, sa->useOr ? SAP_OR : SAP_AND);
	p->level = leaf_level(c, leaf);
	p->nchild = list_length(elems);
	p->child = palloc(p->nchild * sizeof(SiftAggPred *));
	foreach_ptr(Node, e, elems)
	{
		SiftAggPred *q = make_leaf_pred(c, SAP_CMP, leaf);

		q->op = sa->useOr ? SCMP_EQ : SCMP_NE;
		q->expr = e;
		q->argtype = NUMERICOID;
		q->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(q->op), expr_text(c, e));
		p->child[i++] = q;
	}
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), sa->useOr ? "IN" : "NOT IN", expr_text(c, arr));
	return p;
}

/* leaf IN (constants), leaf NOT IN (constants), on integer-valued leaves */
static SiftAggPred *
compile_in(AggCtx *c, ScalarArrayOpExpr *sa)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(sa->opno, &lt, &rt),
				leaf;
	Node	   *l,
			   *r;
	SiftAggPred *p;
	SiftSrcLeaf *lf;

	if (cmp < 0 || list_length(sa->args) != 2 || !((sa->useOr && cmp == SCMP_EQ) || (!sa->useOr && cmp == SCMP_NE)))
		return NULL;
	l = strip_relabel(linitial(sa->args));
	r = strip_relabel(lsecond(sa->args));
	if ((leaf = match_leaf(c, l)) < 0 || !pseudo_const(r))
		return NULL;
	lf = leaf_of(c, leaf);
	if (lf->kind == SCL_SCALAR && lf->typid == NUMERICOID && lf->vtype == SCV_I64 && type_class(lt) == TC_NUMERIC)
		return compile_num_in(c, sa, leaf, r);
	if (lf->kind != SCL_SCALAR || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64) || type_class(lf->typid) == TC_NONE ||
		type_class(lf->typid) != type_class(lt) || type_class(lf->typid) != type_class(rt) ||
		type_class(lf->typid) == TC_FLOAT)
		return NULL;
	p = make_leaf_pred(c, SAP_IN, leaf);
	p->op = !sa->useOr;
	p->expr = r;
	p->argtype = rt;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), sa->useOr ? "IN" : "NOT IN", expr_text(c, r));
	p->sel = clause_selectivity(c->root, (Node *) sa, c->relid, JOIN_INNER, NULL);
	return p;
}

/* any boolean expression of one dictionary or integer leaf */
static SiftAggPred *
compile_lut(AggCtx *c, Node *n)
{
	int			leaf;
	Oid			ptype,
				pcoll;
	int32		ptypmod;
	SiftAggPred *p;

	if (exprType(n) != BOOLOID)
		return NULL;
	if (!single_leaf(c, n, &leaf, &ptype, &ptypmod, &pcoll) || !lut_expr_ok(c, leaf, n))
		return compile_combo_pred(c, n);
	p = make_leaf_pred(c, SAP_LUT, leaf);
	p->expr = substitute_leaf(c, n, ptype, ptypmod, pcoll);
	p->desc = psprintf("%s: %s", leaf_name(c, leaf), expr_text(c, n));
	return p;
}

/* an AND / OR node of nchild children, the children to be set (junction_done after) */
SiftAggPred *
new_junction(AggCtx *c, SiftAggPredKind kind, int nchild)
{
	SiftAggPred *p = new_pred(c, kind);

	p->nchild = nchild;
	p->child = palloc(nchild * sizeof(SiftAggPred *));
	return p;
}

void
junction_done(SiftAggPred *p)
{
	StringInfoData s;

	initStringInfo(&s);
	for (int i = 0; i < p->nchild; i++)
	{
		p->level = Max(p->level, p->child[i]->level);
		p->late |= p->child[i]->late;
		appendStringInfo(&s, "%s%s", i ? (p->kind == SAP_AND ? " AND " : " OR ") : "", p->child[i]->desc);
	}
	p->desc = psprintf("(%s)", s.data);
}

SiftAggPred *
compile_junction(AggCtx *c, SiftAggPredKind kind, List *args)
{
	SiftAggPred *p = new_junction(c, kind, list_length(args));
	int			i = 0;

	foreach_ptr(Node, a, args)
	{
		if ((p->child[i] = compile_pred(c, a)) == NULL || p->child[i]->level > 1)
			return NULL;
		i++;
	}
	junction_done(p);
	return p;
}

static SiftAggPred *
compile_bool(AggCtx *c, BoolExpr *b)
{
	SiftAggPred *p;
	Node	   *target;

	if (b->boolop != NOT_EXPR)
		return compile_junction(c, b->boolop == AND_EXPR ? SAP_AND : SAP_OR, b->args);
	/* NOT of an existential test is its negation unless the test can be NULL (a NULL document) */
	if ((p = compile_existential(c, strip_relabel(linitial(b->args)), &target)) == NULL)
		return compile_lut(c, (Node *) b);
	if (target && !target_not_null(c, target))
		return NULL;
	p->op = 1;
	p->desc = psprintf("NOT %s", p->desc);
	return p;
}

/* is the predicate false (not true) for a tuple of NULL leaves, the one an absent input contributes? */
static bool
false_on_absent(SiftAggPred *p)
{
	switch (p->kind)
	{
		case SAP_FALSE:
		case SAP_CMP:
		case SAP_IN:
		case SAP_NCMP:
			return true;
		case SAP_NULL:
			return p->op == 1;
		case SAP_ANY_ELEM:
			return p->op == 0;
		case SAP_AND:
			for (int i = 0; i < p->nchild; i++)
				if (false_on_absent(p->child[i]))
					return true;
			return false;
		case SAP_OR:
			for (int i = 0; i < p->nchild; i++)
				if (!false_on_absent(p->child[i]))
					return false;
			return true;
		default:
			return false;
	}
}

/*
 * `CASE WHEN <input absent> THEN NULL ELSE x END` keeps the rows x does when x is not true for an absent input: its NULL leaves give
 * the NULL of the CASE, so the guard changes no row (under NOT it would, and compile_bool does not come here).
 */
static SiftAggPred *
compile_guarded(AggCtx *c, Node *arm)
{
	SiftAggPred *p = compile_pred(c, arm);

	return p != NULL && false_on_absent(p) ? p : NULL;
}

/* a condition that reads no column and nothing the query changes (stable functions, parameters): PostgreSQL's one-time filter */
static bool
is_gate(Node *n)
{
	return exprType(n) == BOOLOID && pseudo_const(n);
}

SiftAggPred *
compile_pred(AggCtx *c, Node *n)
{
	SiftAggPred *p;
	Node	   *leaf_target;
	int			leaf;

	n = strip_relabel(n);
	if (IsA(n, Const))
	{
		Const	   *k = (Const *) n;

		p = new_pred(c, !k->constisnull && DatumGetBool(k->constvalue) ? SAP_TRUE : SAP_FALSE);
		return p;
	}
	if (is_gate(n))
	{
		p = new_pred(c, SAP_GATE);
		p->expr = n;
		p->desc = psprintf("(%s)", expr_text(c, n));
		return p;
	}
	if (IsA(n, BoolExpr))
		return compile_bool(c, (BoolExpr *) n);
	if (null_guarded_arm(n) != NULL)
		return compile_guarded(c, null_guarded_arm(n));
	/* a predicate keeps the rows for which it is true: `x IS TRUE` selects the rows `x` does */
	if (IsA(n, BooleanTest) && ((BooleanTest *) n)->booltesttype == IS_TRUE)
		return compile_pred(c, (Node *) ((BooleanTest *) n)->arg);
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow && (leaf = match_leaf(c, (Node *) ((NullTest *) n)->arg)) >= 0)
	{
		p = make_leaf_pred(c, SAP_NULL, leaf);
		p->op = ((NullTest *) n)->nulltesttype == IS_NOT_NULL;
		p->desc = psprintf("%s IS %sNULL", leaf_name(c, leaf), p->op ? "NOT " : "");
		return p;
	}
	if (IsA(n, OpExpr) && (p = compile_cmp(c, (OpExpr *) n)) != NULL)
		return p;
	if (IsA(n, ScalarArrayOpExpr) && (p = compile_in(c, (ScalarArrayOpExpr *) n)) != NULL)
		return p;
	if ((p = compile_existential(c, n, &leaf_target)) != NULL)
		return p;
	return compile_lut(c, n);
}
