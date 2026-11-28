/*
 * agg/unnest/path.c - jsonpath filters and containment over the objects of an array column as the quals of a test of the scan of an unnest (agg/unnest/path.h).
 */
/* check-memory: allow-allocs every list here follows the text of one statement, not the rows of the table */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/namespace.h"
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

static bool
exists_call(Oid fid)
{
	return fid == F_JSONB_EXISTS || fid == F_JSONB_EXISTS_ANY || fid == F_JSONB_EXISTS_ALL;
}

static bool
contains_call(Oid fid)
{
	return fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED;
}

/* the constant array of a containment, NULL for another pattern */
static JsonbContainer *
pattern_array(const Node *e)
{
	List	   *args = func_args(unconstify(Node *, e));
	const Node *rhs = strip_relabel(call_func(e) == F_JSONB_CONTAINED ? linitial(args) : lsecond(args));
	Jsonb	   *pat;

	if (!IsA(rhs, Const) || ((const Const *) rhs)->constisnull)
		return NULL;
	pat = DatumGetJsonbP(((const Const *) rhs)->constvalue);
	return JsonContainerIsArray(&pat->root) && !JsonContainerIsScalar(&pat->root) && JsonContainerSize(&pat->root) > 0 ? &pat->root : NULL;
}

bool
sift_v6_unnest_path_call(const Node *e, bool table)
{
	const Oid	fid = call_func(e);

	return fid == F_JSONB_PATH_EXISTS || fid == F_JSONB_PATH_EXISTS_OPR || (contains_call(fid) && (table || pattern_array(e) != NULL)) || exists_call(fid);
}

/* the strings of a containment pattern that is a list of strings, NIL for another pattern */
static List *
pattern_strings(const Node *e)
{
	JsonbContainer *arr = contains_call(call_func(e)) ? pattern_array(e) : NULL;
	JsonbIterator *it;
	JsonbValue	v;
	List	   *out = NIL;

	if (arr == NULL)
		return NIL;
	it = JsonbIteratorInit(arr);
	(void) JsonbIteratorNext(&it, &v, true);
	while (JsonbIteratorNext(&it, &v, true) == WJB_ELEM)
	{
		if (v.type != jbvString)
			return NIL;
		out = lappend(out, cstring_to_text_with_len(v.val.string.val, v.val.string.len));
	}
	return out;
}

SiftV6ElemFn
sift_v6_unnest_path_fn(const Node *e)
{
	return exists_call(call_func(e)) || pattern_strings(e) != NIL ? SV6E_ELEMS_TEXT : SV6E_PATH;
}

/* the array whose elements the filter of a jsonpath tests: $[*] of the document, or $.key[*] of doc -> 'key', which is the array on a document that is an object (the call decides any other row) */
static bool
path_head(const JsonPath *jp, JsonPathItem *any, char **key)
{
	JsonPathItem root,
				item;
	int32		len;

	jspInit(&root, unconstify(JsonPath *, jp));
	*key = NULL;
	if (root.type != jpiRoot || !jspGetNext(&root, any))
		return false;
	if (any->type == jpiKey)
	{
		*key = jspGetString(any, &len);
		*key = pnstrdup(*key, len);
		item = *any;
		if (!jspGetNext(&item, any))
			return false;
	}
	return any->type == jpiAnyArray;
}

static const JsonPath *
call_path(const Node *e)
{
	const Node *path = strip_relabel(lsecond(func_args(unconstify(Node *, e))));

	return IsA(path, Const) && !((const Const *) path)->constisnull ? DatumGetJsonPathP(((const Const *) path)->constvalue) : NULL;
}

bool
sift_v6_unnest_path_keyed(const Node *e)
{
	const JsonPath *jp = call_func(e) == F_JSONB_PATH_EXISTS || call_func(e) == F_JSONB_PATH_EXISTS_OPR ? call_path(e) : NULL;
	JsonPathItem any;
	char	   *key;

	return jp != NULL && path_head(jp, &any, &key) && key != NULL;
}

/* doc -> 'key' */
static Expr *
json_field(const Expr *doc, text *key)
{
	OpExpr	   *field = (OpExpr *) make_opclause(OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString("->")), JSONBOID, TEXTOID), JSONBOID, false, copyObject(unconstify(Expr *, doc)),
												 (Expr *) makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1, PointerGetDatum(key), false, false), InvalidOid, DEFAULT_COLLATION_OID);

	set_opfuncid(field);
	return (Expr *) field;
}

const Expr *
sift_v6_unnest_path_array(const Node *e, bool table)
{
	List	   *args;
	const JsonPath *jp;
	JsonPathItem any;
	char	   *key;

	if (!sift_v6_unnest_path_call(e, table))
		return NULL;
	args = func_args(unconstify(Node *, e));
	if (contains_call(call_func(e)) || exists_call(call_func(e)))
		return (const Expr *) strip_relabel(call_func(e) == F_JSONB_CONTAINED ? lsecond(args) : linitial(args));
	if ((jp = call_path(e)) == NULL || !path_head(jp, &any, &key))
		return NULL;
	return key == NULL ? (const Expr *) strip_relabel(linitial(args)) : json_field((const Expr *) strip_relabel(linitial(args)), cstring_to_text(key));
}

/* the containment clause doc @> '{..}' (constant <@ doc): its document side, the @> operator and its object pattern; NULL: no such clause */
static const Jsonb *
object_pattern(const Node *clause, const Expr **doc, Oid *opno)
{
	const Oid	fid = call_func(clause);
	List	   *args;
	const Node *pat;
	const Jsonb *jb;

	if (!IsA(clause, OpExpr) || !contains_call(fid))
		return NULL;
	args = ((const OpExpr *) clause)->args;
	*doc = (const Expr *) strip_relabel(fid == F_JSONB_CONTAINS ? linitial(args) : lsecond(args));
	*opno = fid == F_JSONB_CONTAINS ? ((const OpExpr *) clause)->opno : get_commutator(((const OpExpr *) clause)->opno);
	pat = strip_relabel(fid == F_JSONB_CONTAINS ? lsecond(args) : linitial(args));
	if (!IsA(pat, Const) || ((const Const *) pat)->constisnull)
		return NULL;
	jb = DatumGetJsonbP(((const Const *) pat)->constvalue);
	return JsonContainerIsObject(&jb->root) ? jb : NULL;
}

/* (doc -> 'key') @> 'array' */
static Expr *
member_contains(const Expr *doc, Oid opno, const JsonbValue *key, JsonbValue *array)
{
	OpExpr	   *test = (OpExpr *) make_opclause(opno, BOOLOID, false, json_field(doc, cstring_to_text_with_len(key->val.string.val, key->val.string.len)),
												(Expr *) makeConst(JSONBOID, -1, InvalidOid, -1, JsonbPGetDatum(JsonbValueToJsonb(array)), false, false), InvalidOid, InvalidOid);

	set_opfuncid(test);
	return (Expr *) test;
}

List *
sift_v6_unnest_path_members(const Node *clause, Expr **rest)
{
	const Expr *doc;
	Oid			opno;
	const Jsonb *pat = object_pattern(clause, &doc, &opno);
	SiftJsonbBuild st = {0};
	JsonbIterator *it;
	JsonbValue	k,
				v,
			   *obj;
	List	   *tests = NIL;

	if (pat == NULL)
		return NIL;
	it = JsonbIteratorInit(unconstify(JsonbContainer *, &pat->root));
	sift_jsonb_push(&st, JsonbIteratorNext(&it, &k, true), NULL);
	while (JsonbIteratorNext(&it, &k, true) == WJB_KEY)
	{
		Expr	   *test;

		(void) JsonbIteratorNext(&it, &v, true);
		test = v.type == jbvBinary && JsonContainerIsArray(v.val.binary.data) ? member_contains(doc, opno, &k, &v) : NULL;
		if (test != NULL && pattern_strings((const Node *) test) != NIL)
			tests = lappend(tests, test);
		else
		{
			sift_jsonb_push(&st, WJB_KEY, &k);
			sift_jsonb_push(&st, WJB_VALUE, &v);
		}
	}
	obj = sift_jsonb_push(&st, WJB_END_OBJECT, NULL);
	*rest = tests == NIL || obj->val.object.nPairs == 0 ? NULL :
		make_opclause(opno, BOOLOID, false, copyObject(unconstify(Expr *, doc)), (Expr *) makeConst(JSONBOID, -1, InvalidOid, -1, JsonbPGetDatum(JsonbValueToJsonb(obj)), false, false),
					  InvalidOid, InvalidOid);
	if (*rest != NULL)
		set_opfuncid((OpExpr *) *rest);
	return tests;
}

/* ---- one comparison of a member with a literal, as the kinds and values of the member it holds ---- */

static bool
numeric_int(Numeric num, int64 *out)
{
	bool		err = false;

	*out = sift_numeric_int8(num, &err);
	return !err && DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(num), NumericGetDatum(int64_to_numeric(*out))));
}

/*
 * The literal is of the JSON type of a leaf of the type, and one this leaf holds as it is (an integer in its range): its comparisons are two-valued, as the quals need. An astext leaf holds the text of
 * numbers and booleans too, whose comparison with a string is unknown: only a string that none of them prints as is equal to the value of the leaf exactly when the member is that string.
 */
static bool
lit_fits(Oid type, bool astext, const Lit *lit, int op, bool neg, int64 *iv)
{
	const bool	eq_only = op == SCMP_EQ || op == SCMP_NE;

	switch (lit->kind)
	{
		case 'z':
			return eq_only;
		case 's':
			return type == TEXTOID && (astext ? op == SCMP_EQ && !neg && sift_v6_json_text_unambiguous(pnstrdup(lit->s, lit->len)) : eq_only);
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
	bool		astext = false;
	const Oid	type = sift_v6_unnest_member_type(c->p, c->rte, key, &astext);
	int64		iv = 0;
	int			vcol,
				kcol;
	Datum		value;

	if (!OidIsValid(type) || !lit_fits(type, astext, lit, op, neg, &iv) || (vcol = sift_v6_unnest_rte_col(c->rte, key, type, false)) < 0 || (kcol = sift_v6_unnest_rte_col(c->rte, key, INT8OID, true)) < 0)
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

/*
 * jsonb_path_exists(doc, '$[*] ? (filter)'[, vars[, silent]]) and doc @? '$[*] ? (filter)', or '$.key[*] ? (filter)' of the array doc -> 'key'. A strict path that is not silent raises for a
 * document without the key, whose leaves hold no array as for a NULL one.
 */
static List *
path_clauses(PathCtx *c, const Node *e)
{
	List	   *args = func_args(unconstify(Node *, e));
	const JsonPath *jp = call_path(e);
	JsonPathItem any,
				filt,
				expr;
	char	   *key;
	bool		silent = true;

	if (call_func(e) == F_JSONB_PATH_EXISTS)
	{
		const Node *arg = list_length(args) == 4 ? strip_relabel(lfourth(args)) : NULL;

		if (arg == NULL || !IsA(arg, Const) || ((const Const *) arg)->constisnull)
			return NIL;
		silent = DatumGetBool(((const Const *) arg)->constvalue);
		c->vars = jsonpath_vars(lthird(args), &c->ok);
	}
	if (!c->ok || jp == NULL)
		return NIL;
	c->strict = !(jp->header & JSONPATH_LAX);
	if (!path_head(jp, &any, &key) || (c->strict && key != NULL && !silent) || !jspGetNext(&any, &filt) || filt.type != jpiFilter || jspHasNext(&filt))
		return NIL;
	jspGetArg(&filt, &expr);
	return filter_cnf(c, &expr, false);
}

/* the object obj of the pattern of a containment: the element that contains it, whose members all have the value, one clause for each (NIL: none the leaves decide) */
static List *
object_clauses(PathCtx *c, JsonbContainer *obj)
{
	JsonbIterator *it = JsonbIteratorInit(obj);
	JsonbValue	v,
				key;
	List	   *out = NIL;

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

/* doc @> '[{"k": v, ..}, ..]': each object of the pattern is contained by some element of the array, the same or another one: the clauses of each object are one part */
static List *
contains_parts(PathCtx *c, const Node *e)
{
	JsonbContainer *arr = pattern_array(e);
	JsonbIterator *it;
	JsonbValue	v;
	List	   *parts = NIL;

	if (arr == NULL || JsonContainerSize(arr) > SIFT_V6_XPARTS)
		return NIL;
	it = JsonbIteratorInit(arr);
	(void) JsonbIteratorNext(&it, &v, true);
	while (JsonbIteratorNext(&it, &v, true) == WJB_ELEM)
	{
		List	   *part = v.type == jbvBinary ? object_clauses(c, v.val.binary.data) : NIL;

		if (part == NIL)
			return NIL;
		parts = lappend(parts, part);
	}
	return parts;
}

/* the clauses of a part as its quals: a clause of several tests is their OR */
static List *
part_quals(const List *clauses)
{
	List	   *quals = NIL;

	foreach_ptr(List, clause, clauses)
		quals = lappend(quals, list_length(clause) == 1 ? linitial(clause) : makeBoolExpr(OR_EXPR, clause, -1));
	return quals;
}

/* the keys of doc ? 'key', doc ?| keys and doc ?& keys: NIL for no constant, no key or a NULL among them */
static List *
exists_keys(const Node *e)
{
	const Const *k = (const Const *) strip_relabel(lsecond(func_args(unconstify(Node *, e))));
	List	   *keys = NIL;
	Datum	   *el;
	bool	   *nl;
	int			n;

	if (!IsA(k, Const) || k->constisnull)
		return NIL;
	if (call_func(e) == F_JSONB_EXISTS)
		return list_make1(DatumGetPointer(k->constvalue));
	deconstruct_array_builtin(DatumGetArrayTypeP(k->constvalue), TEXTOID, &el, &nl, &n);
	for (int i = 0; i < n; i++)
	{
		if (nl[i])
			return NIL;
		keys = lappend(keys, DatumGetPointer(el[i]));
	}
	return keys;
}

/*
 * doc ? 'key', doc ?| keys, doc ?& keys and doc @> '["s", ..]' of a list of strings: an element is the string (the leaf holds the strings alone, not the text of other scalars as an astext leaf
 * does). Some element is one of the keys of ?|, the one part; every other string is a part of its own
 */
static List *
string_parts(PathCtx *c, const List *strings, bool any)
{
	bool		astext = false;
	List	   *preds = NIL;
	List	   *parts = NIL;

	if (strings == NIL || list_length(strings) > SIFT_V6_XPARTS || sift_v6_unnest_member_type(c->p, c->rte, NULL, &astext) != TEXTOID || astext)
		return NIL;
	foreach_ptr(void, s, strings)
		preds = lappend(preds, cmp_pred(0, TEXTOID, SCMP_EQ, PointerGetDatum(s)));
	if (any)
		return list_make1(list_make1(preds));
	foreach_ptr(Expr, pred, preds)
		parts = lappend(parts, list_make1(list_make1(pred)));
	return parts;
}

bool
sift_v6_unnest_path_quals(const V6Plan *p, SiftV6ElemRte *rte, const Node *e, List **parts)
{
	PathCtx		c = {.p = p,.rte = rte,.ok = true};
	const Oid	fid = call_func(e);
	List	   *clauses = exists_call(fid) ? string_parts(&c, exists_keys(e), fid == F_JSONB_EXISTS_ANY) :
		pattern_strings(e) != NIL ? string_parts(&c, pattern_strings(e), false) : contains_call(fid) ? contains_parts(&c, e) : list_make1(path_clauses(&c, e));

	if (!c.ok || clauses == NIL || list_member(clauses, NIL))
		return false;
	foreach_ptr(List, part, clauses)
		*parts = lappend(*parts, part_quals(part));
	return true;
}
