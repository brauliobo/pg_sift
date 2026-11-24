/*
 * agg/v6/jsonb_qual.c - the keys of the jsonb documents of an index of format major 2 as the aggregate scan of SiftAggV6 reads them: the leaf of a top-level key (col/f2/f2_rel.c) says on which rows
 * `doc ? 'k'` holds and of which JSON type the value is (jsonb_typeof), and holds the value of the rows that are of its type; the other rows keep their value as the payload of an exception.
 */
/* check-memory: allow-allocs every array here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/group/ht.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_unnest_jsonb.h"
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

/* The column of the index (1-based) that holds the jsonb attribute e is, and is shredded */
static bool
doc_of(const SiftV6Rel *rel, const Expr *e, int *att)
{
	const Var  *v = (const Var *) strip(e);

	if (v == NULL || !IsA(v, Var) || v->varno != rel->relid || v->varlevelsup != 0 || v->vartype != JSONBOID)
		return false;
	for (int i = 0; i < rel->index->ncolumns; i++)
		if (rel->index->indexkeys[i] == v->varattno)
		{
			*att = i + 1;
			return rel->colrel->json[i + 1];
		}
	return false;
}

/* the leaf of the format of the index that keeps whether the key is on the rows of the document attribute att (-1: none keeps it) */
static int
key_leaf(const SiftV6Rel *rel, int att, const char *key)
{
	return rel->ops != NULL && rel->ops->jsonb != NULL ? rel->ops->jsonb->key_leaf(rel->index->indexoid, att, key, strlen(key)) : -1;
}

/* doc -> 'k' (fn F_JSONB_OBJECT_FIELD) or doc ->> 'k' (F_JSONB_OBJECT_FIELD_TEXT) of such a document, and the leaf of its key (-1: none keeps it) */
static int
field_leaf(const SiftV6Rel *rel, const Expr *e, Oid fn, int *att, const char **key)
{
	List	   *args;

	e = strip(e);
	args = e != NULL ? args_of(e) : NIL;
	if (e == NULL || func_of(e) != fn || list_length(args) != 2 || !doc_of(rel, linitial(args), att) || (*key = text_const(lsecond(args))) == NULL)
		return -1;
	return key_leaf(rel, *att, *key);
}

static bool
scalar_leaf(const SiftV6Rel *rel, int leaf)
{
	return leaf >= 0 && rel->colrel->schema->cols[leaf].kind == SIFT_COLK_SCALAR;
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
		if (key != NULL && doc_of(rel, linitial(args_of(clause)), &att) && key_leaf(rel, att, key) >= 0)
			return qual_of(JQ_EXISTS, att, key, 0);
		return NIL;
	}
	if (typeof_test(clause, &call, &mask) && func_of(call) == F_JSONB_TYPEOF && list_length(args_of(call)) == 1 &&
		scalar_leaf(rel, field_leaf(rel, linitial(args_of(call)), F_JSONB_OBJECT_FIELD, &att, &key)))
		return qual_of(JQ_TYPE, att, key, mask);
	return NIL;
}

bool
sift_v6_json_read(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out)
{
	const Expr *text = strip(e);
	const char *key;
	int			att,
				leaf;
	Oid			typid = TEXTOID;
	uint8		store = SIFT_COLS_STR;

	if (text != NULL && IsA(text, CoerceViaIO) && ((const CoerceViaIO *) text)->resulttype == FLOAT8OID)
	{
		text = strip(((const CoerceViaIO *) text)->arg);
		typid = FLOAT8OID;
		store = SIFT_COLS_F8;
	}
	leaf = field_leaf(rel, text, F_JSONB_OBJECT_FIELD_TEXT, &att, &key);
	if (!scalar_leaf(rel, leaf) || rel->colrel->schema->cols[leaf].store != store)
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = typid,.jleaf = leaf + 1};
	return true;
}

/*
 * The text of a number, a boolean or a document is what `->>` gives for such a value, which the leaf of a text key does not hold (it holds the strings, and a value of another type is an exception
 * the leaf reads as NULL): a comparison with a constant that no such value can be the text of is decided by the leaf alone.
 */
static bool
json_text_unambiguous(const Const *c)
{
	char	   *s = TextDatumGetCString(c->constvalue);

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
	if (c == NULL || !IsA(c, Const) || c->constisnull || c->consttype != TEXTOID || !json_text_unambiguous(c) || !sift_v6_json_read(rel, l, &col) || col.typid != TEXTOID)
		return NULL;
	q = copyObject((OpExpr *) op);
	q->args = list_make2(makeVar(INDEX_VAR, SIFT_V6_JLEAF_VATT + col.jleaf - 1, TEXTOID, -1, op->inputcollid, 0), copyObject((Node *) c));
	return (Expr *) q;
}

void
sift_v6_json_scankeys(const SiftV6Spec *spec, Relation index, ScanKey *keys, int *nkeys)
{
	const SiftJsonbOps *ops = sift_store_ops_for_scan(RelationGetRelid(index), false)->jsonb;
	ScanKey		all;

	if (spec->jsonqual == NIL)
		return;
	all = palloc((*nkeys + list_length(spec->jsonqual)) * sizeof(ScanKeyData));
	if (*nkeys > 0)
		memcpy(all, *keys, *nkeys * sizeof(ScanKeyData));
	foreach_ptr(List, q, spec->jsonqual)
	{
		const char *key = strVal(lthird(q));
		const int	att = intVal(lsecond(q));
		const int	leaf = ops->key_leaf(RelationGetRelid(index), att, key, strlen(key));

		SIFT_INVARIANT(leaf >= 0, "the key of a jsonb qual of the aggregate scan is a leaf of the index");
		all[(*nkeys)++] = intVal(linitial(q)) == JQ_EXISTS ?
			(ScanKeyData) {.sk_attno = att,.sk_strategy = COL_STRAT_JSONB_EXISTS,.sk_argument = CStringGetTextDatum(key)} :
			(ScanKeyData) {.sk_attno = att,.sk_strategy = COL_STRAT_JSONB_TYPE,.sk_argument = Int64GetDatum(COL_JSON_TYPE_ARG(leaf, (uint32) intVal(lfourth(q))))};
	}
	*keys = all;
}

bool
sift_v6_json_exception(SiftV6Class cls, const char *payload, uint32 len, MemoryContext cx, const char **text, uint32 *tlen, double *f)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;
	char	   *s = NULL;

	if (!JsonbExtractScalar(&jb->root, &jv))
		s = JsonbToCString(NULL, &jb->root, (int) len);
	else if (jv.type == jbvString)
		s = pnstrdup(jv.val.string.val, jv.val.string.len);
	else if (jv.type == jbvNumeric)
		s = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv.val.numeric)));
	else if (jv.type == jbvBool)
		s = pstrdup(jv.val.boolean ? "true" : "false");
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
