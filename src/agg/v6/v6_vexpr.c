/*
 * agg/v6/v6_vexpr.c - the Expr trees PostgreSQL planned (WHERE clauses, expressions of a key or an argument) to the trees of agg/vexpr.h and from there to a program of the machine.
 * What is taken is what the machine gives bit for bit: operators of pg_catalog over int4, int8, float8 and numeric of at most 38 digits (int2 and the date types only compare, as their
 * arithmetic raises other errors), comparisons of one domain (a date is not a timestamp), AND, OR, NOT, IS [NOT] NULL, COALESCE, CASE WHEN, the casts the machine has, abs,
 * date_trunc and extract of a timestamp, a timestamptz (in the zone of the session) or a date, jsonb_exists(doc, key) and boolean columns. Anything else is not compiled and the
 * caller keeps the executor's evaluation of the query.
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "executor/nodeSubplan.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#include "agg/plan/safety.h"
#include "executor/nodeSubplan.h"

#include "agg/v6/v6_vexpr.h"
#include "agg/zoneoff.h"
#include "core/memtrack.h"
#include "core/store_ops.h"

static const SiftV6Format *bound_format;

void
sift_v6_format_bind(const SiftV6Format *fmt)
{
	bound_format = fmt;
}

const SiftV6Format *
sift_v6_format(void)
{
	Assert(bound_format != NULL);
	return bound_format;
}

bool
sift_v6_format_bound(void)
{
	return bound_format != NULL;
}

bool
sift_v6_vec_type(Oid typid, int32 typmod, SiftV6InCol *out)
{
	*out = (SiftV6InCol) {.typid = typid};
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			out->type = SVEC_I32;
			return true;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			out->type = SVEC_I64;
			return true;
		case FLOAT8OID:
			out->type = SVEC_F64;
			return true;
		case NUMERICOID:
			if (typmod < (int32) VARHDRSZ)
				return false;
			out->prec = ((typmod - VARHDRSZ) >> 16) & 0xffff;
			out->scale = (int16) ((((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024);
			out->type = out->prec <= 18 ? SVEC_NUM64 : SVEC_NUM128;
			return out->prec >= 1 && out->prec <= 38 && out->scale >= 0 && out->scale <= out->prec;
		default:
			return false;
	}
}

void
sift_v6_input_init_info(SiftV6Input *in, const IndexOptInfo *ix)
{
	*in = (SiftV6Input) {.indexoid = ix->indexoid,.nkeys = ix->ncolumns};
	for (int i = 0; i < ix->ncolumns; i++)
		in->keys[i] = ix->indexkeys[i];
}

void
sift_v6_input_bind(SiftV6Input *in, EState *estate)
{
	in->estate = estate;
}

void
sift_v6_input_init_rel(SiftV6Input *in, Relation index)
{
	*in = (SiftV6Input) {.indexoid = RelationGetRelid(index),.nkeys = index->rd_index->indnatts};
	for (int i = 0; i < in->nkeys; i++)
		in->keys[i] = index->rd_index->indkey.values[i];
}

void
sift_v6_input_init_table(SiftV6Input *in, Relation tbl)
{
	*in = (SiftV6Input) {.indexoid = RelationGetRelid(tbl),.is_table = true,.nkeys = RelationGetDescr(tbl)->natts};
}

/* the 0-based column of the input that holds the table column attno (an expression column of an index holds none) */
static int
input_col(const SiftV6Input *in, AttrNumber attno)
{
	if (in->is_table)
		return attno > 0 && attno <= in->nkeys ? attno - 1 : -1;
	for (int i = 0; i < in->nkeys && attno > 0; i++)
		if (in->keys[i] == attno)
			return i;
	return -1;
}

static int
input_add(SiftV6Input *in, const Var *v, SiftV6InCol col)
{
	const int	i = input_col(in, v->varattno);

	if (i < 0)
		return -1;
	col.attno = i + 1;
	for (int k = 0; k < in->ncols; k++)
		if (in->col[k].attno == col.attno)
			return k;
	if (in->ncols == SIFT_V6_MAX_COLS || !sift_v6_format()->leaf_holds(in->indexoid, col.attno, sift_v6_class(v->vartype)))
		return -1;
	in->col[in->ncols] = col;
	return in->ncols++;
}

int
sift_v6_input_add_var(SiftV6Input *in, const Var *v)
{
	SiftV6InCol col;

	return sift_v6_vec_type(v->vartype, v->vartypmod, &col) ? input_add(in, v, col) : -1;
}

int
sift_v6_input_add_text(SiftV6Input *in, const Var *v)
{
	return v->vartype == TEXTOID || v->vartype == VARCHAROID ? input_add(in, v, (SiftV6InCol) {.typid = v->vartype,.type = SVEC_STRVIEW}) : -1;
}

typedef enum Domain
{
	DOM_NONE,
	DOM_INT,
	DOM_F64,
	DOM_NUM,
	DOM_DATE,
	DOM_TS,
	DOM_TSTZ
} Domain;

static Domain
domain_of(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return DOM_INT;
		case FLOAT8OID:
			return DOM_F64;
		case NUMERICOID:
			return DOM_NUM;
		case DATEOID:
			return DOM_DATE;
		case TIMESTAMPOID:
			return DOM_TS;
		case TIMESTAMPTZOID:
			return DOM_TSTZ;
		default:
			return DOM_NONE;
	}
}

/* arithmetic only where PostgreSQL's errors are the machine's: int2 raises "smallint out of range", the date types have their own operators */
static bool
arithmetic_ok(const Node *arg)
{
	const Oid	t = exprType(arg);

	return t != INT2OID && (domain_of(t) == DOM_INT || domain_of(t) == DOM_F64 || domain_of(t) == DOM_NUM);
}

typedef struct Comp
{
	MemoryContext cxt;			/* the nodes: dropped when the compile ends */
	MemoryContext keep;			/* what the program keeps: the zone and the keys of the input */
	Index		relid;
	ParamListInfo params;
	SiftV6Input *in;
	ZoneOff    *zone;
	int			nbounds;
	SiftV6Bound bound[SIFT_V6_MAX_BOUNDS];
	int			ninsets;
	SiftV6InSet inset[SIFT_V6_MAX_INSETS];
} Comp;

static SiftVexprNode *conv(Comp *c, const Node *e);

static SiftVexprNode *
node_new(Comp *c, SiftVexprKind kind, SiftVecType type)
{
	SiftVexprNode *n = MemoryContextAllocZero(c->cxt, sizeof(SiftVexprNode));

	n->kind = kind;
	n->type = type;
	return n;
}

/* a node over arguments, NULL when one of them is */
static SiftVexprNode *
node_args(Comp *c, SiftVexprKind kind, SiftVecType type, int nargs, SiftVexprNode *const *args)
{
	SiftVexprNode *n;

	if (nargs > SIFT_VEXPR_MAX_ARGS)
		return NULL;
	n = node_new(c, kind, type);
	for (int i = 0; i < nargs; i++)
		if ((n->args[n->nargs++] = args[i]) == NULL)
			return NULL;
	return n;
}

static SiftVexprNode *
node_args2(Comp *c, SiftVexprKind kind, SiftVecType type, SiftVexprNode *a, SiftVexprNode *b)
{
	SiftVexprNode *const args[] = {a, b};

	return node_args(c, kind, type, 2, args);
}

static bool
is_bool(const SiftVexprNode *n)
{
	return n != NULL && n->type == SVEC_BOOL;
}

static ZoneOff *
zone_of(Comp *c)
{
	if (c->zone == NULL)
		c->zone = zone_open(c->keep, session_timezone);
	return c->zone;
}

static SiftVexprNode *
mask_node(Comp *c, AttrNumber attno, const char *key)
{
	SiftV6Input *in = c->in;
	const int	i = input_col(in, attno);
	SiftVexprNode *n;
	int			m;

	if (i < 0 || !sift_v6_format()->leaf_holds(in->indexoid, i + 1, key == NULL ? SV6C_BOOL : SV6C_NONE))
		return NULL;
	for (m = 0; m < in->nmasks; m++)
		if (in->mask[m].attno == i + 1 && (in->mask[m].key == NULL) == (key == NULL) && (key == NULL || strcmp(in->mask[m].key, key) == 0))
			break;
	if (m == in->nmasks)
	{
		if (m == SIFT_V6_MAX_MASKS)
			return NULL;
		in->mask[m] = (SiftV6InMask) {.attno = i + 1,.key = key == NULL ? NULL : MemoryContextStrdup(c->keep, key)};
		in->nmasks++;
	}
	n = node_new(c, SVX_MASK_IN, SVEC_BOOL);
	n->aux = m;
	return n;
}

static SiftVexprNode *
numeric_const(Comp *c, Datum value, bool isnull, int32 typmod)
{
	SiftV6InCol t;
	SiftVexprNode *n;
	Numeric		num;
	bool		bad = false;
	int			scale;
	int64		pow10 = 1;

	if (isnull)
	{
		if (!sift_v6_vec_type(NUMERICOID, typmod, &t))
			return NULL;
		n = node_new(c, SVX_NULL, t.type);
		n->scale = t.scale;
		return n;
	}
	num = DatumGetNumeric(value);
	if (numeric_is_nan(num) || numeric_is_inf(num))
		return NULL;
	scale = DatumGetInt32(DirectFunctionCall1(numeric_scale, value));
	if (scale < 0 || scale > 18)
		return NULL;
	for (int i = 0; i < scale; i++)
		pow10 *= 10;
	n = node_new(c, SVX_CONST, SVEC_NUM64);
	n->i64 = sift_numeric_int8(sift_numeric_mul(num, int64_to_numeric(pow10), &bad), &bad);
	n->scale = (int16) scale;
	return bad ? NULL : n;
}

static SiftVexprNode *
const_node(Comp *c, Oid typid, int32 typmod, Datum value, bool isnull)
{
	SiftV6InCol t;
	SiftVexprNode *n;

	if (typid == BOOLOID)
	{
		if (isnull)
			return NULL;
		n = node_new(c, SVX_BOOL_CONST, SVEC_BOOL);
		n->flag = DatumGetBool(value);
		return n;
	}
	if (typid == NUMERICOID)
		return numeric_const(c, value, isnull, typmod);
	if (!sift_v6_vec_type(typid, -1, &t))
		return NULL;
	if (isnull)
		return node_new(c, SVX_NULL, t.type);
	n = node_new(c, SVX_CONST, t.type);
	switch (typid)
	{
		case INT2OID:
			n->i64 = DatumGetInt16(value);
			break;
		case INT4OID:
		case DATEOID:
			n->i64 = DatumGetInt32(value);
			break;
		case FLOAT8OID:
			n->f64 = DatumGetFloat8(value);
			break;
		default:
			n->i64 = DatumGetInt64(value);
	}
	return n;
}

/* the value of a PARAM_EXEC of this execution (a sub-select that has not run yet runs now); before it a NULL of its type stands for it */
static SiftVexprNode *
exec_param_node(Comp *c, const Param *p)
{
	EState	   *estate = c->in->estate;
	ParamExecData *prm;

	if (estate == NULL)
		return const_node(c, p->paramtype, p->paramtypmod, (Datum) 0, true);
	prm = &estate->es_param_exec_vals[p->paramid];
	if (prm->execPlan != NULL)
		ExecSetParamPlan(prm->execPlan, GetPerTupleExprContext(estate));
	return const_node(c, p->paramtype, p->paramtypmod, prm->value, prm->isnull);
}

/* the value this execution gives $n; before it (the path being made) a NULL of its type stands for it */
static SiftVexprNode *
param_node(Comp *c, const Param *p)
{
	Datum		value;
	bool		isnull;

	if (p->paramkind == PARAM_EXEC)
		return exec_param_node(c, p);
	if (p->paramkind != PARAM_EXTERN)
		return NULL;
	if (c->params == NULL)
		return const_node(c, p->paramtype, p->paramtypmod, (Datum) 0, true);
	if (!sift_v6_param_value(c->params, p, &value, &isnull))
		return NULL;
	return const_node(c, p->paramtype, p->paramtypmod, value, isnull);
}

static SiftVexprNode *
var_node(Comp *c, const Var *v)
{
	SiftVexprNode *n;
	int			slot;

	if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
		return NULL;
	if (v->vartype == BOOLOID)
		return mask_node(c, v->varattno, NULL);
	if ((slot = sift_v6_input_add_var(c->in, v)) < 0)
		return NULL;
	n = node_new(c, SVX_COL, c->in->col[slot].type);
	n->aux = slot;
	n->scale = c->in->col[slot].scale;
	n->prec = c->in->col[slot].prec;
	return n;
}

static bool
text_const(const Node *e, char **out)
{
	const Const *k = (const Const *) e;

	if (e == NULL || !IsA(e, Const) || k->constisnull || (k->consttype != TEXTOID))
		return false;
	*out = TextDatumGetCString(k->constvalue);
	return true;
}

static SiftVexprNode *
jsonb_exists_node(Comp *c, List *args)
{
	const Var  *doc;
	char	   *key;

	if (list_length(args) != 2 || !IsA(linitial(args), Var))
		return NULL;
	doc = linitial(args);
	if (doc->vartype != JSONBOID || doc->varno != c->relid || doc->varlevelsup != 0 || doc->varattno <= 0 || !text_const(lsecond(args), &key))
		return NULL;
	return mask_node(c, doc->varattno, key);
}

bool
sift_v6_cmp_op_of(const char *name, int *op)
{
	static const struct
	{
		const char *name;
		SiftCmpOp	op;
	}			ops[] = {{"=", SCMP_EQ}, {"<>", SCMP_NE}, {"<", SCMP_LT}, {"<=", SCMP_LE}, {">", SCMP_GT}, {">=", SCMP_GE}};

	for (int i = 0; i < lengthof(ops); i++)
		if (strcmp(name, ops[i].name) == 0)
		{
			*op = ops[i].op;
			return true;
		}
	return false;
}

static SiftVexprNode *
binary_node(Comp *c, const char *name, const Node *l, const Node *r)
{
	SiftVexprNode *a,
			   *b,
			   *n;
	int			cmp;
	const Domain dl = domain_of(exprType(l));

	if (dl == DOM_NONE || dl != domain_of(exprType(r)))
		return NULL;
	if (!sift_v6_cmp_op_of(name, &cmp) && !(arithmetic_ok(l) && arithmetic_ok(r) && strlen(name) == 1 && strchr("+-*/", name[0]) != NULL))
		return NULL;
	if ((a = conv(c, l)) == NULL || (b = conv(c, r)) == NULL)
		return NULL;
	if (sift_v6_cmp_op_of(name, &cmp))
	{
		n = node_args2(c, SVX_CMP, SVEC_BOOL, a, b);
		n->aux = cmp;
		return n;
	}
	return node_args2(c, name[0] == '+' ? SVX_ADD : name[0] == '-' ? SVX_SUB : name[0] == '*' ? SVX_MUL : SVX_DIV, a->type, a, b);
}

static SiftVexprNode *
op_node(Comp *c, const OpExpr *op)
{
	const char *name;
	SiftVexprNode *a;

	if (get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE || (name = get_opname(op->opno)) == NULL)
		return NULL;
	if (strcmp(name, "?") == 0)
		return jsonb_exists_node(c, op->args);
	if (list_length(op->args) == 2)
		return binary_node(c, name, linitial(op->args), lsecond(op->args));
	if (list_length(op->args) != 1 || strcmp(name, "-") != 0 || !arithmetic_ok(linitial(op->args)) || (a = conv(c, linitial(op->args))) == NULL)
		return NULL;
	return node_args(c, SVX_NEG, a->type, 1, &a);
}

static SiftVexprNode *
bool_node(Comp *c, const BoolExpr *be)
{
	SiftVexprNode *acc = NULL;

	foreach_ptr(Node, arg, be->args)
	{
		SiftVexprNode *x = conv(c, arg);

		if (!is_bool(x))
			return NULL;
		if (be->boolop == NOT_EXPR)
			return node_args(c, SVX_NOT, SVEC_BOOL, 1, &x);
		acc = acc == NULL ? x : node_args2(c, be->boolop == AND_EXPR ? SVX_AND : SVX_OR, SVEC_BOOL, acc, x);
		if (acc == NULL)
			return NULL;
	}
	return acc;
}

static SiftVexprNode *
case_node(Comp *c, const CaseExpr *ce)
{
	SiftVexprNode *args[SIFT_VEXPR_MAX_ARGS];
	int			n = 0;

	if (ce->arg != NULL || 2 * list_length(ce->args) + 1 > SIFT_VEXPR_MAX_ARGS)
		return NULL;
	foreach_node(CaseWhen, w, ce->args)
	{
		args[n++] = conv(c, (Node *) w->expr);
		args[n++] = conv(c, (Node *) w->result);
		if (!is_bool(args[n - 2]))
			return NULL;
	}
	if (ce->defresult != NULL)
		args[n++] = conv(c, (Node *) ce->defresult);
	{
		SiftVexprNode *r = node_args(c, SVX_CASE, n > 1 && args[1] != NULL ? args[1]->type : SVEC_I64, n, args);

		if (r != NULL)
		{
			r->aux = list_length(ce->args);
			r->flag = ce->defresult != NULL;
		}
		return r;
	}
}

static SiftVexprNode *
coalesce_node(Comp *c, const CoalesceExpr *ce)
{
	SiftVexprNode *args[SIFT_VEXPR_MAX_ARGS];
	int			n = 0;

	if (list_length(ce->args) < 2 || list_length(ce->args) > SIFT_VEXPR_MAX_ARGS)
		return NULL;
	foreach_ptr(Node, arg, ce->args)
		args[n++] = conv(c, arg);
	return node_args(c, SVX_COALESCE, args[0] != NULL ? args[0]->type : SVEC_I64, n, args);
}

static bool
unit_of(const char *s, int *unit)
{
	static const char *const names[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};

	for (int i = 0; i < lengthof(names); i++)
		if (strcmp(s, names[i]) == 0)
			return (*unit = i, true);
	return false;
}

static bool
field_of(const char *s, int *field)
{
	static const char *const names[] = {"year", "quarter", "month", "day", "dow", "isodow", "doy", "hour", "minute", "epoch"};

	for (int i = 0; i < lengthof(names); i++)
		if (strcmp(s, names[i]) == 0)
			return (*field = i, true);
	return false;
}

/* the date a timestamp or timestamptz is made of by the implicit coercion of a date: date_trunc has no date form, so PostgreSQL reads the date as a timestamptz */
static Node *
date_coerced(Node *e)
{
	const FuncExpr *f = (const FuncExpr *) e;
	const char *name;

	if (!IsA(e, FuncExpr) || list_length(f->args) != 1 || get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE || (name = get_func_name(f->funcid)) == NULL ||
		(strcmp(name, "timestamptz") != 0 && strcmp(name, "timestamp") != 0) || exprType(linitial(f->args)) != DATEOID)
		return NULL;
	return linitial(f->args);
}

/* date_trunc(unit, ts) and extract(field, ts): the zone is the session's for a timestamptz and none for the others; date_trunc of a date is that of its midnight */
static SiftVexprNode *
calendar_node(Comp *c, List *args, bool trunc)
{
	char	   *word;
	int			which;
	Domain		d;
	Node	   *src;
	SiftVexprNode *a,
			   *n;

	if (list_length(args) != 2 || !text_const(linitial(args), &word) || (trunc ? !unit_of(word, &which) : !field_of(word, &which)))
		return NULL;
	d = domain_of(exprType(lsecond(args)));
	src = trunc ? date_coerced(lsecond(args)) : NULL;
	if (!(d == DOM_TS || d == DOM_TSTZ || (!trunc && d == DOM_DATE)) || (a = conv(c, src != NULL ? src : lsecond(args))) == NULL)
		return NULL;
	n = node_args(c, trunc ? SVX_DATE_TRUNC : SVX_EXTRACT, trunc ? SVEC_I64 : SVEC_NUM64, 1, &a);
	n->aux = which;
	n->zone = d == DOM_TSTZ ? zone_of(c) : NULL;
	return n;
}

static SiftVexprNode *
cast_node(Comp *c, SiftVecType to, int scale, SiftVexprNode *a)
{
	SiftVexprNode *n = node_args(c, SVX_CAST, to, 1, &a);

	if (n != NULL)
	{
		n->scale = (int16) scale;
		n->aux = to == SVEC_F64;
	}
	return n;
}

/* col = ANY (constants): the equalities, ORed; no more elements than the machine's program holds, and a NULL array is no array the machine takes */
#define SIFT_V6_ANY_MAX	12

static SiftVexprNode *
any_node(Comp *c, const ScalarArrayOpExpr *sa)
{
	const char *name = get_opname(sa->opno);
	const Node *left = linitial(sa->args);
	const Const *arr = lsecond(sa->args);
	ArrayType  *a;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *el;
	bool	   *nulls;
	int			n;
	SiftVexprNode *acc = NULL;

	if (!sa->useOr || name == NULL || strcmp(name, "=") != 0 || !IsA(arr, Const) || arr->constisnull ||
		get_func_namespace(get_opcode(sa->opno)) != PG_CATALOG_NAMESPACE)
		return NULL;
	a = DatumGetArrayTypeP(arr->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(a), &len, &byval, &align);
	deconstruct_array(a, ARR_ELEMTYPE(a), len, byval, align, &el, &nulls, &n);
	if (n > SIFT_V6_ANY_MAX || domain_of(exprType(left)) == DOM_NONE || domain_of(exprType(left)) != domain_of(ARR_ELEMTYPE(a)))
		return NULL;
	if (n == 0)
	{
		acc = node_new(c, SVX_BOOL_CONST, SVEC_BOOL);
		acc->flag = false;
		return acc;
	}
	for (int i = 0; i < n; i++)
	{
		SiftVexprNode *x = conv(c, left),
				   *k = const_node(c, ARR_ELEMTYPE(a), -1, el[i], nulls[i]),
				   *eq;

		if (x == NULL || k == NULL)
			return NULL;
		eq = node_args2(c, SVX_CMP, SVEC_BOOL, x, k);
		eq->aux = SCMP_EQ;
		acc = acc == NULL ? eq : node_args2(c, SVX_OR, SVEC_BOOL, acc, eq);
	}
	return acc;
}

static SiftVexprNode *
func_node(Comp *c, const FuncExpr *f)
{
	char	   *name;
	SiftVexprNode *a;
	Domain		d;

	if (get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE || (name = get_func_name(f->funcid)) == NULL)
		return NULL;
	if (strcmp(name, "jsonb_exists") == 0)
		return jsonb_exists_node(c, f->args);
	if (strcmp(name, "date_trunc") == 0 || strcmp(name, "extract") == 0)
		return calendar_node(c, f->args, name[0] == 'd');
	if (list_length(f->args) != 1)
		return NULL;
	d = domain_of(exprType(linitial(f->args)));
	if (strcmp(name, "abs") == 0 && arithmetic_ok(linitial(f->args)))
		return (a = conv(c, linitial(f->args))) ? node_args(c, SVX_ABS, a->type, 1, &a) : NULL;
	if ((d != DOM_INT && !(d == DOM_NUM && strcmp(name, "float8") == 0)) || (a = conv(c, linitial(f->args))) == NULL)
		return NULL;
	if (strcmp(name, "int8") == 0 && a->type == SVEC_I32)
		return cast_node(c, SVEC_I64, 0, a);
	if (strcmp(name, "float8") == 0)
		return cast_node(c, SVEC_F64, 0, a);
	if (strcmp(name, "numeric") == 0 && exprType(linitial(f->args)) != INT2OID)
		return cast_node(c, a->type == SVEC_I32 ? SVEC_NUM64 : SVEC_NUM128, 0, a);
	return NULL;
}

static SiftVexprNode *
conv(Comp *c, const Node *e)
{
	SiftVexprNode *a;

	if (e == NULL)
		return NULL;
	check_stack_depth();
	switch (nodeTag(e))
	{
		case T_RestrictInfo:
			return conv(c, (Node *) ((const RestrictInfo *) e)->clause);
		case T_Var:
			return var_node(c, (const Var *) e);
		case T_Const:
			return const_node(c, ((const Const *) e)->consttype, ((const Const *) e)->consttypmod, ((const Const *) e)->constvalue, ((const Const *) e)->constisnull);
		case T_Param:
			return param_node(c, (const Param *) e);
		case T_RelabelType:
			return domain_of(exprType(e)) == domain_of(exprType((Node *) ((const RelabelType *) e)->arg)) ? conv(c, (Node *) ((const RelabelType *) e)->arg) : NULL;
		case T_OpExpr:
			return op_node(c, (const OpExpr *) e);
		case T_ScalarArrayOpExpr:
			return any_node(c, (const ScalarArrayOpExpr *) e);
		case T_FuncExpr:
			return func_node(c, (const FuncExpr *) e);
		case T_BoolExpr:
			return bool_node(c, (const BoolExpr *) e);
		case T_NullTest:
			if (((const NullTest *) e)->argisrow || (a = conv(c, (Node *) ((const NullTest *) e)->arg)) == NULL)
				return NULL;
			return node_args(c, ((const NullTest *) e)->nulltesttype == IS_NULL ? SVX_IS_NULL : SVX_IS_NOT_NULL, SVEC_BOOL, 1, &a);
		case T_CaseExpr:
			return case_node(c, (const CaseExpr *) e);
		case T_CoalesceExpr:
			return coalesce_node(c, (const CoalesceExpr *) e);
		default:
			return NULL;
	}
}

static bool
finish(Comp *c, SiftVexprNode *root, SiftV6Vexpr *out)
{
	bool		ok;

	*out = (SiftV6Vexpr) {.zone = c->zone,.nbounds = c->nbounds,.ninsets = c->ninsets};
	memcpy(out->bound, c->bound, sizeof(out->bound));
	memcpy(out->inset, c->inset, sizeof(out->inset));
	ok = (root != NULL || c->ninsets > 0) && (root == NULL || sift_vexpr_compile(root, &out->prog));
	MemoryContextDelete(c->cxt);
	return ok;
}

static Comp
comp_start(MemoryContext keep, Index relid, ParamListInfo params, SiftV6Input *in)
{
	return (Comp) {.cxt = AllocSetContextCreate(CurrentMemoryContext, "sift v6 vexpr nodes", ALLOCSET_SMALL_SIZES),.keep = keep,.relid = relid,.params = params,.in = in};
}

bool
sift_v6_vexpr_compile(MemoryContext cxt, const Node *e, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out)
{
	Comp		c = comp_start(cxt, relid, params, in);

	return finish(&c, conv(&c, e), out);
}

bool
sift_v6_vexpr_quals(MemoryContext cxt, const List *clauses, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out)
{
	Comp		c = comp_start(cxt, relid, params, in);
	SiftVexprNode *root = NULL;

	foreach_ptr(Node, clause, (List *) clauses)
	{
		SiftVexprNode *x;

		sift_v6_bounds_of(clause, c.relid, c.params, c.in, &c.nbounds, c.bound);
		if (c.ninsets < SIFT_V6_MAX_INSETS && sift_v6_inset_of(cxt, clause, c.relid, c.params, c.in, &c.inset[c.ninsets]))
		{
			c.ninsets++;
			continue;
		}
		x = conv(&c, clause);
		if (!is_bool(x))
		{
			c.ninsets = 0;
			return finish(&c, NULL, out);
		}
		root = root == NULL ? x : node_args2(&c, SVX_AND, SVEC_BOOL, root, x);
	}
	if (root == NULL && clauses == NIL)
	{
		*out = (SiftV6Vexpr) {0};
		MemoryContextDelete(c.cxt);
		return true;
	}
	return finish(&c, root, out) && (root == NULL || out->prog.out_is_bool);
}

uint16
sift_v6_vexpr_filter(SiftVexprCtx *ctx, const SiftV6Vexpr *quals, const SiftV6Batch *b, uint64 *out)
{
	const int	words = (b->n + 63) / 64;
	uint16		left = 0;

	memset(out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	if (b->mask != NULL)
		memcpy(out, b->mask, words * sizeof(uint64));
	else
		for (int i = 0; i < b->n; i++)
			out[i / 64] |= UINT64_C(1) << (i % 64);
	if (quals != NULL && quals->prog.n_ops > 0)
	{
		const SiftVexprIn in = {.cols = b->col,.masks = b->masks};

		left = sift_vexpr_filter(ctx, &quals->prog, &in, out, b->n);
	}
	else
		for (int w = 0; w < words; w++)
			left += pg_popcount64(out[w]);
	for (int i = 0; quals != NULL && i < quals->ninsets; i++)
		left = sift_v6_inset_filter(&quals->inset[i], &b->col[quals->inset[i].slot], b->n, out, left);
	return left;
}

void
sift_v6_explain_index(ExplainState *es, Oid indexoid)
{
	ExplainPropertyText("Index", get_rel_name(indexoid), es);
}

void
sift_v6_explain_count(ExplainState *es, const char *label, int64 value)
{
	ExplainPropertyInteger(label, NULL, value, es);
}

bool
sift_v6_base_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	if (rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL || agg_query_has_rls(root))
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL &&
		(rel->indexlist != NIL || sift_store_ops_for_scan(rte->relid, true) != NULL);
}

void
sift_v6_add_path(RelOptInfo *output_rel, const CustomPathMethods *methods, double startup, double total, double rows, List *pathkeys, List *custom_private)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.rows = clamp_row_est(rows);
	cp->path.startup_cost = startup;
	cp->path.total_cost = total;
	cp->path.pathkeys = pathkeys;
	cp->methods = methods;
	cp->custom_private = custom_private;
	add_path(output_rel, &cp->path);
}

void
sift_v6_stage_target(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *output_rel)
{
	output_rel->reltarget = root->upper_targets[stage];
}

Plan *
sift_v6_plan_scan(PlannerInfo *root, CustomPath *best, List *tlist, const CustomScanMethods *methods, Oid indexoid)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->custom_private = best->custom_private;
	cs->methods = methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, indexoid);
	return &cs->scan.plan;
}

List *
sift_v6_scan_tlist(const List *exprs, Index relid, NodeTag extra)
{
	List	   *tlist = NIL;
	Bitmapset  *seen_vars = NULL;

	foreach_ptr(Node, n, pull_var_clause((Node *) exprs, PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS))
	{
		const bool	var = IsA(n, Var) && ((Var *) n)->varno == relid && ((Var *) n)->varlevelsup == 0 && bms_is_empty(((Var *) n)->varnullingrels);
		const int	bit = var ? ((Var *) n)->varattno - FirstLowInvalidHeapAttributeNumber : 0;

		if (nodeTag(n) != extra && (!IsA(n, Var) || ((Var *) n)->varno != relid || ((Var *) n)->varlevelsup != 0))
			return NIL;
		if (var ? bms_is_member(bit, seen_vars) : tlist_member((Expr *) n, tlist) != NULL)
			continue;
		seen_vars = var ? bms_add_member(seen_vars, bit) : seen_vars;
		tlist = lappend(tlist, makeTargetEntry((Expr *) copyObject(n), list_length(tlist) + 1, NULL, false));
	}
	return tlist;
}

Datum
sift_v6_numeric_from_i128(SiftI128 v, int scale)
{
	char		digits[48];
	char		text[64];
	bool		neg = v.hi < 0;
	uint64		hi = (uint64) v.hi,
				lo = v.lo;
	int			n = 0;

	if (neg)
	{
		lo = ~lo + 1;
		hi = ~hi + (lo == 0);
	}
	while (hi != 0 || lo != 0)
	{
		const unsigned __int128 cur = ((unsigned __int128) hi << 64) | lo;

		digits[n++] = (char) ('0' + (int) (cur % 10));
		hi = (uint64) ((cur / 10) >> 64);
		lo = (uint64) (cur / 10);
	}
	while (n <= scale)
		digits[n++] = '0';
	{
		int			o = 0;

		if (neg)
			text[o++] = '-';
		for (int i = n - 1; i >= 0; i--)
		{
			text[o++] = digits[i];
			if (i == scale && scale > 0)
				text[o++] = '.';
		}
		text[o] = '\0';
	}
	return DirectFunctionCall3(numeric_in, CStringGetDatum(text), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

void
sift_v6_store_start(SiftV6Store *s, EState *estate)
{
	MemoryContext old;

	s->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 run");
	s->row_cx = AllocSetContextCreate(s->cx, "sift agg v6 tuple", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(estate->es_query_cxt);
	s->store = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(old);
}

void
sift_v6_store_put(SiftV6Store *s, TupleDesc desc, const Datum *values, const bool *isnull)
{
	tuplestore_putvalues(s->store, desc, (Datum *) values, (bool *) isnull);
	MemoryContextReset(s->row_cx);
}

TupleTableSlot *
sift_v6_store_get(SiftV6Store *s, TupleTableSlot *slot)
{
	return tuplestore_gettupleslot(s->store, true, false, slot) ? slot : NULL;
}

void
sift_v6_store_end(SiftV6Store *s)
{
	if (s->store != NULL)
		tuplestore_end(s->store);
	if (s->cx != NULL)
		MemoryContextDelete(s->cx);
	*s = (SiftV6Store) {0};
}

void
sift_v6_store_rescan(SiftV6Store *s, bool params_changed)
{
	if (params_changed)
		sift_v6_store_end(s);
	else if (s->store != NULL)
		tuplestore_rescan(s->store);
}
