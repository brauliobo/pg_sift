/*
 * agg/qual/jsonb.c - the keys of the jsonb documents of an index of format major 2 as the aggregate scan of SiftAggV6 reads them: the leaf of a top-level key (col/f2/rel.c) says on which rows
 * `doc ? 'k'` holds and of which JSON type the value is (jsonb_typeof), and holds the value of the rows that are of its type; the other rows keep their value as the payload of an exception.
 */
/* check-memory: allow-allocs every array here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/agg.h"
#include "agg/ht/ht.h"
#include "agg/engine.h"
#include "agg/qual/contains.h"
#include "agg/qual/qual.h"
#include "agg/unnest/jsonb.h"
#include "col/am.h"
#include "core/errctx.h"

#define JQ_EXISTS	1
#define JQ_TYPE		2

static const Expr *
strip(const Expr *e)
{
	while (e != NULL && IsA(e, RelabelType))
		e = ((const RelabelType *) e)->arg;
	return e;
}

static Oid
func_of(const Expr *e)
{
	return IsA(e, OpExpr) ? get_opcode(((const OpExpr *) e)->opno) : IsA(e, FuncExpr) ? ((const FuncExpr *) e)->funcid : InvalidOid;
}

static List *
args_of(const Expr *e)
{
	return IsA(e, OpExpr) ? ((const OpExpr *) e)->args : IsA(e, FuncExpr) ? ((const FuncExpr *) e)->args : NIL;
}

static const char *
text_const(const Expr *e)
{
	const Const *c = (const Const *) strip(e);

	return c != NULL && IsA(c, Const) && !c->constisnull && c->consttype == TEXTOID ? TextDatumGetCString(c->constvalue) : NULL;
}

const SiftColSchema *
sift_v6_rel_leaves(const SiftV6Rel *rel)
{
	return rel->table != NULL ? rel->table->leaves : rel->colrel != NULL ? rel->colrel->schema : NULL;
}

/* The column of the index (1-based) that holds the jsonb attribute e is, and is shredded; of a table, the attribute that has the leaves of declared paths */
static bool
doc_of(const SiftV6Rel *rel, const Expr *e, int *att)
{
	const Var  *v = (const Var *) strip(e);

	if (v == NULL || !IsA(v, Var) || v->varno != rel->relid || v->varlevelsup != 0 || v->vartype != JSONBOID)
		return false;
	if (rel->table != NULL)
	{
		*att = v->varattno;
		return v->varattno > 0 && v->varattno <= rel->table->natts && rel->table->att_nleaves[v->varattno] > 1;
	}
	for (int i = 0; i < rel->index->ncolumns; i++)
		if (rel->index->indexkeys[i] == v->varattno)
		{
			*att = i + 1;
			return rel->colrel->json[i + 1];
		}
	return false;
}

bool
sift_v6_json_doc_att(const SiftV6Rel *rel, const Expr *e, int *att)
{
	return doc_of(rel, e, att);
}

/* the leaf of the format of the relation at the path of the document attribute att (-1: none) */
static int
path_leaf(const SiftV6Rel *rel, int att, const List *path)
{
	const SiftJsonbOps *jsonb = rel->ops != NULL ? rel->ops->jsonb : NULL;

	return jsonb == NULL ? -1 : jsonb->key_leaf(rel->table != NULL ? rel->reloid : rel->index->indexoid, rel->table != NULL, att, path);
}

/* the leaf of the format of the relation that keeps whether the key is on the rows of the document attribute att (-1: none keeps it) */
static int
key_leaf(const SiftV6Rel *rel, int att, const char *key)
{
	return path_leaf(rel, att, list_make1(makeString(pstrdup(key))));
}

/* the format tests whether the key is on a document with the leaf of the key; the leaves of a sift_table keep no presence of their key (a document that is no object and holds it) */
static bool
exists_leaf(const SiftV6Rel *rel, int att, const char *key)
{
	return rel->table == NULL && rel->ops != NULL && rel->ops->jsonb != NULL && rel->ops->jsonb->key_exists && key_leaf(rel, att, key) >= 0;
}

/* the empty path '{}' of a #>> */
static bool
empty_path(const Expr *e)
{
	const Const *c = (const Const *) strip(e);

	return c != NULL && IsA(c, Const) && !c->constisnull && c->consttype == TEXTARRAYOID && ARR_NDIM(DatumGetArrayTypeP(c->constvalue)) == 0;
}

/* the one key of a path of one element: '{k}' of #> and #>>, or the k of the variadic path of jsonb_extract_path(doc, 'k') and jsonb_extract_path_text(doc, 'k') */
static const char *
path_key(const Expr *e)
{
	const Expr *a = strip(e);
	const Const *c = (const Const *) a;
	Datum	   *el;
	bool	   *nl;
	int			n;

	if (a != NULL && IsA(a, ArrayExpr) && list_length(((const ArrayExpr *) a)->elements) == 1)
		return text_const(linitial(((const ArrayExpr *) a)->elements));
	if (a == NULL || !IsA(a, Const) || c->constisnull || c->consttype != TEXTARRAYOID || ARR_NDIM(DatumGetArrayTypeP(c->constvalue)) != 1)
		return NULL;
	deconstruct_array_builtin(DatumGetArrayTypeP(c->constvalue), TEXTOID, &el, &nl, &n);
	return n == 1 && !nl[0] ? TextDatumGetCString(el[0]) : NULL;
}

/* a key that a path or a subscript reads as the key of an object whatever the document is: one an array would read as the index of an element is not */
static bool
object_key(const char *key)
{
	return key != NULL && strchr(" +-0123456789", key[strspn(key, " ")]) == NULL;
}

/* doc -> 'k' as a call with a path of one key: jsonb_extract_path(doc, 'k') or doc #> '{k}' (as_text false), jsonb_extract_path_text(doc, 'k') or doc #>> '{k}' (as_text true) */
static bool
path_member(const Expr *e, bool as_text, const Var **doc, const char **key)
{
	const List *args = e != NULL ? args_of(e) : NIL;

	if (e == NULL || func_of(e) != (as_text ? F_JSONB_EXTRACT_PATH_TEXT : F_JSONB_EXTRACT_PATH) || list_length(args) != 2 || !object_key(*key = path_key(lsecond(args))))
		return false;
	*doc = (const Var *) strip(linitial(args));
	return *doc != NULL && IsA(*doc, Var);
}

/* the keys of doc['k1']['k2'].. that reads the members of objects (doc -> 'k1' -> 'k2'), NIL for any other subscript */
static List *
subscript_keys(const Expr *e)
{
	const SubscriptingRef *sr = (const SubscriptingRef *) e;
	List	   *keys = NIL;

	if (sr->refcontainertype != JSONBOID || sr->refassgnexpr != NULL || sr->reflowerindexpr != NIL)
		return NIL;
	foreach_ptr(Expr, k, sr->refupperindexpr)
	{
		const char *key = text_const(k);

		if (!object_key(key))
			return NIL;
		keys = lappend(keys, makeString(pstrdup(key)));
	}
	return keys;
}

bool
sift_v6_json_member(const Expr *e, bool as_text, const Var **doc, const char **key)
{
	const List *args;

	e = strip(e);
	if (path_member(e, as_text, doc, key))
		return true;
	if (as_text && e != NULL && func_of(e) == F_JSONB_EXTRACT_PATH_TEXT && list_length(args_of(e)) == 2 && empty_path(lsecond(args_of(e))))
	{
		e = strip(linitial(args_of(e)));
		as_text = false;
	}
	if (e != NULL && IsA(e, SubscriptingRef))
	{
		List	   *keys = subscript_keys(e);

		*doc = (const Var *) strip(((const SubscriptingRef *) e)->refexpr);
		*key = list_length(keys) == 1 ? strVal(linitial(keys)) : NULL;
		return !as_text && *key != NULL && IsA(*doc, Var);
	}
	args = e != NULL ? args_of(e) : NIL;
	if (e == NULL || func_of(e) != (as_text ? F_JSONB_OBJECT_FIELD_TEXT : F_JSONB_OBJECT_FIELD) || list_length(args) != 2)
		return false;
	*doc = (const Var *) strip(linitial(args));
	*key = text_const(lsecond(args));
	return *doc != NULL && IsA(*doc, Var) && *key != NULL;
}

/*
 * doc -> 'k' or doc['k'] (as_text false), doc ->> 'k' or its #>> '{}' (as_text true) of such a document, and the leaf of its key (-1: none keeps it); as text, also
 * doc -> 'k1' .. ->> 'kn' and the leaf of its path (*key is not set)
 */
static int
field_leaf(const SiftV6Rel *rel, const Expr *e, bool as_text, int *att, const char **key)
{
	const Var  *doc;
	List	   *path = NIL;

	if (!sift_v6_json_member(e, as_text, &doc, key) &&
		(!as_text || (doc = sift_v6_doc_chain(e, &path)) == NULL || list_length(path) < 2 || exprType((const Node *) e) != TEXTOID))
		return -1;
	if (!doc_of(rel, (const Expr *) doc, att))
		return -1;
	return path != NIL ? path_leaf(rel, *att, path) : key_leaf(rel, *att, *key);
}

/* e is a link of a chain over a document (*link): `->` or `->>`, or jsonb_extract_path(doc, ..) and its text; the constant object key it reads (NULL: none) */
static const char *
chain_link(const Expr *e, bool *link)
{
	const Oid	fn = func_of(e);
	const char *key;

	*link = list_length(args_of(e)) == 2 && (fn == F_JSONB_OBJECT_FIELD || fn == F_JSONB_OBJECT_FIELD_TEXT || fn == F_JSONB_EXTRACT_PATH || fn == F_JSONB_EXTRACT_PATH_TEXT);
	if (!*link)
		return NULL;
	if (fn == F_JSONB_OBJECT_FIELD || fn == F_JSONB_OBJECT_FIELD_TEXT)
		return text_const(lsecond(args_of(e)));
	key = path_key(lsecond(args_of(e)));
	return object_key(key) ? key : NULL;
}

const Var *
sift_v6_doc_chain(const Expr *e, List **path)
{
	*path = NIL;
	for (e = strip(e); e != NULL; e = strip(e))
	{
		bool		link;
		const char *key;

		if (IsA(e, SubscriptingRef))
		{
			List	   *keys = subscript_keys(e);

			if (keys == NIL)
				return NULL;
			*path = list_concat(keys, *path);
			e = ((const SubscriptingRef *) e)->refexpr;
			continue;
		}
		key = chain_link(e, &link);
		if (!link)
			break;
		if (key == NULL)
			return NULL;
		*path = lcons(makeString(pstrdup(key)), *path);
		e = linitial(args_of(e));
	}
	return e != NULL && IsA(e, Var) && ((const Var *) e)->vartype == JSONBOID ? (const Var *) e : NULL;
}

static bool
scalar_leaf(const SiftV6Rel *rel, int leaf)
{
	return leaf >= 0 && sift_v6_rel_leaves(rel)->cols[leaf].kind == SIFT_COLK_SCALAR;
}

static uint32
type_bit(const char *name)
{
	static const struct
	{
		const char *name;
		uint8		tag;
	}			types[] = {{"string", SIFT_COLTAG_STRING}, {"number", SIFT_COLTAG_NUMBER}, {"boolean", SIFT_COLTAG_BOOLEAN}, {"object", SIFT_COLTAG_OBJECT},
	{"array", SIFT_COLTAG_ARRAY}, {"null", SIFT_COLTAG_JNULL}};

	for (int i = 0; i < lengthof(types); i++)
		if (strcmp(types[i].name, name) == 0)
			return (uint32) 1 << types[i].tag;
	return 0;
}

/* jsonb_typeof(..) = 'type' or = ANY (ARRAY['type', ..]): the call and the set of types */
static bool
typeof_test(const Expr *clause, const Expr **call, uint32 *mask)
{
	*mask = 0;
	if (IsA(clause, OpExpr) && get_opcode(((const OpExpr *) clause)->opno) == F_TEXTEQ && list_length(((const OpExpr *) clause)->args) == 2)
	{
		const OpExpr *op = (const OpExpr *) clause;
		const char *name = text_const(lsecond(op->args));

		*call = name != NULL ? strip(linitial(op->args)) : strip(lsecond(op->args));
		if (name == NULL)
			name = text_const(linitial(op->args));
		if (name == NULL)
			return false;
		*mask = type_bit(name);
		return true;
	}
	if (IsA(clause, ScalarArrayOpExpr) && ((const ScalarArrayOpExpr *) clause)->useOr && get_opcode(((const ScalarArrayOpExpr *) clause)->opno) == F_TEXTEQ)
	{
		const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) clause;
		const Const *arr = (const Const *) strip(lsecond(sa->args));
		Datum	   *el;
		bool	   *nl;
		int			n;

		if (!IsA(arr, Const) || arr->constisnull || ARR_ELEMTYPE(DatumGetArrayTypeP(arr->constvalue)) != TEXTOID)
			return false;
		deconstruct_array_builtin(DatumGetArrayTypeP(arr->constvalue), TEXTOID, &el, &nl, &n);
		for (int i = 0; i < n; i++)
			if (!nl[i])
				*mask |= type_bit(TextDatumGetCString(el[i]));
		*call = strip(linitial(sa->args));
		return true;
	}
	return false;
}

static List *
qual_of(int kind, int att, const char *key, uint32 mask)
{
	return list_make4(makeInteger(kind), makeInteger(att), makeString(pstrdup(key)), makeInteger((int) mask));
}

List *
sift_v6_json_qual(const SiftV6Rel *rel, const Expr *clause)
{
	const Expr *call;
	const char *key;
	uint32		mask;
	int			att;

	clause = strip(clause);
	if (func_of(clause) == F_JSONB_EXISTS && list_length(args_of(clause)) == 2)
	{
		key = text_const(lsecond(args_of(clause)));
		if (key != NULL && doc_of(rel, linitial(args_of(clause)), &att) && exists_leaf(rel, att, key))
			return qual_of(JQ_EXISTS, att, key, 0);
		return NIL;
	}
	if (typeof_test(clause, &call, &mask) && func_of(call) == F_JSONB_TYPEOF && list_length(args_of(call)) == 1)
	{
		const int	leaf = field_leaf(rel, linitial(args_of(call)), false, &att, &key);

		if (scalar_leaf(rel, leaf))
			return qual_of(JQ_TYPE, att, key, mask);
	}
	return NIL;
}

typedef struct VarsCtx
{
	const SiftV6Rel *rel;
	bool		values;
	bool		truth;			/* the expression is read for its truth alone: a NULL is as false */
	SiftV6MemberVatt member;
	void	   *arg;
} VarsCtx;

/* the column of whether the document doc has the key (SIFT_V6_JPRES_VATT + its leaf), of a format that reads it; InvalidAttrNumber: none */
static int
presence_vatt(const SiftV6Rel *rel, const Expr *doc, const char *key)
{
	int			att;

	if (rel->ops == NULL || rel->ops->jsonb == NULL || !rel->ops->jsonb->key_presence || key == NULL || !doc_of(rel, doc, &att) || !exists_leaf(rel, att, key))
		return InvalidAttrNumber;
	return SIFT_V6_JPRES_VATT + key_leaf(rel, att, key);
}

int
sift_v6_json_presence(const SiftV6Rel *rel, const Expr *clause)
{
	clause = strip(clause);
	if (func_of(clause) != F_JSONB_EXISTS || list_length(args_of(clause)) != 2)
		return InvalidAttrNumber;
	return presence_vatt(rel, linitial(args_of(clause)), text_const(lsecond(args_of(clause))));
}

/* the keys of the text[] constant of ?| and ?&: NIL for no constant, no key or a NULL among them */
static List *
array_keys(const Expr *e)
{
	const Const *c = (const Const *) strip(e);
	List	   *keys = NIL;
	Datum	   *el;
	bool	   *nl;
	int			n;

	if (c == NULL || !IsA(c, Const) || c->constisnull || c->consttype != TEXTARRAYOID)
		return NIL;
	deconstruct_array_builtin(DatumGetArrayTypeP(c->constvalue), TEXTOID, &el, &nl, &n);
	for (int i = 0; i < n; i++)
	{
		if (nl[i])
			return NIL;
		keys = lappend(keys, TextDatumGetCString(el[i]));
	}
	return keys;
}

/* `doc ? 'k'`, `doc ?| keys` and `doc ?& keys` as the presence columns of the keys, ORed or ANDed, and its document; NULL: no such test. A SQL NULL document has no key, where the test is NULL */
static Node *
presence_of(const SiftV6Rel *rel, const Expr *e, const Var **doc)
{
	const Oid	fn = func_of(e = strip(e));
	List	   *keys;
	List	   *tests = NIL;

	*doc = list_length(args_of(e)) == 2 ? (const Var *) strip(linitial(args_of(e))) : NULL;
	if ((fn != F_JSONB_EXISTS && fn != F_JSONB_EXISTS_ANY && fn != F_JSONB_EXISTS_ALL) || *doc == NULL || !IsA(*doc, Var))
		return NULL;
	keys = fn == F_JSONB_EXISTS ? list_make1((void *) text_const(lsecond(args_of(e)))) : array_keys(lsecond(args_of(e)));
	foreach_ptr(const char, key, keys)
	{
		const int	vatt = presence_vatt(rel, (const Expr *) *doc, key);

		if (vatt == InvalidAttrNumber)
			return NULL;
		tests = lappend(tests, makeVar(rel->relid, vatt, BOOLOID, -1, InvalidOid, 0));
	}
	if (tests == NIL)
		return NULL;
	return list_length(tests) == 1 ? linitial(tests) : (Node *) (fn == F_JSONB_EXISTS_ANY ? make_orclause(tests) : make_andclause(tests));
}

static Expr *
doc_null_test(const Var *doc, NullTestType type)
{
	NullTest   *nt = makeNode(NullTest);

	*nt = (NullTest) {.xpr.type = T_NullTest,.arg = (Expr *) copyObject(doc),.nulltesttype = type,.location = -1};
	return (Expr *) nt;
}

/*
 * A key test as the presence of its keys where the document is NOT NULL or only the truth of the test counts (truth: a WHERE, a FILTER, a CASE WHEN, the argument of IS [NOT] TRUE, through
 * AND and OR); NOT, IS FALSE and IS NOT FALSE of the test of a document that may be NULL, read for their truth, tell the NULL document apart with a test of it (IS NOT NULL, IS NULL), a scan
 * key of a WHERE or a FILTER. NULL: none of those
 */
static Node *
presence_test(const VarsCtx *ctx, const Node *n)
{
	const BooleanTest *bt = IsA(n, BooleanTest) ? (const BooleanTest *) n : NULL;
	const bool	negated = is_notclause(n);
	const Var  *doc;
	Node	   *pres = presence_of(ctx->rel, bt != NULL ? bt->arg : negated ? get_notclausearg(n) : (const Expr *) n, &doc);
	const bool	nullable = pres != NULL && !bms_is_member(doc->varattno, ctx->rel->notnull);

	if (pres == NULL)
		return NULL;
	if (bt == NULL && !negated)
		return !nullable || ctx->truth ? pres : NULL;
	if (!nullable || !ctx->truth || (bt != NULL && bt->booltesttype != IS_FALSE && bt->booltesttype != IS_NOT_FALSE))
		return NULL;
	if (bt != NULL && bt->booltesttype == IS_NOT_FALSE)
		return (Node *) make_orclause(list_make2(pres, doc_null_test(doc, IS_NULL)));
	return (Node *) make_andclause(list_make2(make_notclause((Expr *) pres), doc_null_test(doc, IS_NOT_NULL)));
}

/* the format reads the values of the keys of a document as the heap does, or the scan runs alone, where a value no leaf holds is refused */
bool
sift_v6_values_exact(const SiftV6Rel *rel)
{
	return rel->ops != NULL && rel->ops->jsonb != NULL;
}

static bool
is_int_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

/* the integers of a leaf of the type are integers of the cast's type */
static bool
int_leaf_fits(const SiftColDef *d, Oid typid)
{
	const int	width = typid == INT2OID ? SIFT_COLT_INT2 : typid == INT4OID ? SIFT_COLT_INT4 : SIFT_COLT_INT8;

	return d->store == SIFT_COLS_INT && d->type >= SIFT_COLT_INT2 && d->type <= width;
}

/*
 * The leaf holds what the column reads: the strings of a text key, the numbers of a float8 key, the booleans of a bool key, the integers of an integer key (the writer keeps a number whose text is
 * no integer, 3.0, aside); the `->>` text of a boolean and the bool and integer casts are read only by a format whose scan reads the other values from the exceptions of their rows (a value of
 * another JSON type is one), as `->>` gives them
 */
static bool
leaf_reads(const SiftV6Rel *rel, int leaf, Oid typid)
{
	const SiftColDef *d = &sift_v6_rel_leaves(rel)->cols[leaf];
	const bool	exact = rel->ops->jsonb->exact_values;
	const bool	bool_leaf = d->store == SIFT_COLS_INT && d->type == SIFT_COLT_BOOL && exact;

	switch (typid)
	{
		case FLOAT8OID:
			return d->store == SIFT_COLS_F8;
		case BOOLOID:
			return bool_leaf;
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return exact && int_leaf_fits(d, typid);
		default:
			return d->store == SIFT_COLS_STR || bool_leaf;
	}
}

bool
sift_v6_json_read(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out)
{
	const Expr *text = strip(e);
	const char *key;
	int			att,
				leaf;
	Oid			typid = TEXTOID;

	if (text != NULL && IsA(text, CoerceViaIO) && (((const CoerceViaIO *) text)->resulttype == FLOAT8OID || ((const CoerceViaIO *) text)->resulttype == BOOLOID ||
											 is_int_type(((const CoerceViaIO *) text)->resulttype)))
	{
		typid = ((const CoerceViaIO *) text)->resulttype;
		text = strip(((const CoerceViaIO *) text)->arg);
	}
	leaf = field_leaf(rel, text, true, &att, &key);
	if (!scalar_leaf(rel, leaf) || !sift_v6_values_exact(rel) || !leaf_reads(rel, leaf, typid))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = typid,.jleaf = leaf + 1,.jint = is_int_type(typid)};
	return true;
}

/*
 * e reads a member of a document as a number or a boolean: the float8 of its `->>` text, or the cast of its jsonb to float8, an integer or bool (a JSON null casts to NULL from PostgreSQL 18 on, where it raised before,
 * which the leaf does not tell). The document, the keys from it, the type and whether it is the cast of the jsonb; NULL: e is no such read
 */
static const Var *
member_read(const Expr *e, List **path, Oid *typid, bool *jsonb_cast)
{
	const Expr *x = strip(e);

	*jsonb_cast = x != NULL && IsA(x, FuncExpr) && list_length(args_of(x)) == 1 &&
		(func_of(x) == F_FLOAT8_JSONB || func_of(x) == F_INT2_JSONB || func_of(x) == F_INT4_JSONB || func_of(x) == F_INT8_JSONB || func_of(x) == F_BOOL_JSONB);
	if (*jsonb_cast && !SIFT_JSON_NULL_CAST_RAISES)
	{
		*typid = ((const FuncExpr *) x)->funcresulttype;
		return sift_v6_doc_chain(linitial(args_of(x)), path);
	}
	if (x == NULL || !IsA(x, CoerceViaIO) || ((const CoerceViaIO *) x)->resulttype != FLOAT8OID || exprType((const Node *) ((const CoerceViaIO *) x)->arg) != TEXTOID)
		return NULL;
	*typid = FLOAT8OID;
	return sift_v6_doc_chain(((const CoerceViaIO *) x)->arg, path);
}

/* the leaf of a member holds what the read converts: any of its numbers from float8, integers to an integer type at least as wide, the booleans of a bool leaf to bool */
static bool
member_leaf_reads(const SiftV6Rel *rel, int leaf, Oid typid)
{
	const SiftColDef *d = &sift_v6_rel_leaves(rel)->cols[leaf];

	if (typid == BOOLOID)
		return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_BOOL;
	return d->store == SIFT_COLS_F8 || (is_int_type(typid) && int_leaf_fits(d, typid));
}

bool
sift_v6_json_map_member(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out, List **path)
{
	const Var  *doc = NULL;
	const SiftJsonbOps *jsonb = rel->ops != NULL ? rel->ops->jsonb : NULL;
	Oid			typid;
	bool		jsonb_cast;
	int			att,
				leaf;

	if (jsonb == NULL || jsonb->member_leaf == NULL || (doc = member_read(e, path, &typid, &jsonb_cast)) == NULL || *path == NIL || !doc_of(rel, (const Expr *) doc, &att) ||
		(jsonb_cast && !jsonb->exact_values) || (leaf = jsonb->member_leaf(rel->reloid, att, *path)) < 0 || !member_leaf_reads(rel, leaf, typid))
		return false;
	if (sift_v6_rel_leaves(rel)->cols[leaf].parent < 0 && !jsonb_cast)
		*path = NIL;
	*out = (SiftV6Col) {.attno = att,.typid = typid,.jleaf = leaf + 1,.jsonb_cast = jsonb_cast};
	return true;
}

/* jsonb_typeof(doc -> 'key') of a shredded scalar key: the leaf of the key (-1: the expression is no such call) */
static int
typeof_leaf(const SiftV6Rel *rel, const Node *n)
{
	const Expr *e = strip((const Expr *) n);
	const char *key;
	int			att,
				leaf;

	if (e == NULL || !IsA(e, FuncExpr) || func_of(e) != F_JSONB_TYPEOF || list_length(args_of(e)) != 1)
		return -1;
	leaf = field_leaf(rel, linitial(args_of(e)), false, &att, &key);
	return scalar_leaf(rel, leaf) && sift_v6_values_exact(rel) ? leaf : -1;
}

/* The virtual attribute of what n reads of a shredded scalar key (the type, the `->>` text of a text leaf, its float8 cast of a float8 leaf), InvalidAttrNumber for any other expression */
static int
read_vatt(const SiftV6Rel *rel, const Node *n, bool values, Oid *typid)
{
	SiftV6Col	col;
	int			leaf;

	if ((leaf = typeof_leaf(rel, n)) >= 0)
	{
		*typid = TEXTOID;
		return SIFT_V6_JTYPE_VATT + leaf;
	}
	if (!values || !sift_v6_json_read(rel, (const Expr *) n, &col))
		return InvalidAttrNumber;
	*typid = col.typid;
	return sift_v6_col_input_attno(&col);
}

static Node *
vars_mutator(Node *n, VarsCtx *ctx)
{
	Oid			typid;
	int			vatt;
	const bool	truth = ctx->truth;
	Node	   *out;
	List	   *tests;

	if (n == NULL)
		return NULL;
	if ((vatt = read_vatt(ctx->rel, n, ctx->values, &typid)) != InvalidAttrNumber ||
		(ctx->values && ctx->member != NULL && (vatt = ctx->member(ctx->arg, (const Expr *) n, &typid)) != InvalidAttrNumber))
		return (Node *) makeVar(ctx->rel->relid, vatt, typid, -1, exprCollation(n), 0);
	if ((out = presence_test(ctx, n)) != NULL)
		return out;
	if (ctx->truth && (tests = sift_v6_contains_quals(ctx->rel, (const Expr *) n)) != NIL)
		return (Node *) make_ands_explicit(tests);
	if (IsA(n, CaseWhen))
	{
		CaseWhen   *w = palloc(sizeof(CaseWhen));

		*w = *(const CaseWhen *) n;
		ctx->truth = true;
		w->expr = (Expr *) vars_mutator((Node *) w->expr, ctx);
		ctx->truth = false;
		w->result = (Expr *) vars_mutator((Node *) w->result, ctx);
		ctx->truth = truth;
		return (Node *) w;
	}
	ctx->truth = (truth && (IsA(n, List) || is_andclause(n) || is_orclause(n))) ||
		(IsA(n, BooleanTest) && (((const BooleanTest *) n)->booltesttype == IS_TRUE || ((const BooleanTest *) n)->booltesttype == IS_NOT_TRUE));
	out = expression_tree_mutator(n, vars_mutator, ctx);
	ctx->truth = truth;
	return out;
}

Expr *
sift_v6_json_vars(const SiftV6Rel *rel, const Expr *e, bool values, bool truth, SiftV6MemberVatt member, void *arg)
{
	VarsCtx		ctx = {rel, values, truth, member, arg};

	return (Expr *) vars_mutator((Node *) e, &ctx);
}

bool
sift_v6_json_type_col(const SiftV6Rel *rel, const Var *v, SiftV6Col *out)
{
	const int	leaf = sift_v6_vatt_leaf(v->varattno);
	const SiftColSchema *leaves = sift_v6_rel_leaves(rel);
	const bool	held = leaves != NULL && leaf < leaves->ncols;

	if (held)
		*out = sift_v6_vatt_col(v->varattno, leaves->cols[leaf].attno, v->vartype, 0);
	return held;
}

typedef struct JsonLeaves
{
	Bitmapset  *value;			/* the leaves the statement reads a value of that raises for some JSON value (the float8 and the bool casts) */
	Bitmapset  *type;			/* the leaves it reads the JSON type of, anywhere but in a scan key */
} JsonLeaves;

/* the presence of a key is no test of a type: a row that has not the key has no value to read either; a read that raises for no value (the `->>` text, the integer a leaf holds) clashes with none */
static void
note_col(JsonLeaves *j, const SiftV6Col *c)
{
	if (c->jleaf > 0 && c->jmember == 0 && c->jtype && c->typid != BOOLOID)
		j->type = bms_add_member(j->type, c->jleaf - 1);
	else if (c->jleaf > 0 && c->jmember == 0 && !c->jtype && (c->typid == FLOAT8OID || c->typid == BOOLOID || c->jint))
		j->value = bms_add_member(j->value, c->jleaf - 1);
}

static void
note_vatt(JsonLeaves *j, int attno)
{
	if (sift_v6_vatt_is_type(attno) && !sift_v6_vatt_is_presence(attno))
		j->type = bms_add_member(j->type, sift_v6_vatt_leaf(attno));
	else if (sift_v6_vatt_raises(attno) && !sift_v6_vatt_is_member(attno))
		j->value = bms_add_member(j->value, sift_v6_vatt_leaf(attno));
}

static bool
note_vars(Node *n, JsonLeaves *j)
{
	if (n != NULL && IsA(n, Var) && sift_v6_vatt_is_json(((const Var *) n)->varattno))
		note_vatt(j, ((const Var *) n)->varattno);
	return n != NULL && expression_tree_walker(n, note_vars, j);
}

/*
 * PostgreSQL does not read the value of a row a test of its type leaves out (`FILTER (WHERE jsonb_typeof(d -> 'k') = 'number')` over `(d ->> 'k')::float8`), and the leaf cannot hold the strings among its
 * numbers: a statement that reads both the type and the value of one key in the batches would raise for a row the heap skips. Only the scan keys of the type test (SiftV6Spec.jsonqual) drop those rows first.
 */
bool
sift_v6_json_type_clash(const SiftV6Spec *s)
{
	JsonLeaves	j = {0};

	for (int i = 0; i < s->nkeys; i++)
		note_col(&j, &s->key[i]);
	for (int a = 0; a < s->naggs; a++)
	{
		note_col(&j, &s->agg[a].arg);
		note_col(&j, &s->agg[a].arg2);
		for (int c = 0; c < s->agg[a].nxcols; c++)
			note_col(&j, &s->agg[a].xcol[c]);
	}
	for (int f = 0; f < s->nfcols; f++)
		note_col(&j, &s->fcol[f]);
	for (int d = 0; d < s->nderive; d++)
		note_col(&j, &s->derive[d].src);
	foreach_ptr(List, item, s->pgexpr)
		for (int i = 1; i < list_length(lsecond(item)); i += SIFT_V6_PGX_INTS)
			if (sift_v6_vatt_is_json(list_nth_int(lsecond(item), i)))
				note_vatt(&j, list_nth_int(lsecond(item), i));
	foreach_ptr(Node, clause, s->vqual)
		note_vars(clause, &j);
	return bms_overlap(j.value, j.type);
}

/*
 * The text of a number, a boolean or a document is what `->>` gives for such a value, which the leaf of a text key does not hold (it holds the strings, and a value of another type is an exception
 * the leaf reads as NULL): a comparison with a constant that no such value can be the text of is decided by the leaf alone.
 */
bool
sift_v6_json_text_unambiguous(const char *s)
{
	return s[0] != '\0' && strchr("0123456789+-.{[", s[0]) == NULL && strcmp(s, "true") != 0 && strcmp(s, "false") != 0 && strcmp(s, "null") != 0 &&
		pg_strcasecmp(s, "nan") != 0 && pg_strcasecmp(s, "infinity") != 0;
}

Expr *
sift_v6_json_text_qual(const SiftV6Rel *rel, const OpExpr *op)
{
	const Expr *l;
	const Const *c;
	SiftV6Col	col;
	OpExpr	   *q;

	if (list_length(op->args) != 2 || get_opcode(op->opno) != F_TEXTEQ || !sift_text_bytes_equal(op->inputcollid))
		return NULL;
	l = strip(linitial(op->args));
	c = (const Const *) strip(lsecond(op->args));
	if (c != NULL && IsA(l, Const) && !IsA(c, Const))
	{
		const Expr *t = l;

		l = (const Expr *) c;
		c = (const Const *) t;
	}
	if (c == NULL || !IsA(c, Const) || c->constisnull || c->consttype != TEXTOID || !sift_v6_json_text_unambiguous(TextDatumGetCString(c->constvalue)) || !sift_v6_json_read(rel, l, &col) || col.typid != TEXTOID)
		return NULL;
	q = copyObject((OpExpr *) op);
	q->args = list_make2(makeVar(INDEX_VAR, SIFT_V6_JTEXT_VATT + col.jleaf - 1, TEXTOID, -1, op->inputcollid, 0), copyObject((Node *) c));
	return (Expr *) q;
}

void
sift_v6_json_append_scankeys(List *jsonqual, Oid reloid, bool is_table, ScanKey keys, int *nkeys)
{
	const SiftJsonbOps *ops;

	if (jsonqual == NIL)
		return;
	ops = sift_store_ops_for_oid(reloid, is_table)->jsonb;
	foreach_ptr(List, q, jsonqual)
	{
		const char *key = strVal(lthird(q));
		const int	att = intVal(lsecond(q));
		const int	leaf = ops->key_leaf(reloid, is_table, att, list_make1(makeString(pstrdup(key))));

		SIFT_INVARIANT(leaf >= 0, "the key of a jsonb qual of the aggregate scan is a leaf of the relation");
		keys[(*nkeys)++] = intVal(linitial(q)) == JQ_EXISTS ?
			(ScanKeyData) {.sk_attno = att,.sk_strategy = COL_STRAT_JSONB_EXISTS,.sk_argument = CStringGetTextDatum(key)} :
			(ScanKeyData) {.sk_attno = att,.sk_strategy = COL_STRAT_JSONB_TYPE,.sk_argument = Int64GetDatum(COL_JSON_TYPE_ARG(leaf, (uint32) intVal(lfourth(q))))};
	}
}

void
sift_v6_json_scankeys(const SiftV6Spec *spec, Relation index, ScanKey *keys, int *nkeys)
{
	ScanKey		all;

	if (spec->jsonqual == NIL)
		return;
	all = palloc((*nkeys + list_length(spec->jsonqual)) * sizeof(ScanKeyData));
	if (*nkeys > 0)
		memcpy(all, *keys, *nkeys * sizeof(ScanKeyData));
	sift_v6_json_append_scankeys(spec->jsonqual, RelationGetRelid(index), false, all, nkeys);
	*keys = all;
}

/* the `->>` text of the value of an exception, NULL for a JSON null */
static char *
exception_text(const char *payload, uint32 len)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;

	if (!JsonbExtractScalar(&jb->root, &jv))
		return JsonbToCString(NULL, &jb->root, (int) len);
	switch (jv.type)
	{
		case jbvString:
			return pnstrdup(jv.val.string.val, jv.val.string.len);
		case jbvNumeric:
			return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv.val.numeric)));
		case jbvBool:
			return pstrdup(jv.val.boolean ? "true" : "false");
		default:
			return NULL;
	}
}

bool
sift_v6_json_exception(SiftV6Class cls, const char *payload, uint32 len, MemoryContext cx, const char **text, uint32 *tlen, double *f)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	char	   *s = exception_text(payload, len);

	if (s != NULL && cls == SV6C_F64)
		*f = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(s)));
	else if (s != NULL)
	{
		*text = s;
		*tlen = strlen(s);
	}
	MemoryContextSwitchTo(old);
	return s != NULL;
}

/* the number of the jsonb of a (doc -> 'k')::float8, as its text; NULL: a JSON null (NULL from PostgreSQL 18 on, the only version the cast is read in) or a value the cast raises for (*raises) */
static char *
cast_number_text(const char *payload, uint32 len, bool *raises)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;
	const bool	scalar = JsonbExtractScalar(&jb->root, &jv);

	*raises = !scalar || (jv.type != jbvNumeric && jv.type != jbvNull);
	return scalar && jv.type == jbvNumeric ? DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv.val.numeric))) : NULL;
}

/* the input function of the cast of the `->>` text to the type */
static PGFunction
text_cast_in(Oid typid)
{
	switch (typid)
	{
		case FLOAT8OID:
			return float8in;
		case INT2OID:
			return int2in;
		case INT4OID:
			return int4in;
		case INT8OID:
			return int8in;
		default:
			return boolin;
	}
}

/* the cast of a jsonb to bool (of a boolean only) or to an integer (of a number only, rounded half away from zero as numeric is, raising for one the type cannot hold) */
static SiftV6JsonExc
jsonb_scalar_read(Oid typid, const char *payload, uint32 len, Datum *out)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;
	bool		err = false;
	int64		v;

	if (!JsonbExtractScalar(&jb->root, &jv) || (jv.type != (typid == BOOLOID ? jbvBool : jbvNumeric) && jv.type != jbvNull))
		return SV6JX_RAISES;
	if (jv.type == jbvNull)
		return SV6JX_NULL;
	if (typid == BOOLOID)
		return (*out = BoolGetDatum(jv.val.boolean), SV6JX_VALUE);
	v = sift_numeric_int8(jv.val.numeric, &err);
	if (err || (typid == INT4OID && (v < PG_INT32_MIN || v > PG_INT32_MAX)) || (typid == INT2OID && (v < PG_INT16_MIN || v > PG_INT16_MAX)))
		return SV6JX_RAISES;
	*out = typid == INT8OID ? Int64GetDatum(v) : typid == INT4OID ? Int32GetDatum((int32) v) : Int16GetDatum((int16) v);
	return SV6JX_VALUE;
}

/* the cast of a jsonb to the type, whose errors are PostgreSQL's */
static PGFunction
jsonb_cast_fn(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
			return jsonb_int2;
		case INT4OID:
			return jsonb_int4;
		case INT8OID:
			return jsonb_int8;
		case BOOLOID:
			return jsonb_bool;
		default:
			return jsonb_float8;
	}
}

SiftV6JsonExc
sift_v6_json_exception_read(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx, SiftV6JsonValue *out)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	bool		raises = false;
	char	   *s;
	SiftV6JsonExc res;

	if (jsonb_cast && typid != FLOAT8OID)
	{
		res = jsonb_scalar_read(typid, payload, len, &out->value);
		MemoryContextSwitchTo(old);
		return res;
	}
	s = jsonb_cast ? cast_number_text(payload, len, &raises) : exception_text(payload, len);
	res = raises ? SV6JX_RAISES : s == NULL ? SV6JX_NULL : SV6JX_VALUE;
	if (res == SV6JX_VALUE && sift_v6_class(typid) == SV6C_TEXT)
		*out = (SiftV6JsonValue) {.text = s,.tlen = strlen(s)};
	else if (res == SV6JX_VALUE && !DirectInputFunctionCallSafe(text_cast_in(typid), s, InvalidOid, -1, (Node *) &escontext, &out->value))
		res = SV6JX_RAISES;
	MemoryContextSwitchTo(old);
	return res;
}

void
sift_v6_json_exception_raise(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	char	   *s;

	if (jsonb_cast)
		(void) DirectFunctionCall1(jsonb_cast_fn(typid), PointerGetDatum(memcpy(palloc(len), payload, len)));
	else if ((s = exception_text(payload, len)) != NULL)
		(void) DirectFunctionCall1(text_cast_in(typid), CStringGetDatum(s));
	MemoryContextSwitchTo(old);
	SIFT_INVARIANT(false, "the conversion of a value of a key that does not convert raises");
}

bool
sift_v6_json_float_int(Oid typid, double f, Datum *out)
{
	const double r = round(f);

	if (typid == INT2OID ? !(r >= PG_INT16_MIN && r <= PG_INT16_MAX) : typid == INT4OID ? !(r >= PG_INT32_MIN && r <= PG_INT32_MAX) : !(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
		return false;
	*out = typid == INT8OID ? Int64GetDatum((int64) r) : typid == INT4OID ? Int32GetDatum((int32) r) : Int16GetDatum((int16) r);
	return true;
}

void
sift_v6_json_float_int_raise(Oid typid, double f)
{
	const Datum num = DirectFunctionCall1(float8_numeric, Float8GetDatum(f));

	(void) DirectFunctionCall1(typid == INT2OID ? numeric_int2 : typid == INT4OID ? numeric_int4 : numeric_int8, num);
	SIFT_INVARIANT(false, "the integer cast of a number out of the range of its type raises");
}
