/*
 * agg/filter/pg.c - the clause of a FILTER (WHERE ..) from PostgreSQL's quals (v6_filter_pg.h).
 */
/* check-memory: allow-allocs the preds and the arrays of one clause follow the text of one statement, not the rows of the table */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/filter/pg.h"
#include "agg/expr/pg.h"
#include "agg/unnest/jsonb.h"
#include "col/am.h"

typedef struct Build
{
	MemoryContext cx;
	SiftV6Col  *fcol;
	int		   *nfcols;
	bool		add;
	bool		params;			/* $n stands for a constant of its type (param_operand) */
	int			ngroups;
	SiftBytes  *canon;
} Build;

static void
canon_put(Build *b, const void *p, uint32 n)
{
	if (b->canon != NULL)
		sift_bytes_put(b->canon, p, n);
}

static int
fcol_index(Build *b, const Var *v)
{
	for (int i = 0; i < *b->nfcols; i++)
		if (b->fcol[i].derive == 0 && b->fcol[i].attno == v->varattno)
			return i;
	if (!b->add || *b->nfcols == SIFT_V6_MAX_FCOLS)
		return -1;
	b->fcol[*b->nfcols] = sift_v6_vatt_is_json(v->varattno) ? sift_v6_vatt_col(v->varattno, v->varattno, v->vartype, 0) : sift_v6_elem_vatt_col(v->varattno, v->vartype);
	if (v->vartype == NUMERICOID)
		b->fcol[*b->nfcols].scale = sift_v6_numeric_typmod_scale(v->vartypmod);
	return (*b->nfcols)++;
}

static bool
is_int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static SiftPredStr
str_of(Build *b, Datum d)
{
	struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
	const uint32 len = (uint32) VARSIZE_ANY_EXHDR(v);
	uint8	   *copy = MemoryContextAlloc(b->cx, Max(len, 1));

	memcpy(copy, VARDATA_ANY(v), len);
	return (SiftPredStr) {copy, len};
}

static bool
is_text_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
}

static SiftPredOp
op_of(int strategy, bool *ok)
{
	*ok = true;
	switch (strategy)
	{
		case BTLessStrategyNumber:
			return SPO_LT;
		case BTLessEqualStrategyNumber:
			return SPO_LE;
		case BTEqualStrategyNumber:
			return SPO_EQ;
		case BTGreaterEqualStrategyNumber:
			return SPO_GE;
		case BTGreaterStrategyNumber:
			return SPO_GT;
		default:
			*ok = false;
			return SPO_EQ;
	}
}

/* the btree comparison an operator is, whichever the family it is found in */
static bool
btree_op(Oid opno, SiftPredOp *op)
{
	foreach_int(strategy, sift_op_strategies(opno))
	{
		bool		ok;

		*op = op_of(strategy, &ok);
		if (ok)
			return true;
	}
	return false;
}

/* a comparison that holds for a value that is not equal: the negator of an equality */
static bool
filter_op(Oid opno, SiftPredOp *op)
{
	const Oid	neg = get_negator(opno);
	SiftPredOp	eq;

	if (btree_op(opno, op))
		return true;
	if (!OidIsValid(neg) || !btree_op(neg, &eq) || eq != SPO_EQ)
		return false;
	*op = SPO_NE;
	return true;
}

int
sift_v6_numeric_typmod_scale(int32 typmod)
{
	return ((((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024);
}

typedef enum NumPlace
{
	NUM_AT,						/* the scaled integer of a number of the column */
	NUM_INSIDE,					/* between the scaled integer and the next one */
	NUM_ABOVE,					/* above every number of the column (+Infinity too) */
	NUM_BELOW,
	NUM_NAN
} NumPlace;

/* where a numeric constant is among the scaled integers of a numeric(p <= 18, scale) leaf: *key the integer at or below it */
static NumPlace
num_place(int scale, Datum d, int64 *key)
{
	if (numeric_is_nan(DatumGetNumeric(d)))
		return NUM_NAN;
	if (!sift_col_numeric_key_fits(scale, d))
		return DatumGetInt32(DirectFunctionCall2(numeric_cmp, d, NumericGetDatum(int64_to_numeric(0)))) > 0 ? NUM_ABOVE : NUM_BELOW;
	*key = sift_col_numeric_key(scale, d, SIFT_COL_ROUND_DOWN);
	return *key == sift_col_numeric_key(scale, d, SIFT_COL_ROUND_UP) ? NUM_AT : NUM_INSIDE;
}

static bool
num_pred(SiftPred *p, SiftPredOp op, int64 i)
{
	p->op = op;
	p->i = op == SPO_IN || op == SPO_NOT_NULL ? 0 : i;
	return true;
}

/*
 * A comparison of a numeric with a constant as one of its scaled integer (eval.c puts a NaN above every number): a constant between two integers is <= or > the lower one, one
 * past every number is passed by no number, by every one, or by the NaN alone (> PG_INT64_MAX), and a NaN constant equals the NaN alone
 */
static bool
set_numeric(SiftPred *p, int scale, const Const *c)
{
	const SiftPredOp op = p->op;
	const bool	lower = op == SPO_LT || op == SPO_LE;
	int64		key = 0;

	p->kind = SPK_INT;
	if (c->constisnull || c->consttype != NUMERICOID)
		return false;
	switch (num_place(scale, c->constvalue, &key))
	{
		case NUM_AT:
			return num_pred(p, op, key);
		case NUM_INSIDE:
			return num_pred(p, op == SPO_EQ ? SPO_IN : op == SPO_NE ? SPO_NOT_NULL : lower ? SPO_LE : SPO_GT, key);
		case NUM_ABOVE:
			return num_pred(p, op == SPO_EQ ? SPO_IN : op == SPO_NE ? SPO_NOT_NULL : lower ? SPO_LE : SPO_GT, PG_INT64_MAX);
		case NUM_BELOW:
			return num_pred(p, op == SPO_EQ || lower ? SPO_IN : SPO_NOT_NULL, 0);
		case NUM_NAN:
			return num_pred(p, op == SPO_EQ || op == SPO_GE ? SPO_GT : op == SPO_NE || op == SPO_LT ? SPO_LE : op == SPO_LE ? SPO_NOT_NULL : SPO_IN, PG_INT64_MAX);
	}
	return false;
}

/* the kind of pred a column of coltype compared by op with a value of valtype is; false when the scan does not compare them */
static bool
pred_kind(Oid coltype, Oid valtype, SiftPredOp op, SiftPredKind *kind)
{
	switch (sift_v6_class(coltype))
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			*kind = SPK_INT;
			return (is_int_type(coltype) && is_int_type(valtype)) ||
				(coltype == valtype && (coltype == DATEOID || coltype == TIMESTAMPOID || coltype == TIMESTAMPTZOID || coltype == BOOLOID));
		case SV6C_F64:
			*kind = SPK_F64;
			return valtype == FLOAT4OID || valtype == FLOAT8OID;
		case SV6C_TEXT:
			*kind = SPK_STR;
			return is_text_type(valtype) && (op == SPO_EQ || op == SPO_NE || op == SPO_IN);
		default:
			return false;
	}
}

static int64
int_of(const Const *c)
{
	switch (c->consttype)
	{
		case INT2OID:
			return DatumGetInt16(c->constvalue);
		case INT4OID:
			return DatumGetInt32(c->constvalue);
		case DATEOID:
			return DatumGetDateADT(c->constvalue);
		case BOOLOID:
			return DatumGetBool(c->constvalue);
		default:
			return DatumGetInt64(c->constvalue);
	}
}

/* a non-NULL constant of a type pred_kind takes as the value of p, a numeric one as a scaled integer of its column (set_numeric) */
static bool
set_const(Build *b, SiftPred *p, Oid coltype, int scale, const Const *c)
{
	if (coltype == NUMERICOID)
		return set_numeric(p, scale, c);
	if (c->constisnull || !pred_kind(coltype, c->consttype, p->op, &p->kind))
		return false;
	if (p->kind == SPK_INT)
		p->i = int_of(c);
	else if (p->kind == SPK_F64)
		p->f = c->consttype == FLOAT4OID ? DatumGetFloat4(c->constvalue) : DatumGetFloat8(c->constvalue);
	else
		p->s = str_of(b, c->constvalue);
	return true;
}

/*
 * $n of a statement whose node binds its values as constants before it binds the clause (sift_v6_clause_make_params): only its type is known when the plan is made, and its number
 * tells the clause from those of other parameters or of constants
 */
static bool
param_operand(Build *b, const Node *n)
{
	const uint8 tag = 0xFF;

	if (!b->params || !IsA(n, Param) || ((const Param *) n)->paramkind != PARAM_EXTERN)
		return false;
	canon_put(b, &tag, sizeof(tag));
	canon_put(b, &((const Param *) n)->paramid, sizeof(int));
	return true;
}

/* the comparison of a column with NULL, which holds for no row: the empty IN list */
static void
set_no_row(SiftPred *p)
{
	p->op = SPO_IN;
	p->nin = 0;
}

/* the bytes of a literal match where its characters do: at the start of a string in any encoding, anywhere in one whose characters no byte of another character can begin */
static bool
like_shape_exact(SiftLikeShape shape)
{
	return shape == SIFT_LIKE_PREFIX || shape == SIFT_LIKE_EQUAL || (shape != SIFT_LIKE_OTHER && (GetDatabaseEncoding() == PG_UTF8 || pg_database_encoding_max_length() == 1));
}

/* text LIKE 'lit', 'lit%', '%lit' or '%lit%': the literal of the pattern on the bytes of the string */
static bool
pred_like(Build *b, const OpExpr *op, SiftPred *p)
{
	const Var  *v;
	const Const *c;
	SiftPredStr pattern;
	uint8	   *lit;

	if (list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
		return false;
	v = linitial(op->args);
	c = lsecond(op->args);
	if (v->vartype != TEXTOID || c->constisnull || c->consttype != TEXTOID)
		return false;
	pattern = str_of(b, c->constvalue);
	lit = MemoryContextAlloc(b->cx, Max(pattern.len, 1));
	p->like = sift_like_parse(pattern.bytes, pattern.len, lit, &p->s.len);
	if (!like_shape_exact(p->like))
		return false;
	p->op = SPO_LIKE;
	p->kind = SPK_STR;
	p->s.bytes = lit;
	sift_like_needle_borrow(&p->needle, lit, p->s.len, false);
	return (p->vec = fcol_index(b, v)) >= 0;
}

static bool
pred_compare(Build *b, const OpExpr *op, SiftPred *p)
{
	const Var  *v;
	const Node *c;

	if (list_length(op->args) != 2 || !IsA(linitial(op->args), Var))
		return false;
	v = linitial(op->args);
	c = lsecond(op->args);
	if ((p->vec = fcol_index(b, v)) < 0 || !filter_op(op->opno, &p->op))
		return false;
	if (param_operand(b, c))
		return pred_kind(v->vartype, exprType(c), p->op, &p->kind);
	if (!IsA(c, Const))
		return false;
	if (((const Const *) c)->constisnull && pred_kind(v->vartype, exprType(c), p->op, &p->kind))
	{
		set_no_row(p);
		return true;
	}
	return set_const(b, p, v->vartype, b->fcol[p->vec].scale, (const Const *) c);
}

static bool
pred_in(Build *b, const ScalarArrayOpExpr *sa, SiftPred *p)
{
	const Var  *v;
	const Const *c;
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftPredOp	op;
	uint32		k = 0;

	if (list_length(sa->args) != 2 || !IsA(linitial(sa->args), Var))
		return false;
	v = linitial(sa->args);
	if ((p->vec = fcol_index(b, v)) < 0 || !filter_op(sa->opno, &op) || op != (sa->useOr ? SPO_EQ : SPO_NE))
		return false;
	p->op = sa->useOr ? SPO_IN : SPO_NOT_IN;
	if (param_operand(b, lsecond(sa->args)))
		return pred_kind(v->vartype, get_element_type(exprType(lsecond(sa->args))), SPO_EQ, &p->kind);
	if (!IsA(lsecond(sa->args), Const))
		return false;
	c = lsecond(sa->args);
	if (c->constisnull)
	{
		set_no_row(p);
		return pred_kind(v->vartype, get_element_type(c->consttype), SPO_EQ, &p->kind);
	}
	arr = DatumGetArrayTypeP(c->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arr), &typlen, &typbyval, &typalign);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), typlen, typbyval, typalign, &elems, &nulls, &n);
	p->in_i = MemoryContextAllocZero(b->cx, sizeof(int64) * Max(n, 1));
	p->in_f = MemoryContextAllocZero(b->cx, sizeof(double) * Max(n, 1));
	p->in_s = MemoryContextAllocZero(b->cx, sizeof(SiftPredStr) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		Const		e = {.consttype = ARR_ELEMTYPE(arr),.constvalue = elems[i],.constisnull = nulls[i]};
		SiftPred	one = {.op = SPO_EQ};

		if (nulls[i] && !sa->useOr)
		{
			set_no_row(p);
			return pred_kind(v->vartype, ARR_ELEMTYPE(arr), SPO_EQ, &p->kind);
		}
		if (nulls[i])
			continue;
		if (!set_const(b, &one, v->vartype, b->fcol[p->vec].scale, &e) || (one.op != SPO_EQ && one.op != SPO_IN))
			return false;
		if (one.op == SPO_IN)
			continue;
		p->kind = one.kind;
		((int64 *) p->in_i)[k] = one.i;
		((double *) p->in_f)[k] = one.f;
		((SiftPredStr *) p->in_s)[k] = one.s;
		k++;
	}
	p->nin = k;
	if (n == 0 || k == 0)
		p->kind = sift_v6_class(v->vartype) == SV6C_F64 ? SPK_F64 : sift_v6_class(v->vartype) == SV6C_TEXT ? SPK_STR : SPK_INT;
	return true;
}

/* a clause PostgreSQL evaluates over the datums of its columns (a derivation, or an expression of the joined rows): the constant of the test is 1 + the column of the boolean, the rows where it is true */
static bool
pred_derived(Build *b, const BooleanTest *bt, SiftPred *p)
{
	int			vec;

	if (bt->booltesttype != IS_TRUE || !IsA(bt->arg, Const))
		return false;
	vec = DatumGetInt32(((const Const *) bt->arg)->constvalue) - 1;
	if (vec < 0 || vec >= *b->nfcols)
		return false;
	*p = (SiftPred) {.op = SPO_EQ,.kind = SPK_INT,.vec = vec,.i = 1};
	return true;
}

static bool
pred_null_test(Build *b, const NullTest *nt, SiftPred *p)
{
	if (nt->argisrow || !IsA(nt->arg, Var) || (p->vec = fcol_index(b, (const Var *) nt->arg)) < 0)
		return false;
	p->op = nt->nulltesttype == IS_NULL ? SPO_IS_NULL : SPO_NOT_NULL;
	return true;
}

static void
canon_pred(Build *b, const SiftPred *p)
{
	const uint8 head[3] = {(uint8) p->op, (uint8) p->kind, (uint8) p->like};

	canon_put(b, head, sizeof(head));
	canon_put(b, &p->vec, sizeof(p->vec));
	canon_put(b, &p->i, sizeof(p->i));
	canon_put(b, &p->f, sizeof(p->f));
	canon_put(b, &p->s.len, sizeof(p->s.len));
	canon_put(b, p->s.bytes, p->s.len);
	canon_put(b, &p->nin, sizeof(p->nin));
	for (uint32 k = 0; k < p->nin; k++)
	{
		canon_put(b, &p->in_i[k], sizeof(int64));
		canon_put(b, &p->in_f[k], sizeof(double));
		canon_put(b, &p->in_s[k].len, sizeof(uint32));
		canon_put(b, p->in_s[k].bytes, p->in_s[k].len);
	}
}

static bool add_pred(Build *b, const Node *q, int group, SiftClause *out);

/* the arms of an OR (the ORs among them are its arms too) as the preds of one disjunction */
static bool
add_or(Build *b, const BoolExpr *q, int group, SiftClause *out)
{
	if (q->boolop != OR_EXPR)
		return false;
	if (group == 0)
		group = ++b->ngroups;
	foreach_ptr(Node, arm, q->args)
		if (!add_pred(b, arm, group, out))
			return false;
	return true;
}

static bool
add_pred(Build *b, const Node *q, int group, SiftClause *out)
{
	SiftPred   *p = &out->pred[out->npred];
	bool		ok;

	if (IsA(q, BoolExpr))
		return add_or(b, (const BoolExpr *) q, group, out);
	if (out->npred == SIFT_CLAUSE_MAX_PREDS)
		return false;
	*p = (SiftPred) {.group = group};
	switch (nodeTag(q))
	{
		case T_OpExpr:
			ok = get_opcode(((const OpExpr *) q)->opno) == F_TEXTLIKE ? pred_like(b, (const OpExpr *) q, p) : pred_compare(b, (const OpExpr *) q, p);
			break;
		case T_ScalarArrayOpExpr:
			ok = pred_in(b, (const ScalarArrayOpExpr *) q, p);
			break;
		case T_NullTest:
			ok = pred_null_test(b, (const NullTest *) q, p);
			break;
		case T_BooleanTest:
			ok = pred_derived(b, (const BooleanTest *) q, p);
			break;
		default:
			ok = false;
	}
	if (!ok)
		return false;
	if (group != 0)
		canon_put(b, &group, sizeof(group));
	canon_pred(b, p);
	out->npred++;
	return true;
}

static bool
build(Build *b, const List *quals, SiftClause *out)
{
	*out = (SiftClause) {0};
	foreach_ptr(Node, q, quals)
		if (!add_pred(b, q, 0, out))
			return false;
	return true;
}

bool
sift_v6_clause_make(MemoryContext cx, const List *quals, SiftV6Spec *spec, SiftClause *out, SiftBytes *canon)
{
	Build		b = {.cx = cx,.fcol = spec->fcol,.nfcols = &spec->nfcols,.add = true,.canon = canon};

	return build(&b, quals, out);
}

bool
sift_v6_clause_make_params(MemoryContext cx, const List *quals, SiftV6Spec *spec, SiftClause *out, SiftBytes *canon)
{
	Build		b = {.cx = cx,.fcol = spec->fcol,.nfcols = &spec->nfcols,.add = true,.params = true,.canon = canon};

	return build(&b, quals, out);
}

bool
sift_v6_clause_bind(MemoryContext cx, const List *quals, const SiftV6Spec *spec, SiftClause *out)
{
	SiftV6Col	fcol[SIFT_V6_MAX_FCOLS];
	int			nfcols = spec->nfcols;
	Build		b = {.cx = cx,.fcol = fcol,.nfcols = &nfcols,.add = false};

	memcpy(fcol, spec->fcol, sizeof(SiftV6Col) * nfcols);
	return build(&b, quals, out);
}

/*
 * An aggregate whose FILTER (WHERE f) the executor evaluates (it reads an InitPlan or starts a sub-plan, which the constants of a native clause cannot say): the same aggregate of
 * CASE WHEN f THEN arg END, which takes the rows f passes and skips the others as the NULLs of an aggregate are skipped; count(*) counts the 1 of the rows that pass
 */
Aggref *
sift_v6_filter_fold(Aggref *a)
{
	Aggref	   *b = copyObject(a);
	CaseExpr   *ce = makeNode(CaseExpr);
	CaseWhen   *cw = makeNode(CaseWhen);
	Expr	   *arg;

	if (a->aggorder != NIL || a->aggdistinct != NIL || list_length(a->args) > 1 || !sift_v6_pgexpr_exec((Node *) a->aggfilter))
		return NULL;
	arg = a->aggstar ? (Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true) : linitial_node(TargetEntry, a->args)->expr;
	cw->expr = a->aggfilter;
	cw->result = arg;
	cw->location = -1;
	ce->casetype = exprType((Node *) arg);
	ce->casecollid = exprCollation((Node *) arg);
	ce->args = list_make1(cw);
	ce->defresult = (Expr *) makeNullConst(ce->casetype, exprTypmod((Node *) arg), ce->casecollid);
	ce->location = -1;
	b->aggfilter = NULL;
	if (a->aggstar)
	{
		b->aggstar = false;
		b->aggfnoid = F_COUNT_ANY;
		b->aggargtypes = list_make1_oid(INT4OID);
		b->args = list_make1(makeTargetEntry((Expr *) ce, 1, NULL, false));
	}
	else
		linitial_node(TargetEntry, b->args)->expr = (Expr *) ce;
	return b;
}
