/*
 * agg/qual/jsonb.c - the keys of the jsonb documents of an index of format major 2 as the aggregate scan of SiftAggV6 reads them: the leaf of a top-level key (col/f2/rel.c) says on which rows
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

#include "agg/agg.h"
#include "agg/ht/ht.h"
#include "agg/engine.h"
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

/* The leaves of the relation: those of its index, or of the store of its table */
static const SiftColSchema *
leaves_of(const SiftV6Rel *rel)
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

/* the leaf of the format of the index that keeps whether the key is on the rows of the document attribute att (-1: none keeps it) */
static int
key_leaf(const SiftV6Rel *rel, int att, const char *key)
{
	const SiftJsonbOps *jsonb = rel->ops != NULL ? rel->ops->jsonb : NULL;

	if (jsonb == NULL)
		return -1;
	if (rel->table != NULL)
		return jsonb->table_key_leaf != NULL ? jsonb->table_key_leaf(rel->reloid, att, key, strlen(key)) : -1;
	return jsonb->key_leaf(rel->index->indexoid, att, key, strlen(key));
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

/* doc -> 'k' as a call with a path of one key: jsonb_extract_path(doc, 'k') or doc #> '{k}' (as_text false), jsonb_extract_path_text(doc, 'k') or doc #>> '{k}' (as_text true) */
static bool
path_member(const Expr *e, bool as_text, const Var **doc, const char **key)
{
	const List *args = e != NULL ? args_of(e) : NIL;

	if (e == NULL || func_of(e) != (as_text ? F_JSONB_EXTRACT_PATH_TEXT : F_JSONB_EXTRACT_PATH) || list_length(args) != 2 || (*key = path_key(lsecond(args))) == NULL ||
		strchr(" +-0123456789", (*key)[strspn(*key, " ")]) != NULL)
		return false;
	*doc = (const Var *) strip(linitial(args));
	return *doc != NULL && IsA(*doc, Var);
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
		const SubscriptingRef *sr = (const SubscriptingRef *) e;

		if (as_text || sr->refcontainertype != JSONBOID || sr->refassgnexpr != NULL || sr->reflowerindexpr != NIL || list_length(sr->refupperindexpr) != 1)
			return false;
		*doc = (const Var *) strip(sr->refexpr);
		*key = text_const(linitial(sr->refupperindexpr));
		return *doc != NULL && IsA(*doc, Var) && *key != NULL;
	}
	args = e != NULL ? args_of(e) : NIL;
	if (e == NULL || func_of(e) != (as_text ? F_JSONB_OBJECT_FIELD_TEXT : F_JSONB_OBJECT_FIELD) || list_length(args) != 2)
		return false;
	*doc = (const Var *) strip(linitial(args));
	*key = text_const(lsecond(args));
	return *doc != NULL && IsA(*doc, Var) && *key != NULL;
}

/* doc -> 'k' or doc['k'] (as_text false), doc ->> 'k' or its #>> '{}' (as_text true) of such a document, and the leaf of its key (-1: none keeps it) */
static int
field_leaf(const SiftV6Rel *rel, const Expr *e, bool as_text, int *att, const char **key)
{
	const Var  *doc;

	if (!sift_v6_json_member(e, as_text, &doc, key) || !doc_of(rel, (const Expr *) doc, att))
		return -1;
	return key_leaf(rel, *att, *key);
}

const Var *
sift_v6_doc_chain(const Expr *e, List **path)
{
	*path = NIL;
	for (e = strip(e); e != NULL && (func_of(e) == F_JSONB_OBJECT_FIELD || func_of(e) == F_JSONB_OBJECT_FIELD_TEXT) && list_length(args_of(e)) == 2; e = strip(linitial(args_of(e))))
	{
		const char *key = text_const(lsecond(args_of(e)));

		if (key == NULL)
			return NULL;
		*path = lcons(makeString(pstrdup(key)), *path);
	}
	return e != NULL && IsA(e, Var) && ((const Var *) e)->vartype == JSONBOID ? (const Var *) e : NULL;
}

static bool
scalar_leaf(const SiftV6Rel *rel, int leaf)
{
	return leaf >= 0 && leaves_of(rel)->cols[leaf].kind == SIFT_COLK_SCALAR;
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

static bool
types_exact(const SiftV6Rel *rel, int leaf, uint32 mask)
{
	const SiftJsonbOps *jsonb = rel->ops->jsonb;

	return jsonb->types_exact == NULL || jsonb->types_exact(rel->table != NULL ? rel->reloid : rel->index->indexoid, rel->table != NULL, leaf, mask);
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
	if (typeof_test(clause, &call, &mask) && func_of(call) == F_JSONB_TYPEOF && list_length(args_of(call)) == 1)
	{
		const int	leaf = field_leaf(rel, linitial(args_of(call)), false, &att, &key);

		if (scalar_leaf(rel, leaf) && types_exact(rel, leaf, mask))
			return qual_of(JQ_TYPE, att, key, mask);
	}
	return NIL;
}

/* the format reads the values of the keys of a document as the heap does, or the scan runs alone, where a value no leaf holds is refused */
bool
sift_v6_values_exact(const SiftV6Rel *rel)
{
	return rel->ops != NULL && rel->ops->jsonb != NULL;
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
	leaf = field_leaf(rel, text, true, &att, &key);
	if (!scalar_leaf(rel, leaf) || leaves_of(rel)->cols[leaf].store != store || !sift_v6_values_exact(rel))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = typid,.jleaf = leaf + 1};
	return true;
}

bool
sift_v6_json_map_member(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out, List **path)
{
	const Expr *text = strip(e);
	const Var  *doc;
	int			att,
				leaf;

	if (rel->table != NULL || text == NULL || !IsA(text, CoerceViaIO) || ((const CoerceViaIO *) text)->resulttype != FLOAT8OID)
		return false;
	text = strip(((const CoerceViaIO *) text)->arg);
	if (text == NULL || func_of(text) != F_JSONB_OBJECT_FIELD_TEXT || (doc = sift_v6_doc_chain(text, path)) == NULL || list_length(*path) < 2 || !doc_of(rel, (const Expr *) doc, &att))
		return false;
	leaf = rel->ops != NULL && rel->ops->jsonb != NULL && rel->ops->jsonb->member_leaf != NULL ? rel->ops->jsonb->member_leaf(rel->index->indexoid, att, *path) : -1;
	if (leaf < 0)
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = FLOAT8OID,.jleaf = leaf + 1};
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

typedef struct VarsCtx
{
	const SiftV6Rel *rel;
	bool		values;
} VarsCtx;

static Node *
vars_mutator(Node *n, VarsCtx *ctx)
{
	Oid			typid;
	int			vatt;

	if (n == NULL)
		return NULL;
	if ((vatt = read_vatt(ctx->rel, n, ctx->values, &typid)) != InvalidAttrNumber)
		return (Node *) makeVar(ctx->rel->relid, vatt, typid, -1, exprCollation(n), 0);
	return expression_tree_mutator(n, vars_mutator, ctx);
}

Expr *
sift_v6_json_vars(const SiftV6Rel *rel, const Expr *e, bool values)
{
	VarsCtx		ctx = {rel, values};

	return (Expr *) vars_mutator((Node *) e, &ctx);
}

bool
sift_v6_json_type_col(const SiftV6Rel *rel, const Var *v, SiftV6Col *out)
{
	const int	leaf = sift_v6_vatt_leaf(v->varattno);
	const SiftColSchema *leaves = leaves_of(rel);
	const bool	held = leaves != NULL && leaf < leaves->ncols;

	if (held)
		*out = sift_v6_vatt_col(v->varattno, leaves->cols[leaf].attno, v->vartype, 0);
	return held;
}

typedef struct JsonLeaves
{
	Bitmapset  *value;			/* the leaves the statement reads the values of (`->>`, its float8 cast) */
	Bitmapset  *type;			/* the leaves it reads the JSON type of, anywhere but in a scan key */
} JsonLeaves;

static void
note_col(JsonLeaves *j, const SiftV6Col *c)
{
	if (c->jleaf > 0)
	{
		Bitmapset **set = c->jtype ? &j->type : &j->value;

		*set = bms_add_member(*set, c->jleaf - 1);
	}
}

static void
note_vatt(JsonLeaves *j, int attno)
{
	Bitmapset **set = sift_v6_vatt_is_type(attno) ? &j->type : &j->value;

	*set = bms_add_member(*set, sift_v6_vatt_leaf(attno));
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
	q->args = list_make2(makeVar(INDEX_VAR, SIFT_V6_JTEXT_VATT + col.jleaf - 1, TEXTOID, -1, op->inputcollid, 0), copyObject((Node *) c));
	return (Expr *) q;
}

void
sift_v6_json_scankeys(const SiftV6Spec *spec, Relation index, ScanKey *keys, int *nkeys)
{
	const SiftJsonbOps *ops;
	ScanKey		all;

	if (spec->jsonqual == NIL)
		return;
	ops = sift_store_ops_for_oid(RelationGetRelid(index), false)->jsonb;
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
