/*
 * agg/v6/filter/pg.c - the clause of a FILTER (WHERE ..) from PostgreSQL's quals (v6_filter_pg.h).
 */
/* check-memory: allow-allocs the preds and the arrays of one clause follow the text of one statement, not the rows of the table */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/v6/filter/pg.h"
#include "agg/v6/expr/pg.h"
#include "agg/v6/unnest/jsonb.h"

typedef struct Build
{
	MemoryContext cx;
	SiftV6Col  *fcol;
	int		   *nfcols;
	bool		add;
	bool		or_ok;			/* an OR of preds is a conjunct (the clause of the elements of an unnest: its evaluation is sift_clause_eval alone) */
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
	return (*b->nfcols)++;
}

static bool
is_int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

/* the constant as the integer of a column of the type, for the types the comparison of which is the integer's */
static bool
int_of(Oid coltype, const Const *c, int64 *out)
{
	if (c->constisnull)
		return false;
	if (is_int_type(coltype) && is_int_type(c->consttype))
		*out = c->consttype == INT2OID ? DatumGetInt16(c->constvalue) : c->consttype == INT4OID ? DatumGetInt32(c->constvalue) : DatumGetInt64(c->constvalue);
	else if (coltype == c->consttype && coltype == DATEOID)
		*out = DatumGetDateADT(c->constvalue);
	else if (coltype == c->consttype && (coltype == TIMESTAMPOID || coltype == TIMESTAMPTZOID))
		*out = DatumGetInt64(c->constvalue);
	else if (coltype == c->consttype && coltype == BOOLOID)
		*out = DatumGetBool(c->constvalue);
	else
		return false;
	return true;
}

static bool
f64_of(Datum d, Oid consttype, double *out)
{
	if (consttype == FLOAT4OID)
		*out = DatumGetFloat4(d);
	else if (consttype == FLOAT8OID)
		*out = DatumGetFloat8(d);
	else
		return false;
	return true;
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

static bool
set_const(Build *b, SiftPred *p, Oid coltype, const Const *c)
{
	switch (sift_v6_class(coltype))
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			p->kind = SPK_INT;
			return int_of(coltype, c, &p->i);
		case SV6C_F64:
			p->kind = SPK_F64;
			return !c->constisnull && f64_of(c->constvalue, c->consttype, &p->f);
		case SV6C_TEXT:
			p->kind = SPK_STR;
			if (c->constisnull || !is_text_type(c->consttype) || (p->op != SPO_EQ && p->op != SPO_NE && p->op != SPO_IN))
				return false;
			p->s = str_of(b, c->constvalue);
			return true;
		default:
			return false;
	}
}

/* text LIKE 'abc%': the pattern is literal bytes and one % at its end, which no collation or escape can read another way */
static bool
pred_prefix(Build *b, const OpExpr *op, SiftPred *p)
{
	const Var  *v;
	const Const *c;
	SiftPredStr pattern;

	if (list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
		return false;
	v = linitial(op->args);
	c = lsecond(op->args);
	if (v->vartype != TEXTOID || c->constisnull || c->consttype != TEXTOID)
		return false;
	pattern = str_of(b, c->constvalue);
	if (pattern.len == 0 || pattern.bytes[pattern.len - 1] != '%' || memchr(pattern.bytes, '%', pattern.len - 1) != NULL ||
		memchr(pattern.bytes, '_', pattern.len) != NULL || memchr(pattern.bytes, '\\', pattern.len) != NULL)
		return false;
	p->op = SPO_PREFIX;
	p->kind = SPK_STR;
	p->s = (SiftPredStr) {pattern.bytes, pattern.len - 1};
	return (p->vec = fcol_index(b, v)) >= 0;
}

static bool
pred_compare(Build *b, const OpExpr *op, SiftPred *p)
{
	const Var  *v;
	const Const *c;

	if (list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
		return false;
	v = linitial(op->args);
	c = lsecond(op->args);
	return (p->vec = fcol_index(b, v)) >= 0 && filter_op(op->opno, &p->op) && set_const(b, p, v->vartype, c);
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

	if (list_length(sa->args) != 2 || !IsA(linitial(sa->args), Var) || !IsA(lsecond(sa->args), Const))
		return false;
	v = linitial(sa->args);
	c = lsecond(sa->args);
	if (c->constisnull || (p->vec = fcol_index(b, v)) < 0 || !filter_op(sa->opno, &op) || op != (sa->useOr ? SPO_EQ : SPO_NE))
		return false;
	arr = DatumGetArrayTypeP(c->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arr), &typlen, &typbyval, &typalign);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), typlen, typbyval, typalign, &elems, &nulls, &n);
	p->op = sa->useOr ? SPO_IN : SPO_NOT_IN;
	p->in_i = MemoryContextAllocZero(b->cx, sizeof(int64) * Max(n, 1));
	p->in_f = MemoryContextAllocZero(b->cx, sizeof(double) * Max(n, 1));
	p->in_s = MemoryContextAllocZero(b->cx, sizeof(SiftPredStr) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		Const		e = {.consttype = ARR_ELEMTYPE(arr),.constvalue = elems[i],.constisnull = nulls[i]};
		SiftPred	one = {.op = SPO_EQ};

		if (nulls[i] && !sa->useOr)
			return false;
		if (nulls[i])
			continue;
		if (!set_const(b, &one, v->vartype, &e))
			return false;
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

/* a clause PostgreSQL evaluates over the datums of its columns (the planner's derivation, whose number is the constant of the test): the rows where the derived boolean is true */
static bool
pred_derived(Build *b, const BooleanTest *bt, SiftPred *p)
{
	int			derive;

	if (bt->booltesttype != IS_TRUE || !IsA(bt->arg, Const))
		return false;
	derive = DatumGetInt32(((const Const *) bt->arg)->constvalue);
	for (int i = 0; i < *b->nfcols; i++)
		if (b->fcol[i].derive == derive)
		{
			*p = (SiftPred) {.op = SPO_EQ,.kind = SPK_INT,.vec = i,.i = 1};
			return true;
		}
	return false;
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
	const uint8 head[2] = {(uint8) p->op, (uint8) p->kind};
	const int	attno = b->fcol[p->vec].attno;
	const int	derive = b->fcol[p->vec].derive;

	canon_put(b, head, sizeof(head));
	canon_put(b, &attno, sizeof(attno));
	canon_put(b, &derive, sizeof(derive));
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
	if (!b->or_ok || q->boolop != OR_EXPR)
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
			ok = get_opcode(((const OpExpr *) q)->opno) == F_TEXTLIKE ? pred_prefix(b, (const OpExpr *) q, p) : pred_compare(b, (const OpExpr *) q, p);
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
sift_v6_clause_make_or(MemoryContext cx, const List *quals, SiftV6Spec *spec, SiftClause *out)
{
	Build		b = {.cx = cx,.fcol = spec->fcol,.nfcols = &spec->nfcols,.add = true,.or_ok = true};

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
