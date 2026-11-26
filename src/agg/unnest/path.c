/*
 * agg/unnest/path.c - jsonpath filters and containment over the objects of an array column as the quals of a test of the scan of an unnest (agg/unnest/path.h).
 */
/* check-memory: allow-allocs every list here follows the text of one statement, not the rows of the table */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_opfamily_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/kern/kern.h"
#include "agg/ext.h"
#include "agg/jsonpath.h"
#include "agg/unnest/path.h"

/* the clauses of the conjunction and the tests of a clause that the quals of one test may have: an OR of ANDs is their product */
#define PATH_CNF_CLAUSES	32
#define PATH_CNF_TERMS	16

typedef struct PathCtx
{
	const V6Plan *p;
	SiftV6ElemRte *rte;
	const Jsonb *vars;
	bool		strict;
	bool		ok;
} PathCtx;

static Oid
call_func(const Node *e)
{
	if (IsA(e, FuncExpr))
		return ((const FuncExpr *) e)->funcid;
	return IsA(e, OpExpr) ? get_opcode(((const OpExpr *) e)->opno) : InvalidOid;
}

bool
sift_v6_unnest_path_call(const Node *e)
{
	const Oid	fid = call_func(e);

	return fid == F_JSONB_PATH_EXISTS || fid == F_JSONB_PATH_EXISTS_OPR || fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED;
}

const Expr *
sift_v6_unnest_path_array(const Node *e)
{
	List	   *args;

	if (!sift_v6_unnest_path_call(e))
		return NULL;
	args = func_args(unconstify(Node *, e));
	return (const Expr *) strip_relabel(call_func(e) == F_JSONB_CONTAINED ? lsecond(args) : linitial(args));
}

/* ---- one comparison of a member with a literal, as the kinds and values of the member it holds ---- */

static bool
numeric_int(Numeric num, int64 *out)
{
	bool		err = false;

	*out = sift_numeric_int8(num, &err);
	return !err && DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(num), NumericGetDatum(int64_to_numeric(*out))));
}

/* the literal is of the JSON type of a leaf of the type, and one this leaf holds as it is (an integer in its range): its comparisons are two-valued, as the quals need */
static bool
lit_fits(Oid type, const Lit *lit, int op, int64 *iv)
{
	const bool	eq_only = op == SCMP_EQ || op == SCMP_NE;

	switch (lit->kind)
	{
		case 'z':
			return eq_only;
		case 's':
			return type == TEXTOID && eq_only;
		case 'b':
			return type == BOOLOID && eq_only;
		case 'n':
			return (type == INT2OID || type == INT4OID || type == INT8OID) && numeric_int(lit->num, iv) &&
				(type == INT8OID || (*iv >= (type == INT4OID ? PG_INT32_MIN : PG_INT16_MIN) && *iv <= (type == INT4OID ? PG_INT32_MAX : PG_INT16_MAX)));
		default:
			return false;
	}
}

static Datum
lit_datum(Oid type, const Lit *lit, int64 iv)
{
	switch (type)
	{
		case TEXTOID:
			return PointerGetDatum(cstring_to_text_with_len(lit->s, lit->len));
		case BOOLOID:
			return BoolGetDatum(lit->b);
		case INT2OID:
			return Int16GetDatum((int16) iv);
		case INT4OID:
			return Int32GetDatum((int32) iv);
		default:
			return Int64GetDatum(iv);
	}
}

static Oid
cmp_family(Oid type)
{
	return type == TEXTOID ? TEXT_BTREE_FAM_OID : type == BOOLOID ? BOOL_BTREE_FAM_OID : INTEGER_BTREE_FAM_OID;
}

/* the predicate `column op constant` over the element column col of the type */
static Expr *
cmp_pred(int col, Oid type, int op, Datum value)
{
	static const int strategy[] = {BTEqualStrategyNumber, 0, BTLessStrategyNumber, BTLessEqualStrategyNumber, BTGreaterStrategyNumber, BTGreaterEqualStrategyNumber};
	const Oid	family = cmp_family(type);
	const Oid	coll = type == TEXTOID ? DEFAULT_COLLATION_OID : InvalidOid;
	const Oid	eq = get_opfamily_member(family, type, type, BTEqualStrategyNumber);
	const Oid	opno = op == SCMP_NE ? get_negator(eq) : get_opfamily_member(family, type, type, strategy[op]);
	int16		len;
	bool		byval;
	OpExpr	   *e;

	get_typlenbyval(type, &len, &byval);
	e = (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) makeVar(INDEX_VAR, SIFT_V6_ELEM_VATT + col, type, -1, coll, 0), (Expr *) makeConst(type, -1, coll, len, value, false, byval), InvalidOid, coll);
	set_opfuncid(e);
	return (Expr *) e;
}

static Expr *
kind_pred(int kcol, int kind)
{
	return cmp_pred(kcol, INT8OID, SCMP_EQ, Int64GetDatum(kind));
}

static int
negate_op(int op)
{
	static const int not_op[] = {SCMP_NE, SCMP_EQ, SCMP_GE, SCMP_GT, SCMP_LE, SCMP_LT};

	return not_op[op];
}

static int
commute_op(int op)
{
	static const int swapped[] = {SCMP_EQ, SCMP_NE, SCMP_GT, SCMP_GE, SCMP_LT, SCMP_LE};

	return swapped[op];
}

/*
 * The tests of one element, an OR, that hold when `@.key op literal` is true (neg false) or false (neg true) of it. An element is a member the leaf holds (K = VALUE), a JSON null or missing; a comparison with
 * a literal is false for a missing key whichever the operator, for a JSON null it is true under != alone, and for a value it is the comparison of the leaf; null as the literal is equal to a JSON null only.
 */
static List *
atom_clause(PathCtx *c, const char *key, int op, const Lit *lit, bool neg)
{
	const Oid	type = sift_v6_unnest_member_type(c->p, c->rte, key);
	int64		iv = 0;
	int			vcol,
				kcol;
	Datum		value;

	if (!OidIsValid(type) || !lit_fits(type, lit, op, &iv) || (vcol = sift_v6_unnest_rte_col(c->rte, key, type, false)) < 0 || (kcol = sift_v6_unnest_rte_col(c->rte, key, INT8OID, true)) < 0)
	{
		c->ok = false;
		return NIL;
	}
	if (lit->kind == 'z')
	{
		if (op == SCMP_EQ)
			return neg ? list_make2(kind_pred(kcol, SIFT_ELEM_KIND_ABSENT), kind_pred(kcol, SIFT_ELEM_KIND_VALUE)) : list_make1(kind_pred(kcol, SIFT_ELEM_KIND_JNULL));
		return neg ? list_make2(kind_pred(kcol, SIFT_ELEM_KIND_ABSENT), kind_pred(kcol, SIFT_ELEM_KIND_JNULL)) : list_make1(kind_pred(kcol, SIFT_ELEM_KIND_VALUE));
	}
	value = lit_datum(type, lit, iv);
	if (op == SCMP_NE)
		return neg ? list_make2(kind_pred(kcol, SIFT_ELEM_KIND_ABSENT), cmp_pred(vcol, type, SCMP_EQ, value)) : list_make2(cmp_pred(vcol, type, SCMP_NE, value), kind_pred(kcol, SIFT_ELEM_KIND_JNULL));
	if (!neg)
		return list_make1(cmp_pred(vcol, type, op, value));
	return list_make3(kind_pred(kcol, SIFT_ELEM_KIND_ABSENT), kind_pred(kcol, SIFT_ELEM_KIND_JNULL), cmp_pred(vcol, type, negate_op(op), value));
}

/* ---- the filter: ! pushed to the comparisons, then the conjunction of ORs ---- */

/* the clauses of a || b: every clause of a with every clause of b */
static List *
or_product(PathCtx *c, List *a, List *b)
{
	List	   *out = NIL;

	foreach_ptr(List, ca, a)
		foreach_ptr(List, cb, b)
		{
			if (list_length(out) == PATH_CNF_CLAUSES || list_length(ca) + list_length(cb) > PATH_CNF_TERMS)
			{
				c->ok = false;
				return NIL;
			}
			out = lappend(out, list_concat_copy(ca, cb));
		}
	return out;
}

static List *
filter_cnf(PathCtx *c, JsonPathItem *v, bool neg)
{
	JsonPathItem l,
				r;
	List	   *a,
			   *b;
	int			op;
	char	   *key;
	Lit			lit;

	if (jspHasNext(v) || (v->type == jpiNot && c->strict))
	{
		c->ok = false;
		return NIL;
	}
	if (v->type == jpiNot)
	{
		jspGetArg(v, &l);
		return filter_cnf(c, &l, !neg);
	}
	op = jsonpath_cmp_op(v->type);
	if (op < 0 && v->type != jpiAnd && v->type != jpiOr)
	{
		c->ok = false;
		return NIL;
	}
	jspGetLeftArg(v, &l);
	jspGetRightArg(v, &r);
	if (op < 0)
	{
		a = filter_cnf(c, &l, neg);
		b = c->ok ? filter_cnf(c, &r, neg) : NIL;
		if (!c->ok)
			return NIL;
		return (v->type == jpiAnd) != neg ? list_concat(a, b) : or_product(c, a, b);
	}
	if ((key = jsonpath_key(&l)) != NULL && jsonpath_lit(&r, c->vars, &lit))
		;
	else if ((key = jsonpath_key(&r)) != NULL && jsonpath_lit(&l, c->vars, &lit))
		op = commute_op(op);
	else
	{
		c->ok = false;
		return NIL;
	}
	a = atom_clause(c, key, op, &lit, neg);
	return c->ok ? list_make1(a) : NIL;
}

/* jsonb_path_exists(doc, '$[*] ? (filter)'[, vars[, silent]]) and doc @? '$[*] ? (filter)' */
static List *
path_clauses(PathCtx *c, const Node *e)
{
	List	   *args = func_args(unconstify(Node *, e));
	const Node *path = strip_relabel(lsecond(args));
	JsonPathItem root,
				any,
				filt,
				expr;
	JsonPath   *jp;

	if (call_func(e) == F_JSONB_PATH_EXISTS)
	{
		const Node *silent = list_length(args) == 4 ? strip_relabel(lfourth(args)) : NULL;

		if (silent == NULL || !IsA(silent, Const))
			return NIL;
		c->vars = jsonpath_vars(lthird(args), &c->ok);
	}
	if (!c->ok || !IsA(path, Const) || ((const Const *) path)->constisnull)
		return NIL;
	jp = DatumGetJsonPathP(((const Const *) path)->constvalue);
	jspInit(&root, jp);
	c->strict = !(jp->header & JSONPATH_LAX);
	if (root.type != jpiRoot || !jspGetNext(&root, &any) || any.type != jpiAnyArray || !jspGetNext(&any, &filt) || filt.type != jpiFilter || jspHasNext(&filt))
		return NIL;
	jspGetArg(&filt, &expr);
	return filter_cnf(c, &expr, false);
}

/* doc @> '[{"k": v, ..}]': the element of the array that contains the object, whose members all have the value: one clause for each */
static List *
contains_clauses(PathCtx *c, const Node *e)
{
	List	   *args = func_args(unconstify(Node *, e));
	const Node *rhs = strip_relabel(call_func(e) == F_JSONB_CONTAINED ? linitial(args) : lsecond(args));
	JsonbIterator *it;
	JsonbValue	v,
				key;
	List	   *out = NIL;

	if (!IsA(rhs, Const) || ((const Const *) rhs)->constisnull)
		return NIL;
	it = JsonbIteratorInit(&DatumGetJsonbP(((const Const *) rhs)->constvalue)->root);
	if (JsonbIteratorNext(&it, &v, true) != WJB_BEGIN_ARRAY || v.val.array.rawScalar || v.val.array.nElems != 1 || JsonbIteratorNext(&it, &v, true) != WJB_ELEM || v.type != jbvBinary)
		return NIL;
	it = JsonbIteratorInit(v.val.binary.data);
	if (JsonbIteratorNext(&it, &v, true) != WJB_BEGIN_OBJECT)
		return NIL;
	while (JsonbIteratorNext(&it, &key, true) == WJB_KEY)
	{
		Lit			lit;
		List	   *clause;

		if (JsonbIteratorNext(&it, &v, true) != WJB_VALUE || !jsonb_lit(&v, &lit))
			return NIL;
		clause = atom_clause(c, pnstrdup(key.val.string.val, key.val.string.len), SCMP_EQ, &lit, false);
		if (!c->ok)
			return NIL;
		out = lappend(out, clause);
	}
	return out;
}

bool
sift_v6_unnest_path_quals(const V6Plan *p, SiftV6ElemRte *rte, const Node *e, List **quals)
{
	PathCtx		c = {.p = p,.rte = rte,.ok = true};
	const Oid	fid = call_func(e);
	List	   *clauses = fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED ? contains_clauses(&c, e) : path_clauses(&c, e);

	if (!c.ok || clauses == NIL)
		return false;
	foreach_ptr(List, clause, clauses)
		*quals = lappend(*quals, list_length(clause) == 1 ? linitial(clause) : makeBoolExpr(OR_EXPR, clause, -1));
	return true;
}
