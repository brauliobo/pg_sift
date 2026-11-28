/*
 * agg/plan/qual.c - the WHERE clauses of a SiftAggV6 statement: the quals of the index scan (a comparison, an IN list or a null test of one column with a constant or a parameter) and
 * the test that the index holds every row the statement reads.
 */
/* check-memory: allow-allocs every list here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_am_d.h"
#include "catalog/pg_operator_d.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/ht/ht.h"
#include "agg/safety.h"
#include "agg/plan/int.h"
#include "agg/ext.h"
#include "agg/filter/pg.h"
#include "agg/qual/bqual.h"
#include "agg/qual/contains.h"
#include "agg/qual/qual.h"
#include "agg/refuse.h"
#include "agg/unnest/jsonb.h"
#include "col/am/preds.h"
#include "col/col.h"

/* an expression of constants and stable functions: its value is the same for the whole scan, which reads it when it starts */
static bool
stable_operand(const Node *n)
{
	return !IsA(n, Const) && !IsA(n, Param) && !contain_var_clause((Node *) n) && !contain_volatile_functions((Node *) n) && !contain_subplans((Node *) n) &&
		!expression_returns_set((Node *) n) && !agg_node_has_extern_param((Node *) n);
}

/*
 * a constant, a parameter, or a stable expression the scan reads when it starts (and again when it is rescanned); in a FILTER clause a constant or $n, which the node binds as a constant
 * before it binds the clause (sift_v6_bind_params)
 */
static bool
operand_usable(const V6Plan *p, const Node *n)
{
	if (IsA(n, Param))
		return ((const Param *) n)->paramkind == PARAM_EXTERN || (!p->in_filter && ((const Param *) n)->paramkind == PARAM_EXEC);
	if (stable_operand(n))
		return !p->in_filter;
	return IsA(n, Const) && !((const Const *) n)->constisnull;
}

static bool
is_operand(const Node *n)
{
	return IsA(n, Const) || IsA(n, Param) || stable_operand(n);
}

/* a column of a domain, which the operator of its base type reads through a relabeling */
static Node *
domain_column(Node *n)
{
	const RelabelType *r = (const RelabelType *) n;

	return IsA(n, RelabelType) && IsA(r->arg, Var) && getBaseType(exprType((Node *) r->arg)) == r->resulttype ? (Node *) r->arg : n;
}

/* the column col of the index that e (a column of the relation or an expression the index stores) is read from */
static Expr *
index_var(const Expr *e, int col)
{
	const bool	json = IsA(e, Var) && sift_v6_vatt_is_json(((const Var *) e)->varattno);

	return (Expr *) makeVar(INDEX_VAR, json ? ((const Var *) e)->varattno : col + 1, getBaseType(exprType((const Node *) e)), exprTypmod((const Node *) e),
							exprCollation((const Node *) e), 0);
}

/* the operator family the comparisons of the column are of: what a key of a document reads as is a text or a float8 */
static Oid
col_opfamily(const V6Plan *p, const SiftV6Col *col)
{
	return col->jleaf > 0 ? get_opclass_family(GetDefaultOpClass(col->typid, BTREE_AM_OID)) : sift_v6_plan_opfamily(p, col->attno);
}

/*
 * A numeric column of an index decides a comparison with a constant of its scale range on the scaled integers; a NaN row is decided by the scan (col/f2/scan/verdict.c). A FILTER clause
 * compares the scaled integers of its rows with any constant (agg/filter/pg.c), for a column whose type modifier says the scale its leaf holds
 */
static bool
numeric_exact(const V6Plan *p, int strat, const SiftV6Col *col, const Var *v, const Node *operand)
{
	if (p->in_filter)
		return v->vartypmod >= VARHDRSZ && sift_v6_numeric_typmod_scale(v->vartypmod) == col->scale;
	return !p->is_table && strat >= BTLessStrategyNumber && strat <= BTGreaterStrategyNumber && IsA(operand, Const) &&
		sift_col_numeric_key_fits(col->scale, ((const Const *) operand)->constvalue);
}

/* The strategy of a comparison of the column with a constant or parameter (a text of a table is no scan key: its leaf holds the cell, which only the batch decides): of the operator class, or, on an index, <> and NOT LIKE '%literal%' (agg/qual/qual.h) that only a scan outside a FILTER clause has; 0: not one the scan decides */
static int
compare_strategy(const V6Plan *p, const OpExpr *q, const SiftV6Col *col, const Node *operand)
{
	const bool	text = sift_v6_class(col->typid) == SV6C_TEXT;
	const Oid	family = col_opfamily(p, col);
	const bool	index = !p->is_table;
	int			strat = get_op_opfamily_strategy(q->opno, family);

	if (strat == 0 && (index || !text) && !stable_operand(operand))
		strat = sift_v6_qual_private_strategy(q->opno, family);
	if (text && (p->is_table || !sift_text_bytes_equal(q->inputcollid)))
		return 0;
	if (col->typid == NUMERICOID && !numeric_exact(p, strat, col, (const Var *) domain_column(linitial(q->args)), operand))
		return 0;
	if (p->in_filter && strat > BTMaxStrategyNumber && strat != COL_STRAT_NE)
		return 0;
	switch (strat)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return text ? 0 : strat;
		case BTEqualStrategyNumber:
			return strat;
		case COL_STRAT_NE:
			return sift_v6_class(col->typid) == SV6C_F64 || (exprType(operand) != col->typid && !sift_int_pair(col->typid, exprType(operand))) ? 0 : strat;
		case COL_STRAT_LIKE:
		case COL_STRAT_NOT_LIKE:
			return index && text && IsA(operand, Const) && sift_v6_qual_like_exact((const Const *) operand) ? strat : 0;
		default:
			return 0;
	}
}

/* a column the scan keys test on the stored rows, or in a FILTER a boolean expression the host stores: not a member of a map, which the scan finds in each row it reads */
static bool
key_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *col)
{
	return (sift_v6_plan_column(p, e, numeric_ok, col) && col->jmember == 0) ||
		(p->in_filter && exprType((const Node *) e) == BOOLOID && sift_v6_plan_expr_column(p, e, col));
}

static Expr *
qual_compare(V6Plan *p, OpExpr *op)
{
	Node	   *l,
			   *r;
	OpExpr	   *q = copyObject(op);
	SiftV6Col	col;

	if (list_length(op->args) != 2)
		return NULL;
	l = domain_column(linitial(q->args));
	r = domain_column(lsecond(q->args));
	if (IsA(r, Var) && is_operand(l))
	{
		Node	   *t = l;

		q->opno = get_commutator(q->opno);
		if (!OidIsValid(q->opno))
			return NULL;
		l = r;
		r = t;
		q->args = list_make2(l, r);
	}
	if (!key_column(p, (Expr *) l, true, &col) || !operand_usable(p, r) || compare_strategy(p, q, &col, r) == 0)
		return NULL;
	linitial(q->args) = index_var((Expr *) l, col.attno - 1);
	q->opfuncid = get_opcode(q->opno);
	return (Expr *) q;
}

/*
 * the array of an IN list: a constant; $n of the WHERE of an unnest, which the node binds to the value of its execution before it reads (sift_v6_bind_params); or, on an index,
 * any other parameter, stable expression or list of the values of the outer row of a LATERAL probe, a runtime key the scan evaluates when it starts (and again when it is rescanned)
 */
static bool
in_array_usable(const V6Plan *p, const Node *arr)
{
	if (IsA(arr, Const))
		return !((const Const *) arr)->constisnull;
	if (p->is_table)
		return false;
	if (IsA(arr, Param) && p->xseed != NULL && p->xseed->unnest)
		return ((const Param *) arr)->paramkind == PARAM_EXTERN && !p->in_filter;
	return operand_usable(p, arr);
}

static Expr *
qual_in_list(V6Plan *p, ScalarArrayOpExpr *sa)
{
	ScalarArrayOpExpr *q = copyObject(sa);
	SiftV6Col	col;

	if ((!sa->useOr && !p->in_filter) || (p->index != NULL && !p->index->amsearcharray) || list_length(sa->args) != 2 || !in_array_usable(p, lsecond(sa->args)) ||
		!key_column(p, (Expr *) domain_column(linitial(sa->args)), p->in_filter, &col) || sift_v6_class(col.typid) == SV6C_F64 ||
		(col.typid == NUMERICOID && !numeric_exact(p, BTEqualStrategyNumber, &col, (const Var *) domain_column(linitial(sa->args)), lsecond(sa->args))) ||
		get_op_opfamily_strategy(sa->useOr ? sa->opno : get_negator(sa->opno), col_opfamily(p, &col)) != BTEqualStrategyNumber ||
		(sift_v6_class(col.typid) == SV6C_TEXT && (p->is_table || !sift_text_bytes_equal(sa->inputcollid))))
		return NULL;
	linitial(q->args) = index_var((Expr *) domain_column(linitial(sa->args)), col.attno - 1);
	return (Expr *) q;
}

/* the leaf "<doc>#present" of the column att of the index, true where the document is not SQL NULL, which a scan key IS [NOT] NULL of the document reads (col/am/preds.c); -1: none */
static int
present_leaf(const V6Plan *p, int att)
{
	const SiftColSchema *sch = p->colrel->schema;

	for (int c = 0; c < sch->ncols; c++)
		if (sch->cols[c].attno == att && sift_col_def_is_present(&sch->cols[c]))
			return c;
	return -1;
}

/*
 * IS [NOT] NULL of a shredded document, to a format that reads the documents as the heap does: of a WHERE, a scan key of the column of the index, which its presence leaf decides; of a FILTER, the
 * test of the value the presence leaf holds, which the scan reads as a column. NULL: the index has no such leaf
 */
static Expr *
doc_null_test(V6Plan *p, NullTest *nt)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	NullTest   *q = copyObject(nt);
	SiftV6Col	col;
	Var		   *present;
	int			att,
				leaf;

	if (p->is_table || p->colrel == NULL || p->ops->jsonb == NULL || !p->ops->jsonb->exact_values || !sift_v6_json_doc_att(&rel, nt->arg, &att) || (leaf = present_leaf(p, att)) < 0)
		return NULL;
	present = makeVar(p->rel->relid, SIFT_V6_JHELD_VATT + leaf, INT8OID, -1, InvalidOid, 0);
	if (p->in_filter && !key_column(p, (Expr *) present, false, &col))
		return NULL;
	q->arg = index_var(p->in_filter ? (Expr *) present : nt->arg, att - 1);
	return (Expr *) q;
}

static Expr *
qual_null_test(V6Plan *p, NullTest *nt)
{
	NullTest   *q = copyObject(nt);
	SiftV6Col	col;

	if (nt->argisrow)
		return NULL;
	if (IsA(nt->arg, Var) && ((const Var *) nt->arg)->vartype == JSONBOID)
		return doc_null_test(p, nt);
	if (!key_column(p, nt->arg, true, &col))
		return NULL;
	q->arg = index_var(nt->arg, col.attno - 1);
	return (Expr *) q;
}

/* doc ->> 'key' = 'constant' where the batches of the scan are the elements of an unnest, whose quals are evaluated on them and not by an index scan */
static Expr *
qual_json_text(V6Plan *p, OpExpr *op)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return p->xseed != NULL && p->xseed->unnest && !p->is_table ? sift_v6_json_text_qual(&rel, op) : NULL;
}

static bool
has_numeric_var(Node *n, void *context)
{
	return n != NULL && ((IsA(n, Var) && ((Var *) n)->vartype == NUMERICOID) || expression_tree_walker(n, has_numeric_var, context));
}

static Expr *
bool_equals(V6Plan *p, Expr *e, bool value)
{
	return qual_compare(p, (OpExpr *) make_opclause(BooleanEqualOperator, BOOLOID, false, e, (Expr *) makeBoolConst(value, false), InvalidOid, InvalidOid));
}

/*
 * a boolean column or stored expression is its comparison with true; its negation (NOT, or what negate_clause makes of an expression: IS NULL of an IS NOT NULL, the
 * AND of an OR) the comparison with false: NULL keeps no row in either
 */
static Expr *
qual_bool_column(V6Plan *p, Expr *clause)
{
	Expr	   *q;

	if (exprType((Node *) clause) != BOOLOID)
		return NULL;
	if (is_notclause(clause))
		return bool_equals(p, get_notclausearg(clause), false);
	if ((q = bool_equals(p, clause, true)) != NULL || IsA(clause, Var))
		return q;
	return bool_equals(p, (Expr *) negate_clause((Node *) copyObject(clause)), false);
}

Expr *
sift_v6_index_qual(V6Plan *p, Expr *clause)
{
	Expr	   *q;

	/* a table has no scan to decide the rows whose stored value is a NaN in a WHERE clause: PostgreSQL's own comparison of the datums does */
	if (p->is_table && !p->in_filter && has_numeric_var((Node *) clause, NULL))
		return NULL;

	switch (nodeTag(clause))
	{
		case T_OpExpr:
			if ((q = qual_compare(p, (OpExpr *) clause)) != NULL || (q = qual_json_text(p, (OpExpr *) clause)) != NULL)
				return q;
			return qual_bool_column(p, clause);
		case T_ScalarArrayOpExpr:
			return qual_in_list(p, (ScalarArrayOpExpr *) clause);
		case T_NullTest:
			return (q = qual_null_test(p, (NullTest *) clause)) != NULL ? q : qual_bool_column(p, clause);
		default:
			return qual_bool_column(p, clause);
	}
}

/* a qual no scan key says (a column against a column, an OR, a numeric against a constant): the scan runs it over the columns of each batch (agg/qual/bqual.h) */
/* the index qual a batch qual implies: the scan prunes its units by it too */
static void
add_implied_qual(V6Plan *p, Expr *clause)
{
	Expr	   *implied = p->ops == NULL || p->ops->zone == NULL ? NULL : sift_v6_implied_qual(p->root, clause);
	Expr	   *q = implied == NULL ? NULL : sift_v6_index_qual(p, implied);

	if (q != NULL)
		p->spec.indexqual = lappend(p->spec.indexqual, q);
}

static bool
batch_clause(V6Plan *p, Expr *where)
{
	SiftV6Input in = {.indexoid = p->relid,.is_table = p->is_table,.nkeys = p->is_table ? p->sch.natts : 0};
	Expr	   *clause;

	if (!p->batch_ok || contain_volatile_functions((Node *) where))
		return false;
	if (!p->is_table)
		sift_v6_input_init_info(&in, p->index);
	clause = sift_v6_plan_json_test(p, where, true);
	clause = clause == where ? copyObject(clause) : clause;
	ChangeVarNodes((Node *) clause, p->rel->relid, SIFT_V6_BQ_RELID, 0);
	if (!sift_v6_bq_plan_ok(p->root, &in, clause))
		return false;
	p->spec.vqual = lappend(p->spec.vqual, clause);
	add_implied_qual(p, where);
	return true;
}

static bool
qual_batch(V6Plan *p, RestrictInfo *ri)
{
	return !sift_v6_text_clause(p->rel, ri) && batch_clause(p, ri->clause);
}

/* the clauses of the query imply the predicate of a partial index: every row the query reads is in the index */
bool
sift_v6_index_complete(const V6Plan *p)
{
	List	   *clauses = NIL;

	if (p->index == NULL || p->index->indpred == NIL)
		return true;
	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		clauses = lappend(clauses, ri->clause);
	return predicate_implied_by(p->index->indpred, clauses, false);
}

static bool
unsafe_initplan_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		foreach_ptr(SubPlan, sp, root->init_plans)
			if (list_member_int(sp->setParam, ((Param *) n)->paramid))
				return !initplan_ok(root, (Param *) n);
		return false;
	}
	return expression_tree_walker(n, unsafe_initplan_walker, root);
}

/* a test of the leaves of a document that a WHERE clause is the conjunction of (agg/qual/contains.h): a scan key or a batch qual */
static bool
compile_lowered(V6Plan *p, const List *tests)
{
	foreach_ptr(Expr, t, tests)
	{
		Expr	   *q = IsA(t, NullTest) ? qual_null_test(p, (NullTest *) t) : NULL;

		if (q != NULL)
			p->spec.indexqual = lappend(p->spec.indexqual, q);
		else if (!batch_clause(p, t))
			return false;
	}
	return true;
}

/* the tests of the leaves a clause is the conjunction of: of a containment (agg/qual/contains.h), or of the presence of keys and the test of a document that may be NULL (sift_v6_json_vars) */
static List *
lowered_tests(V6Plan *p, const SiftV6Rel *rel, Expr *clause)
{
	List	   *tests = sift_v6_contains_quals(rel, clause);

	return tests != NIL ? tests : (tests = sift_v6_conjuncts(sift_v6_plan_json_test(p, clause, true))) != NIL && list_length(tests) > 1 ? tests : NIL;
}

/* the tests of the leaves of a clause, or of a containment whose lists of strings the tests of the plan read: the quals of their flags and the tests of the other members */
static bool
compile_tests(V6Plan *p, const SiftV6Rel *rel, Expr *clause)
{
	Expr	   *rest;
	List	   *flags = sift_v6_tests_contains_quals(p, clause, &rest);
	List	   *tests = flags == NIL ? lowered_tests(p, rel, clause) : rest != NULL ? lowered_tests(p, rel, rest) : NIL;

	if (tests == NIL && (flags == NIL || rest != NULL))
		return false;
	p->spec.indexqual = list_concat(p->spec.indexqual, flags);
	return compile_lowered(p, tests);
}

List *
sift_v6_conjuncts(Expr *clause)
{
	List	   *out = NIL;

	if (!is_andclause(clause))
		return list_make1(clause);
	foreach_ptr(Expr, arg, ((BoolExpr *) clause)->args)
		out = list_concat(out, sift_v6_conjuncts(arg));
	return out;
}

/* the scan of an unnest of a table can read every row again by its address, where the quals of the statement decide it (sift_v6_unnest_where): a clause the executor evaluates alone needs no scan key there */
static bool
heap_decides(const V6Plan *p, const Expr *clause)
{
	return p->is_table && p->xseed != NULL && p->xseed->unnest && !contain_subplans((Node *) clause) &&
		!agg_node_has_param((const Node *) clause);
}

/* a clause of the WHERE that is none of the special ones: a scan key, a key of the JSON leaves, a batch qual, tests of the leaves of a document, or one the heap decides */
static bool
compile_clause(V6Plan *p, const SiftV6Rel *rel, RestrictInfo *ri)
{
	Expr	   *q = ri->pseudoconstant || p->ops == NULL || p->ops->zone == NULL ? NULL : sift_v6_index_qual(p, ri->clause);

	if (q == NULL && !ri->pseudoconstant)
		q = sift_v6_tests_or_qual(p, ri->clause);
	List	   *jq = ri->pseudoconstant || q != NULL || heap_decides(p, ri->clause) || (p->is_table && !p->ops->zone->table_json) ? NIL : sift_v6_json_qual(rel, ri->clause);

	if (q != NULL)
		p->spec.indexqual = lappend(p->spec.indexqual, q);
	else if (jq != NIL)
		p->spec.jsonqual = lappend(p->spec.jsonqual, jq);
	else if (qual_batch(p, ri))
		return true;
	else if (heap_decides(p, ri->clause))
		p->heap_rows = true;
	else
		return !ri->pseudoconstant && compile_tests(p, rel, ri->clause);
	return true;
}

bool
sift_v6_compile_quals(V6Plan *p)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		if (unsafe_initplan_walker((Node *) ri->clause, p->root))
			return false;
		if (sift_v6_unnest_test_ri(p, ri))
			continue;
		if (p->index != NULL && p->index->indpred != NIL && predicate_implied_by(list_make1(ri->clause), p->index->indpred, false))
			continue;
		if (!ri->pseudoconstant && (sift_v6_tid_qual(p, ri) || sift_v6_text_qual(p, ri)))
			continue;
		if (!compile_clause(p, &rel, ri))
		{
			sift_agg_arm_refused("single", AGG_REFUSE_WHERE);
			return false;
		}
	}
	return sift_v6_compile_text(p) && !(p->spec.tid_range && p->text != NULL);
}
