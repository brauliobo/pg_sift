/*
 * agg/leaf/normalize.c - the query expression rewritten for the leaves: null tests, EXISTS, jsonb casts, placeholders and nullable sides
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/leaf/int.h"

/* the negation of a condition with the NOT pushed to its tests: `a IS NULL OR b IS NULL` is that of `a IS NOT NULL AND b IS NOT NULL` */
Node *
negated_cond(Node *n)
{
	return negate_clause(copyObject(n));
}

/* a test or a junction of tests: the conditions an index holds as a boolean column, or the negation of one */
bool
is_cond(Node *n)
{
	return (IsA(n, NullTest) && !((NullTest *) n)->argisrow) || (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop != NOT_EXPR);
}

/* `x->>'k' IS [NOT] NULL` of such a member is the null test of the typed leaf */
static Node *
nulltest_mutator(Node *n, AggCtx *c)
{
	Node	   *typed;

	if (n == NULL)
		return NULL;
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow && (typed = typed_member(c, (Node *) ((NullTest *) n)->arg)) != NULL)
	{
		NullTest   *out = makeNode(NullTest);

		*out = *(NullTest *) n;
		out->arg = (Expr *) typed;
		return (Node *) out;
	}
	/* `x IS [NOT] NULL` of an array that keeps its lengths is the null test of its cardinality: the lane, not the image */
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow && OidIsValid(get_element_type(exprType((Node *) ((NullTest *) n)->arg))) &&
		attr_of_expr(c, strip_relabel((Node *) ((NullTest *) n)->arg)) > 0)
	{
		NullTest   *out = makeNode(NullTest);
		Node	   *call = (Node *) makeFuncExpr(F_CARDINALITY, INT4OID, list_make1(((NullTest *) n)->arg), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

		if (array_lane_leaf(c, call, F_CARDINALITY) >= 0)
		{
			*out = *(NullTest *) n;
			out->arg = (Expr *) call;
			return (Node *) out;
		}
	}
	/* the condition whose negation an index holds as a boolean column of a joined table (`x IS NULL` for `x IS NOT NULL`, the OR of such tests for their AND): NOT of the column (of the scan's: compile_cond_negated) */
	if (is_cond(n) && join_held_of_node(c, n) == NULL)
	{
		Node	   *neg = negated_cond(n);

		if (join_held_of_node(c, neg) != NULL)
			return (Node *) make_notclause((Expr *) neg);
	}
	return expression_tree_mutator(n, nulltest_mutator, c);
}

/*
 * jsonb_exists(doc, 'k') of the document whose list is unnested: a row of the list exists only where the list was read from an object, whose
 * key the index holds as the JSON type of its member (a JSON null member exists, a missing one has no type). Any other document (an array
 * holding the string, the string itself) answers the test and the index does not tell it, so the test is not rewritten elsewhere.
 */
static Node *
exists_mutator(Node *n, AggCtx *c)
{
	JsonRef		r = {0};
	Node	   *doc,
			   *key;

	if (n == NULL)
		return NULL;
	if (func_of(n) == F_JSONB_EXISTS && c->group >= 0 && leaf_of(c, c->group)->path != NIL)
	{
		doc = strip_relabel(linitial(func_args(n)));
		key = strip_relabel(lsecond(func_args(n)));
		if (IsA(key, Const) && !((Const *) key)->constisnull && json_access(c, doc, &r) && r.att == leaf_of(c, c->group)->attno && r.path == NIL)
		{
			NullTest   *out = makeNode(NullTest);
			Node	   *field = (Node *) makeFuncExpr(F_JSONB_OBJECT_FIELD, JSONBOID, list_make2(doc, key), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

			out->arg = (Expr *) makeFuncExpr(F_JSONB_TYPEOF, TEXTOID, list_make1(field), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
			out->nulltesttype = IS_NOT_NULL;
			out->location = -1;
			if (typeof_leaf(c, field) >= 0)
				return (Node *) out;
		}
	}
	return expression_tree_mutator(n, exists_mutator, c);
}

/*
 * `(x -> 'k')::integer` (or `bigint`) of a member the index holds as float8: the cast reads the jsonb the stored float stands for,
 * `to_jsonb(float8 member)`, the way a correlated child is given a member (a missing key and a JSON null are the same NULL, and the cast of a
 * JSON null is NULL in PostgreSQL 18 and an error before it: there the leaf is a cast read, which spec_finish refuses when a unit may hold a JSON
 * null, reads_json_null_cast). The cast stays PostgreSQL's: it rounds as numeric does and raises `integer out of range` for the rows that reach it.
 */
static Node *
jsonb_int_mutator(Node *n, AggCtx *c)
{
	FuncExpr   *f = (FuncExpr *) n;

	if (n != NULL && IsA(n, FuncExpr) && (f->funcid == F_INT4_JSONB || f->funcid == F_INT8_JSONB))
	{
		JsonRef		r = {0};
		Node	   *arg = strip_relabel(linitial(f->args));
		Node	   *num = (Node *) makeFuncExpr(F_FLOAT8_JSONB, FLOAT8OID, list_make1(arg), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

		if (json_access(c, arg, &r) && !r.as_text && match_leaf(c, num) >= 0)
		{
			f = copyObject(f);
			f->args = list_make1(makeFuncExpr(F_TO_JSONB, JSONBOID, list_make1(num), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL));
			return (Node *) f;
		}
	}
	return n == NULL ? NULL : expression_tree_mutator(n, jsonb_int_mutator, c);
}

/* the RTE_GROUP of the grouping step of the query, 0 when there is none (PostgreSQL before 18 has none) */
static Index
group_rti(AggCtx *c)
{
#if PG_VERSION_NUM >= 180000
	return c->root->group_rtindex;
#else
	return 0;
#endif
}

/*
 * Does the grouping step hand its expressions over as placeholders that it does not null itself? From PostgreSQL 18 every grouped query has the RTE_GROUP that
 * marks them; before, the expressions of grouping sets are wrapped in placeholders with no nulling of their own (a Var inside keeps the nulling of the joins).
 */
static bool
group_wraps(AggCtx *c)
{
#if PG_VERSION_NUM >= 180000
	return group_rti(c) > 0;
#else
	return c->root->parse->groupingSets != NIL;
#endif
}

/* does the expression read columns, and only those of the preserved side of the outer joins whose nullable side is the scanned table? */
static bool
reads_preserved_only(AggCtx *c, Node *e)
{
	Relids		rels = pull_varnos(c->root, e);

	if (bms_is_empty(rels))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			rels = bms_del_member(rels, jc->rti);
	return bms_is_empty(rels);
}

/* does the expression read the table of a FULL join? A scanned row it finds no match for has NULL columns there, but a placeholder over them is NULL as a whole */
static bool
reads_full_lookup(AggCtx *c, Node *e)
{
	Relids		rels = pull_varnos(c->root, e);

	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_FULL && bms_is_member(jc->rti, rels))
			return true;
	return false;
}

/*
 * The outer joins that null a placeholder. The group expression of grouping sets is kept as it was below the join, without its own
 * nulling: the joins of the scan null it unless it reads the preserved side only.
 */
static Relids
placeholder_nulling(AggCtx *c, PlaceHolderVar *ph)
{
	Relids		own = ph->phnullingrels;

	if (!group_wraps(c))
		return own;
	/* the grouping step marks the expression as nulled too */
	if (group_rti(c) > 0)
		own = bms_del_member(bms_copy(own), group_rti(c));
	if (bms_is_empty(own) && !reads_preserved_only(c, (Node *) ph->phexpr))
		return c->nulling;
	return own;
}

/*
 * The expression a placeholder stands for when the scan reproduces the placeholder's NULLs: nulled by the scan's own outer joins only (the
 * rows of the preserved side nothing matched are the scan's null input, where the expression is NULL too), or never nulled. NULL otherwise.
 */
Node *
placeholder_expr(AggCtx *c, PlaceHolderVar *ph)
{
	Relids		nulling = placeholder_nulling(c, ph);

	if (ph->phlevelsup != 0 || !bms_is_subset(nulling, c->nulling) || reads_full_lookup(c, (Node *) ph->phexpr))
		return NULL;
	return (Node *) ph->phexpr;
}

/*
 * A placeholder that stands for a strict expression of the scanned table's columns (or of its unnested function's) is that expression: where the
 * scan's outer join nulls the placeholder, those columns are NULL and so is the expression. A constant, a non-strict expression (coalesce, CASE)
 * or a volatile one keeps the placeholder.
 */
static Node *
strict_placeholders_mutator(Node *n, AggCtx *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
	{
		Node	   *e = placeholder_expr(c, (PlaceHolderVar *) n);
		Relids		rels = e ? pull_varnos(c->root, e) : NULL;

		if (e != NULL && (bms_is_member(c->relid, rels) || (c->func_rti && bms_is_member(c->func_rti, rels))) &&
			!contain_nonstrict_functions(e) && !contain_volatile_functions(e))
			return strict_placeholders_mutator(e, c);
	}
	return expression_tree_mutator(n, strict_placeholders_mutator, c);
}

Node *
strict_placeholders(AggCtx *c, Node *n)
{
	return strict_placeholders_mutator(n, c);
}

/* A placeholder computed inside the nullable input becomes NULL when that whole input is absent. */
static Node *
nullable_expr_mutator(Node *n, AggCtx *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
	{
		PlaceHolderVar *ph = (PlaceHolderVar *) n;
		Relids		nulling = placeholder_nulling(c, ph);

		/* the missing input is the scanned table's: a placeholder over a FULL join's table stays, and the plan with it is refused */
		if (ph->phlevelsup == 0 && !bms_is_empty(nulling) && bms_is_subset(nulling, c->nulling) && !reads_full_lookup(c, (Node *) ph->phexpr))
		{
			CaseExpr   *out = makeNode(CaseExpr);
			CaseWhen   *when = makeNode(CaseWhen);
			Param	   *missing = exec_param(SIFT_AGG_NULL_INPUT, BOOLOID, -1, InvalidOid);

			missing->location = SIFT_AGG_NULL_INPUT_LOC;
			when->expr = (Expr *) missing;
			when->result = (Expr *) makeNullConst(exprType(n), exprTypmod(n), exprCollation(n));
			when->location = -1;
			out->casetype = exprType(n);
			out->casecollid = exprCollation(n);
			out->args = list_make1(when);
			out->defresult = (Expr *) nullable_expr_mutator((Node *) ph->phexpr, c);
			out->location = -1;
			return (Node *) out;
		}
		/* a group expression of the preserved side is its expression */
		if (ph->phlevelsup == 0 && bms_is_empty(nulling) && group_wraps(c))
			return nullable_expr_mutator((Node *) ph->phexpr, c);
	}
	return expression_tree_mutator(n, nullable_expr_mutator, c);
}

Node *
normalize_expr(AggCtx *c, Node *n)
{
	/* a group expression that came from a pulled-up sub-select is a placeholder the grouping step nulls: with no join to null it, it is its expression */
	if (c->nulling || group_wraps(c))
		n = nullable_expr_mutator(n, c);
	if (c->nulling)
		n = remove_nulling_relids(n, c->nulling, NULL);
	/* the group expressions of grouping sets are marked as nulled by the grouping step (Vars with the RTE_GROUP in varnullingrels) */
	if (group_rti(c) > 0)
		n = remove_nulling_relids(n, bms_make_singleton(group_rti(c)), NULL);
	return jsonb_int_mutator(nulltest_mutator(exists_mutator(n, c), c), c);
}
