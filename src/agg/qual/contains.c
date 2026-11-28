/*
 * agg/qual/contains.c - jsonb containment and equality as tests of the leaves of the paths of a document of an index of format major 2 or of a sift_table (agg/qual/contains.h).
 *
 * doc @> '{"k": 1, "o": {"s": "x"}}' holds exactly where every scalar member of the constant is the value at its path: a string or a boolean is a leaf of that JSON type whose `->>` text is the
 * literal's, an integer is the one an integer leaf holds (the leaf holds every JSON number equal to an integer of its range, 3.0 as 3, and no other), a null is a leaf of type null. A leaf has a
 * value only where every key of its path is a member of an object, so the tests also say that the document is one. NOT (doc @> ..) is false for a SQL NULL document and the complement for the others.
 */
/* check-memory: allow-allocs every list here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_opfamily_d.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/qual/contains.h"
#include "col/col.h"
#include "col/am.h"
#include "core/compat.h"

typedef struct Lower
{
	const SiftV6Rel *rel;
	int			att;			/* the column of the index, or the attribute of the table, the document is */
	List	   *tests;
} Lower;

static const Expr *
strip(const Expr *e)
{
	while (e != NULL && IsA(e, RelabelType))
		e = ((const RelabelType *) e)->arg;
	return e;
}

static const Jsonb *
jsonb_const(const Expr *e)
{
	const Const *c = (const Const *) strip(e);

	return c != NULL && IsA(c, Const) && !c->constisnull && c->consttype == JSONBOID ? DatumGetJsonbP(c->constvalue) : NULL;
}

/* the shredder's text of the path of a leaf below the attribute name: name.k1.k2; NULL for a key no path names */
static char *
path_text(const char *name, const List *path)
{
	StringInfoData b;

	initStringInfo(&b);
	appendStringInfoString(&b, name);
	foreach_node(String, k, path)
	{
		if (strpbrk(strVal(k), ".[]*#") != NULL)
			return NULL;
		appendStringInfo(&b, ".%s", strVal(k));
	}
	return b.data;
}

/* the scalar leaf of one value a row at the path of the document (-1: none) */
static int
path_leaf(const Lower *l, const List *path)
{
	const SiftColSchema *sch = sift_v6_rel_leaves(l->rel);

	for (int c = 0; c < sch->ncols; c++)
	{
		const SiftColDef *d = &sch->cols[c];
		const char *text = d->attno == l->att && d->name != NULL && d->kind == SIFT_COLK_SCALAR && d->parent < 0 && sift_col_def_has_kinds(d) ? path_text(d->name, path) : NULL;

		if (text != NULL && strcmp(d->path, text) == 0)
			return c;
	}
	return -1;
}

static Var *
vatt(const Lower *l, int base, int leaf, Oid typid)
{
	return makeVar(l->rel->relid, base + leaf, typid, -1, typid == TEXTOID ? C_COLLATION_OID : InvalidOid, 0);
}

static Expr *
equals(Var *v, Datum value)
{
	const Oid	opno = v->vartype == TEXTOID ? TextEqualOperator : get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, BTEqualStrategyNumber);
	OpExpr	   *e = (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) v, (Expr *) makeConst(v->vartype, -1, v->varcollid, v->vartype == TEXTOID ? -1 : 8, value, false,
																									 v->vartype != TEXTOID), InvalidOid, v->varcollid);

	set_opfuncid(e);
	return (Expr *) e;
}

static void
type_test(Lower *l, int leaf, const char *type)
{
	l->tests = lappend(l->tests, equals(vatt(l, SIFT_V6_JTYPE_VATT, leaf, TEXTOID), CStringGetTextDatum(type)));
}

/* an integer of the range of the integer leaf d */
static bool
leaf_int(const SiftColDef *d, Numeric num, int64 *out)
{
	bool		err = false;
	const int64 lim = d->type == SIFT_COLT_INT2 ? PG_INT16_MAX : d->type == SIFT_COLT_INT4 ? PG_INT32_MAX : PG_INT64_MAX;

	*out = sift_numeric_int8(num, &err);
	return !err && *out >= -lim - 1 && *out <= lim && DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(num), NumericGetDatum(int64_to_numeric(*out))));
}

/* the value at the path is the scalar v */
static bool
lower_scalar(Lower *l, const List *path, const JsonbValue *v)
{
	const int	leaf = path_leaf(l, path);
	const SiftColDef *d = leaf >= 0 ? &sift_v6_rel_leaves(l->rel)->cols[leaf] : NULL;
	const bool	text_leaf = d != NULL && (d->store == SIFT_COLS_STR || (d->store == SIFT_COLS_INT && d->type == SIFT_COLT_BOOL));
	int64		iv;

	if (d == NULL)
		return false;
	switch (v->type)
	{
		case jbvNull:
			type_test(l, leaf, "null");
			return true;
		case jbvString:
		case jbvBool:
			if (!text_leaf)
				return false;
			type_test(l, leaf, v->type == jbvString ? "string" : "boolean");
			l->tests = lappend(l->tests, equals(vatt(l, SIFT_V6_JTEXT_VATT, leaf, TEXTOID), v->type == jbvString ?
												PointerGetDatum(cstring_to_text_with_len(v->val.string.val, v->val.string.len)) : CStringGetTextDatum(v->val.boolean ? "true" : "false")));
			return true;
		case jbvNumeric:
			if (d->store != SIFT_COLS_INT || (d->type != SIFT_COLT_INT2 && d->type != SIFT_COLT_INT4 && d->type != SIFT_COLT_INT8) || !leaf_int(d, v->val.numeric, &iv))
				return false;
			l->tests = lappend(l->tests, equals(vatt(l, SIFT_V6_JHELD_VATT, leaf, INT8OID), Int64GetDatum(iv)));
			return true;
		default:
			return false;
	}
}

/* every member of the object obj is contained by the value at its path below path: an empty object or an array is no test of the leaves */
static bool
lower_object(Lower *l, const List *path, JsonbContainer *obj)
{
	JsonbIterator *it = JsonbIteratorInit(obj);
	JsonbValue	k,
				v;

	if (JsonContainerSize(obj) == 0 || JsonbIteratorNext(&it, &k, true) != WJB_BEGIN_OBJECT)
		return false;
	while (JsonbIteratorNext(&it, &k, true) == WJB_KEY)
	{
		List	   *at = lappend(list_copy(path), makeString(pnstrdup(k.val.string.val, k.val.string.len)));

		(void) JsonbIteratorNext(&it, &v, true);
		if (v.type == jbvBinary ? !JsonContainerIsObject(v.val.binary.data) || !lower_object(l, at, v.val.binary.data) : !lower_scalar(l, at, &v))
			return false;
	}
	return true;
}

/* the document of the index that e is, or the chain of keys of one below it */
static bool
doc_path(Lower *l, const Expr *e, List **path)
{
	const Var  *doc = sift_v6_doc_chain(e, path);

	return doc != NULL && sift_v6_json_doc_att(l->rel, (const Expr *) doc, &l->att);
}

/* lhs @> rhs, lhs being the document or a path below it, rhs a constant object */
static bool
lower_contains(Lower *l, const Expr *lhs, const Expr *rhs)
{
	const Jsonb *c = jsonb_const(rhs);
	List	   *path;

	return c != NULL && JsonContainerIsObject(&c->root) && doc_path(l, lhs, &path) && lower_object(l, path, (JsonbContainer *) &c->root);
}

/* chain = constant (or commuted), chain a path of at least one key below the document and the constant a scalar */
static bool
lower_equal(Lower *l, const OpExpr *op)
{
	const Expr *lhs = linitial(op->args),
			   *rhs = lsecond(op->args);
	JsonbValue	v;
	List	   *path;

	if (jsonb_const(lhs) != NULL)
	{
		lhs = lsecond(op->args);
		rhs = linitial(op->args);
	}
	return jsonb_const(rhs) != NULL && JsonbExtractScalar((JsonbContainer *) &jsonb_const(rhs)->root, &v) && doc_path(l, lhs, &path) && path != NIL && lower_scalar(l, path, &v);
}

static bool
lower_positive(Lower *l, const Expr *clause)
{
	const OpExpr *op = (const OpExpr *) strip(clause);

	if (!IsA(op, OpExpr) || list_length(op->args) != 2)
		return false;
	switch (get_opcode(op->opno))
	{
		case F_JSONB_CONTAINS:
			return lower_contains(l, linitial(op->args), lsecond(op->args));
		case F_JSONB_CONTAINED:
			return lower_contains(l, lsecond(op->args), linitial(op->args));
		case F_JSONB_EQ:
			return lower_equal(l, op);
		default:
			return false;
	}
}

/* the presence leaf of the document, NULL where it is SQL NULL, which a scan reads as a column wherever the test is; the document itself when the index has none */
static Expr *
doc_present(const Lower *l, const Var *doc)
{
	const SiftColSchema *sch = sift_v6_rel_leaves(l->rel);

	for (int c = 0; c < sch->ncols; c++)
		if (sch->cols[c].attno == l->att && sift_col_def_is_present(&sch->cols[c]))
			return (Expr *) vatt(l, SIFT_V6_JHELD_VATT, c, INT8OID);
	return (Expr *) copyObject(doc);
}

/* (doc @> c) IS NOT TRUE: the tests not all true (a test of a missing key, or of a SQL NULL document, is NULL) */
static bool
lower_none(Lower *l, const Expr *clause)
{
	BooleanTest *none = makeNode(BooleanTest);

	if (!lower_positive(l, clause))
		return false;
	*none = (BooleanTest) {.xpr.type = T_BooleanTest,.arg = make_ands_explicit(l->tests),.booltesttype = IS_NOT_TRUE,.location = -1};
	l->tests = list_make1(none);
	return true;
}

/* NOT (doc @> c), or (doc @> c) IS FALSE: doc IS NOT NULL and the tests not all true */
static bool
lower_negated(Lower *l, const Expr *clause)
{
	const OpExpr *op = (const OpExpr *) strip(clause);
	const Oid	fid = IsA(op, OpExpr) && list_length(op->args) == 2 ? get_opcode(op->opno) : InvalidOid;
	const Expr *lhs = fid == F_JSONB_CONTAINED ? strip(lsecond(op->args)) : fid == F_JSONB_CONTAINS ? strip(linitial(op->args)) : NULL;
	NullTest   *nt;

	if (lhs == NULL || !IsA(lhs, Var) || !lower_none(l, clause))
		return false;
	if (bms_is_member(((const Var *) lhs)->varattno, l->rel->notnull))
		return true;
	nt = makeNode(NullTest);
	*nt = (NullTest) {.xpr.type = T_NullTest,.arg = doc_present(l, (const Var *) lhs),.nulltesttype = IS_NOT_NULL,.location = -1};
	l->tests = lappend(l->tests, nt);
	return true;
}

/* the containment, its NOT, or IS TRUE, IS FALSE or IS NOT TRUE of it */
static bool
lower_clause(Lower *l, const Expr *clause)
{
	const Expr *e = strip(clause);
	const BooleanTest *bt = IsA(e, BooleanTest) ? (const BooleanTest *) e : NULL;

	if (is_notclause(e))
		return lower_negated(l, get_notclausearg(e));
	if (bt == NULL)
		return lower_positive(l, e);
	switch (bt->booltesttype)
	{
		case IS_TRUE:
			return lower_positive(l, bt->arg);
		case IS_FALSE:
			return lower_negated(l, bt->arg);
		case IS_NOT_TRUE:
			return lower_none(l, bt->arg);
		default:
			return false;
	}
}

List *
sift_v6_contains_quals(const SiftV6Rel *rel, const Expr *clause)
{
	Lower		l = {.rel = rel};

	if (sift_v6_rel_leaves(rel) == NULL || !sift_v6_values_exact(rel) || !rel->ops->jsonb->exact_values)
		return NIL;
	return lower_clause(&l, clause) ? l.tests : NIL;
}

Node *
sift_v6_null_test_folded(const SiftV6Rel *rel, const Node *n)
{
	const NullTest *nt = IsA(n, NullTest) ? (const NullTest *) n : NULL;
	const BooleanTest *bt = IsA(n, BooleanTest) ? (const BooleanTest *) n : NULL;
	NullTest   *flip;

	if (nt != NULL && !nt->argisrow && IsA(nt->arg, Var) && ((const Var *) nt->arg)->varno == rel->relid && ((const Var *) nt->arg)->varlevelsup == 0 &&
		bms_is_empty(((const Var *) nt->arg)->varnullingrels) && bms_is_member(((const Var *) nt->arg)->varattno, rel->notnull))
		return (Node *) makeBoolConst(nt->nulltesttype == IS_NOT_NULL, false);
	if (bt == NULL || !IsA(bt->arg, NullTest))
		return NULL;
	if (bt->booltesttype == IS_TRUE || bt->booltesttype == IS_NOT_FALSE)
		return copyObject((Node *) bt->arg);
	flip = copyObject((NullTest *) bt->arg);
	flip->nulltesttype = flip->nulltesttype == IS_NULL ? IS_NOT_NULL : IS_NULL;
	return (Node *) flip;
}

bool
sift_v6_json_payload_member(const char *payload, uint32 len, const List *keys, MemoryContext cx, const char **out, uint32 *outlen)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	Jsonb	   *jb = memcpy(palloc(len), payload, len);

	foreach_node(String, k, keys)
	{
		JsonbValue	key = {.type = jbvString,.val.string = {.val = strVal(k),.len = strlen(strVal(k))}};
		JsonbValue *v = JB_ROOT_IS_OBJECT(jb) ? findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &key) : NULL;

		jb = v != NULL ? JsonbValueToJsonb(v) : NULL;
		if (jb == NULL)
			break;
	}
	MemoryContextSwitchTo(old);
	*out = (const char *) jb;
	*outlen = jb != NULL ? VARSIZE(jb) : 0;
	return jb != NULL;
}
