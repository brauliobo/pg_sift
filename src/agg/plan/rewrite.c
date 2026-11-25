/*
 * agg/plan/rewrite.c - the rewrites of the planner of SiftAggV6 (agg/plan/rewrite.h).
 */
/* check-memory: allow-allocs every node here follows the expressions of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_namespace_d.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"

#include "agg/expr/num.h"
#include "agg/group/topk.h"
#include "agg/plan/rewrite.h"
#include "agg/datum.h"

static bool
func_named(Oid funcid, const char *name)
{
	char	   *fname = get_func_name(funcid);

	return fname != NULL && get_func_namespace(funcid) == PG_CATALOG_NAMESPACE && strcmp(fname, name) == 0;
}

/* ---- SUM(x + C) ---- */

/* sum(x + C) or sum(x - C) with x an int2 column and C an int4 constant small enough that x + C cannot overflow int4: x and the delta */
static bool
offset_sum_of(const Aggref *a, const Var **x, int64 *delta)
{
	const OpExpr *op;
	const Const *k;
	const char *fn;

	if (a->aggfnoid == F_SUM_INT2 || !func_named(a->aggfnoid, "sum") || a->aggfilter != NULL || a->aggdistinct != NIL || a->aggorder != NIL || list_length(a->args) != 1)
		return false;
	op = (const OpExpr *) ((const TargetEntry *) linitial(a->args))->expr;
	if (!IsA(op, OpExpr) || list_length(op->args) != 2 || get_opname(op->opno) == NULL)
		return false;
	fn = get_func_name(get_opcode(op->opno));
	if (fn == NULL)
		return false;
	if (strcmp(fn, "int24pl") == 0 && IsA(linitial(op->args), Var) && IsA(lsecond(op->args), Const))
	{
		*x = linitial(op->args);
		k = lsecond(op->args);
		*delta = k->constisnull ? 0 : DatumGetInt32(k->constvalue);
	}
	else if (strcmp(fn, "int42pl") == 0 && IsA(linitial(op->args), Const) && IsA(lsecond(op->args), Var))
	{
		*x = lsecond(op->args);
		k = linitial(op->args);
		*delta = k->constisnull ? 0 : DatumGetInt32(k->constvalue);
	}
	else if (strcmp(fn, "int24mi") == 0 && IsA(linitial(op->args), Var) && IsA(lsecond(op->args), Const))
	{
		*x = linitial(op->args);
		k = lsecond(op->args);
		*delta = k->constisnull ? 0 : -(int64) DatumGetInt32(k->constvalue);
	}
	else
		return false;
	return !k->constisnull && (*x)->vartype == INT2OID && (*x)->varlevelsup == 0 && *delta <= (int64) PG_INT32_MAX - PG_INT16_MAX && *delta >= (int64) PG_INT32_MIN - PG_INT16_MIN;
}

static Oid
int8_op(const char *name)
{
	return OpernameGetOprid(list_make1(makeString((char *) name)), INT8OID, INT8OID);
}

static Expr *
int8_opexpr(const char *name, Expr *l, Expr *r, Oid rettype)
{
	OpExpr	   *e = (OpExpr *) make_opclause(int8_op(name), rettype, false, l, r, InvalidOid, InvalidOid);

	e->opfuncid = get_opcode(e->opno);
	return (Expr *) e;
}

/* CASE WHEN count(x) = 0 THEN NULL ELSE sum(x) + delta * count(x) END, over the aggregates sum(x) and count(x) of an int2 column */
static Node *
collapsed(const Aggref *a, const Var *x, int64 delta)
{
	Aggref	   *sum = copyObject((Aggref *) a),
			   *cnt = copyObject((Aggref *) a);
	CaseWhen   *w = makeNode(CaseWhen);
	CaseExpr   *c = makeNode(CaseExpr);

	sum->aggfnoid = F_SUM_INT2;
	sum->aggargtypes = list_make1_oid(INT2OID);
	sum->args = list_make1(makeTargetEntry((Expr *) copyObject(x), 1, NULL, false));
	sum->aggno = sum->aggtransno = 0;
	cnt->aggno = cnt->aggtransno = 1;
	cnt->aggfnoid = F_COUNT_ANY;
	cnt->aggtype = INT8OID;
	cnt->aggtranstype = INT8OID;
	cnt->aggargtypes = list_make1_oid(INT2OID);
	cnt->args = list_make1(makeTargetEntry((Expr *) copyObject(x), 1, NULL, false));
	w->expr = int8_opexpr("=", (Expr *) copyObject(cnt), (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true), BOOLOID);
	w->result = (Expr *) makeNullConst(INT8OID, -1, InvalidOid);
	c->casetype = INT8OID;
	c->args = list_make1(w);
	c->defresult = int8_opexpr("+", (Expr *) sum,
							   int8_opexpr("*", (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(delta), false, true), (Expr *) cnt, INT8OID), INT8OID);
	return (Node *) c;
}

static Node *
collapse_mutator(Node *n, void *context)
{
	const Var  *x;
	int64		delta;

	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref) && offset_sum_of((const Aggref *) n, &x, &delta))
		return collapsed((const Aggref *) n, x, delta);
	return expression_tree_mutator(n, collapse_mutator, context);
}

Node *
sift_v6_collapse_offset_sums(Node *expr)
{
	return collapse_mutator(expr, NULL);
}

/* ---- the first key plus a constant ---- */

const Var *
sift_v6_affine_of(const Expr *e, int64 *delta, Oid *type)
{
	const OpExpr *op = (const OpExpr *) e;
	const char *fn;
	const Var  *v;
	const Const *k;
	bool		plus;

	if (!IsA(e, OpExpr) || list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
		return NULL;
	fn = get_func_name(get_opcode(op->opno));
	if (fn == NULL || !((plus = strcmp(fn, "int4pl") == 0) || strcmp(fn, "int4mi") == 0 || (plus = strcmp(fn, "int8pl") == 0) || strcmp(fn, "int8mi") == 0))
		return NULL;
	v = linitial(op->args);
	k = lsecond(op->args);
	if (k->constisnull || v->varlevelsup != 0 || (v->vartype != INT4OID && v->vartype != INT8OID) || k->consttype != v->vartype)
		return NULL;
	*type = v->vartype;
	*delta = v->vartype == INT4OID ? DatumGetInt32(k->constvalue) : DatumGetInt64(k->constvalue);
	if (!plus)
	{
		if (*delta == PG_INT64_MIN || (v->vartype == INT4OID && *delta == PG_INT32_MIN))
			return NULL;
		*delta = -*delta;
	}
	return v;
}

void
sift_v6_affine_range(Oid type, int64 delta, int64 *lo, int64 *hi)
{
	const int64 tmin = type == INT4OID ? PG_INT32_MIN : PG_INT64_MIN,
				tmax = type == INT4OID ? PG_INT32_MAX : PG_INT64_MAX;

	if (delta > 0)
		*hi = Min(*hi, tmax - delta);
	else if (delta < 0)
		*lo = Max(*lo, tmin - delta);
}

/* ---- date_trunc ---- */

bool
sift_v6_date_trunc_bin(const SiftV6Resolver *rs, const Expr *e, SiftV6Col *src, int64 *stride_us)
{
	static const struct
	{
		const char *unit;
		int64		us;
	}			units[] = {{"microseconds", 1}, {"milliseconds", 1000}, {"second", 1000000}, {"minute", 60000000}, {"hour", 3600000000}, {"day", 86400000000}};
	const FuncExpr *f = (const FuncExpr *) e;
	const Const *unit;

	if (!IsA(e, FuncExpr) || list_length(f->args) != 2 || !func_named(f->funcid, "date_trunc") || !IsA(linitial(f->args), Const))
		return false;
	unit = linitial(f->args);
	if (unit->constisnull || !rs->column(rs->ctx, lsecond(f->args), false, src) || src->typid != TIMESTAMPOID)
		return false;
	for (int i = 0; i < lengthof(units); i++)
		if (strcmp(TextDatumGetCString(unit->constvalue), units[i].unit) == 0)
		{
			*stride_us = units[i].us;
			return true;
		}
	return false;
}

/* ---- + - * of float8 ---- */

typedef struct Arith
{
	const SiftV6Resolver *rs;
	SiftV6Derive *d;
	int			ncols;			/* the columns so far: the source and the conditions */
	int			ntok;
	bool		has_const;
	int			depth;
	int			max_depth;
} Arith;

static bool
arith_token(Arith *a, uint8 tok)
{
	if (a->ntok == SIFT_ARITH_MAX_TOKENS)
		return false;
	a->d->cval[a->ntok / 8] |= (int64) tok << (8 * (a->ntok % 8));
	a->ntok++;
	return true;
}

static SiftV6Col *
arith_slot(Arith *a, int i)
{
	return i == 0 ? &a->d->src : &a->d->cond[i - 1];
}

static bool
arith_col(Arith *a, const Expr *e)
{
	SiftV6Col	col;
	int			at;

	if (!a->rs->column(a->rs->ctx, e, false, &col) || (col.typid != FLOAT8OID && col.typid != INT2OID && col.typid != INT4OID && col.typid != INT8OID))
		return false;
	for (at = 0; at < a->ncols; at++)
		if (memcmp(arith_slot(a, at), &col, sizeof(col)) == 0)
			break;
	if (at == a->ncols)
	{
		if (a->ncols == SIFT_V6_MAX_CASE + 1)
			return false;
		*arith_slot(a, a->ncols++) = col;
	}
	return ++a->depth <= SIFT_ARITH_MAX_STACK && arith_token(a, (uint8) (SIFT_ARITH_COL + at)) && (a->max_depth = Max(a->max_depth, a->depth)) > 0;
}

static bool
arith_walk(Arith *a, const Expr *e)
{
	const OpExpr *op = (const OpExpr *) e;
	const FuncExpr *f = (const FuncExpr *) e;
	const Const *c = (const Const *) e;

	if (IsA(e, OpExpr) && list_length(op->args) == 2)
	{
		const Oid	fn = get_opcode(op->opno);

		if ((fn != F_FLOAT8PL && fn != F_FLOAT8MI && fn != F_FLOAT8MUL) || !arith_walk(a, linitial(op->args)) || !arith_walk(a, lsecond(op->args)))
			return false;
		a->depth--;
		return arith_token(a, fn == F_FLOAT8PL ? SIFT_ARITH_ADD : fn == F_FLOAT8MI ? SIFT_ARITH_SUB : SIFT_ARITH_MUL);
	}
	if (IsA(e, FuncExpr) && list_length(f->args) == 1 && (f->funcid == F_FLOAT8_INT2 || f->funcid == F_FLOAT8_INT4 || f->funcid == F_FLOAT8_INT8))
		return arith_col(a, linitial(f->args));
	if (IsA(e, Const) && !c->constisnull && c->consttype == FLOAT8OID && !a->has_const)
	{
		const double v = DatumGetFloat8(c->constvalue);

		a->has_const = true;
		memcpy(&a->d->aux, &v, sizeof(v));
		a->max_depth = Max(a->max_depth, ++a->depth);
		return arith_token(a, SIFT_ARITH_CONST);
	}
	return IsA(e, Var) && ((const Var *) e)->vartype == FLOAT8OID && arith_col(a, e);
}

/* e is float8 arithmetic over at most five columns of the batches: an operation of at least one operator, so that a column alone stays the column */
static bool
arith_of(const SiftV6Resolver *rs, const Expr *e, SiftV6Derive *d)
{
	Arith		a = {.rs = rs,.d = d};

	if (!rs->arith_ok || !IsA(e, OpExpr) || exprType((const Node *) e) != FLOAT8OID || !arith_walk(&a, e) || a.ncols == 0 || a.depth != 1 || a.ntok > 8 * (a.ncols - 1))
		return false;
	d->ncond = a.ncols - 1;
	return true;
}

/* ---- the computed keys and arguments ---- */

static bool
text_const(const Expr *e, const char *want)
{
	const Const *c = (const Const *) e;

	return IsA(e, Const) && !c->constisnull && strcmp(TextDatumGetCString(c->constvalue), want) == 0;
}

static bool
utf8_or_single_byte(void)
{
	return GetDatabaseEncoding() == PG_UTF8 || pg_database_encoding_max_length() == 1;
}

/* regexp_replace(text column, '^https?://(?:www\.)?([^/]+)/.*$', '\1') with no flags */
static bool
host_of(const SiftV6Resolver *rs, const FuncExpr *f, SiftV6Derive *d)
{
	return list_length(f->args) == 3 && func_named(f->funcid, "regexp_replace") && text_const(lsecond(f->args), "^https?://(?:www\\.)?([^/]+)/.*$") &&
		text_const(lthird(f->args), "\\1") && rs->column(rs->ctx, linitial(f->args), false, &d->src) && sift_v6_class(d->src.typid) == SV6C_TEXT;
}

/* extract(minute | hour FROM timestamp column) */
static bool
extract_of(const SiftV6Resolver *rs, const FuncExpr *f, SiftV6Derive *d)
{
	if (list_length(f->args) != 2 || !func_named(f->funcid, "extract") || !rs->column(rs->ctx, lsecond(f->args), false, &d->src) || d->src.typid != TIMESTAMPOID)
		return false;
	d->aux = text_const(linitial(f->args), "minute") ? 60000000 : text_const(linitial(f->args), "hour") ? 3600000000 : 0;
	return d->aux != 0;
}

/* CASE WHEN c1 = k1 [AND ..] THEN text column ELSE text constant END, the conditions on integer columns */
static bool
case_of(const SiftV6Resolver *rs, const CaseExpr *c, SiftV6Derive *d)
{
	const CaseWhen *w;
	const Const *els;
	List	   *conds;
	text	   *t;

	if (c->arg != NULL || list_length(c->args) != 1 || c->defresult == NULL || !IsA(c->defresult, Const))
		return false;
	w = linitial(c->args);
	els = (const Const *) c->defresult;
	if (els->constisnull || !rs->column(rs->ctx, w->result, false, &d->src) || sift_v6_class(d->src.typid) != SV6C_TEXT)
		return false;
	t = DatumGetTextPP(els->constvalue);
	d->else_len = VARSIZE_ANY_EXHDR(t);
	if (d->else_len > SIFT_V6_ELSE_BYTES)
		return false;
	memcpy(d->else_bytes, VARDATA_ANY(t), d->else_len);
	conds = IsA(w->expr, BoolExpr) && ((const BoolExpr *) w->expr)->boolop == AND_EXPR ? ((const BoolExpr *) w->expr)->args : list_make1(w->expr);
	foreach_ptr(Expr, q, conds)
	{
		const OpExpr *op = (const OpExpr *) q;
		const Const *k;
		const char *fn;

		if (!IsA(q, OpExpr) || list_length(op->args) != 2 || d->ncond == SIFT_V6_MAX_CASE || !IsA(lsecond(op->args), Const))
			return false;
		fn = get_func_name(get_opcode(op->opno));
		k = lsecond(op->args);
		if (k->constisnull || fn == NULL || strlen(fn) < 4 || strcmp(fn + strlen(fn) - 2, "eq") != 0 || strncmp(fn, "int", 3) != 0 ||
			!rs->column(rs->ctx, linitial(op->args), false, &d->cond[d->ncond]) || sift_v6_class(d->cond[d->ncond].typid) != SV6C_I32)
			return false;
		d->cval[d->ncond++] = k->consttype == INT2OID ? DatumGetInt16(k->constvalue) : k->consttype == INT4OID ? DatumGetInt32(k->constvalue) : DatumGetInt64(k->constvalue);
	}
	return d->ncond > 0;
}

bool
sift_v6_derive_of(const SiftV6Resolver *rs, const Expr *e, bool key, SiftV6Derive *d, Oid *restype)
{
	const FuncExpr *f = (const FuncExpr *) e;

	*d = (SiftV6Derive) {0};
	if (key && IsA(e, FuncExpr) && host_of(rs, f, d))
		d->kind = SVD_URL_HOST;
	else if (key && IsA(e, FuncExpr) && extract_of(rs, f, d))
		d->kind = SVD_EXTRACT;
	else if (key && IsA(e, CaseExpr) && case_of(rs, (const CaseExpr *) e, d))
		d->kind = SVD_CASE_STR;
	else if (!key && arith_of(rs, e, d))
		d->kind = SVD_ARITH;
	else if (!key && IsA(e, FuncExpr) && list_length(f->args) == 1 && func_named(f->funcid, "length") && utf8_or_single_byte() &&
			 rs->column(rs->ctx, linitial(f->args), false, &d->src) && sift_v6_class(d->src.typid) == SV6C_TEXT)
	{
		d->kind = SVD_STRLEN;
		d->utf8 = GetDatabaseEncoding() == PG_UTF8;
	}
	else if (!key && sift_vn_derive_of(rs, e, d))
		d->kind = SVD_NUM_EXPR;
	else
		return false;
	*restype = exprType((const Node *) e);
	return true;
}

/* ---- the spec in the plan ---- */

static List *
put64(List *ints, int64 v)
{
	return lappend_int(lappend_int(ints, (int) (uint32) (uint64) v), (int) (uint32) ((uint64) v >> 32));
}

static int64
get64(const List *ints, int at)
{
	return (int64) (((uint64) (uint32) list_nth_int(ints, at + 1) << 32) | (uint32) list_nth_int(ints, at));
}

List *
sift_v6_derive_pack(List *ints, const SiftV6Spec *s)
{
	ints = lappend_int(ints, s->nderive);
	for (int i = 0; i < s->nderive; i++)
	{
		const SiftV6Derive *d = &s->derive[i];

		ints = list_concat(ints, list_make5_int(d->kind, d->src.attno, (int) d->src.typid, d->ncond, d->else_len));
		ints = list_concat(ints, list_make5_int((int) d->src.src, d->src.slot, d->src.scale, d->src.jleaf, (int) d->src.jtype));
		ints = put64(lappend_int(ints, d->utf8), d->aux);
		for (int c = 0; c < d->ncond; c++)
			ints = put64(list_concat(lappend_int(lappend_int(ints, d->cond[c].attno), (int) d->cond[c].typid), list_make4_int((int) d->cond[c].src, d->cond[c].slot, d->cond[c].jleaf, (int) d->cond[c].jtype)), d->cval[c]);
		for (int b = 0; b < SIFT_V6_ELSE_BYTES; b++)
			ints = lappend_int(ints, d->else_bytes[b]);
	}
	for (int i = 0; i < s->nkeys; i++)
		ints = lappend_int(ints, s->key[i].derive);
	for (int j = 0; j < s->naggs; j++)
		ints = lappend_int(ints, s->agg[j].arg.derive);
	for (int i = 0; i < s->nfcols; i++)
		ints = lappend_int(ints, s->fcol[i].derive);
	ints = lappend_int(lappend_int(ints, s->affine), (int) s->aff_type);
	ints = put64(put64(ints, s->aff_lo), s->aff_hi);
	ints = lappend_int(lappend_int(lappend_int(lappend_int(ints, s->rows), (int) s->groups_est), (int) s->topk), s->ntopk);
	for (int i = 0; i < s->ntopk; i++)
		ints = list_concat(ints, list_make4_int(s->topk_item[i].pos, (int) s->topk_item[i].sortop, (int) s->topk_item[i].collid, s->topk_item[i].nulls_first));
	ints = lappend_int(ints, s->nout);
	for (int i = 0; i < s->nout; i++)
		ints = list_concat(ints, list_make2_int(s->out[i].from, s->out[i].at));
	return lappend_int(lappend_int(ints, (int) s->lock_k), s->having);
}

void
sift_v6_derive_unpack(SiftV6Spec *s, const List *ints, int at)
{
	s->nderive = list_nth_int(ints, at++);
	for (int i = 0; i < s->nderive; i++)
	{
		SiftV6Derive *d = &s->derive[i];

		d->kind = list_nth_int(ints, at);
		d->src = (SiftV6Col) {.attno = list_nth_int(ints, at + 1),.typid = (Oid) list_nth_int(ints, at + 2),.src = (SiftV6Src) list_nth_int(ints, at + 5),.slot = list_nth_int(ints, at + 6),
			.scale = list_nth_int(ints, at + 7),.jleaf = list_nth_int(ints, at + 8),.jtype = list_nth_int(ints, at + 9) != 0};
		d->ncond = list_nth_int(ints, at + 3);
		d->else_len = list_nth_int(ints, at + 4);
		d->utf8 = list_nth_int(ints, at + 10);
		d->aux = get64(ints, at + 11);
		at += 13;
		for (int c = 0; c < d->ncond; c++, at += 8)
		{
			d->cond[c] = (SiftV6Col) {.attno = list_nth_int(ints, at),.typid = (Oid) list_nth_int(ints, at + 1),.src = (SiftV6Src) list_nth_int(ints, at + 2),.slot = list_nth_int(ints, at + 3),
			.jleaf = list_nth_int(ints, at + 4),.jtype = list_nth_int(ints, at + 5) != 0};
			d->cval[c] = get64(ints, at + 6);
		}
		for (int b = 0; b < SIFT_V6_ELSE_BYTES; b++)
			d->else_bytes[b] = (uint8) list_nth_int(ints, at++);
		sift_vn_derive_restore(d);
	}
	for (int i = 0; i < s->nkeys; i++)
		s->key[i].derive = list_nth_int(ints, at++);
	for (int j = 0; j < s->naggs; j++)
		s->agg[j].arg.derive = list_nth_int(ints, at++);
	for (int i = 0; i < s->nfcols; i++)
		s->fcol[i].derive = list_nth_int(ints, at++);
	s->affine = list_nth_int(ints, at);
	s->aff_type = (Oid) list_nth_int(ints, at + 1);
	s->aff_lo = get64(ints, at + 2);
	s->aff_hi = get64(ints, at + 4);
	at += 6;
	s->rows = list_nth_int(ints, at);
	s->groups_est = (uint32) list_nth_int(ints, at + 1);
	s->topk = (uint32) list_nth_int(ints, at + 2);
	s->ntopk = list_nth_int(ints, at + 3);
	at += 4;
	for (int i = 0; i < s->ntopk; i++, at += 4)
		s->topk_item[i] = (SiftV6TopKItem) {.pos = list_nth_int(ints, at),.sortop = (Oid) list_nth_int(ints, at + 1),.collid = (Oid) list_nth_int(ints, at + 2),.nulls_first = list_nth_int(ints, at + 3)};
	s->nout = list_nth_int(ints, at++);
	s->out = palloc(Max(s->nout, 1) * sizeof(SiftV6RowOut));
	for (int i = 0; i < s->nout; i++, at += 2)
		s->out[i] = (SiftV6RowOut) {.from = (SiftV6RowFrom) list_nth_int(ints, at),.at = list_nth_int(ints, at + 1)};
	s->lock_k = (uint32) list_nth_int(ints, at);
	s->having = list_nth_int(ints, at + 1);
}

/* ---- ORDER BY .. LIMIT of the groups ---- */

/* LIMIT + OFFSET of constants of a query that has no DISTINCT or set-returning function, at most max_k (0: the query has no such limit, or one the node cannot take) */
static uint32
limit_plus_offset(const Query *q, int64 max_k)
{
	int64		limit,
				offset = 0;

	if (q->distinctClause != NIL || q->hasTargetSRFs || q->limitOption != LIMIT_OPTION_COUNT || q->limitCount == NULL || !IsA(q->limitCount, Const))
		return 0;
	if (((const Const *) q->limitCount)->constisnull)
		return 0;
	limit = DatumGetInt64(((const Const *) q->limitCount)->constvalue);
	if (q->limitOffset != NULL)
	{
		if (!IsA(q->limitOffset, Const) || ((const Const *) q->limitOffset)->constisnull)
			return 0;
		offset = DatumGetInt64(((const Const *) q->limitOffset)->constvalue);
	}
	return limit < 1 || offset < 0 || limit + offset > max_k ? 0 : (uint32) (limit + offset);
}

/* the K of the query: ORDER BY and LIMIT + OFFSET of constants */
uint32
sift_v6_topk_of(const Query *q)
{
	return q->sortClause == NIL || q->hasWindowFuncs ? 0 : limit_plus_offset(q, SIFT_TOPK_MAX_K);
}

/* the K of the best rows of the query: the same, window functions over the whole set (the node of the rows computes them) allowed */
uint32
sift_v6_rows_k_of(const Query *q)
{
	return q->sortClause == NIL ? 0 : limit_plus_offset(q, SIFT_TOPK_MAX_K);
}

uint32
sift_v6_limit_of(const Query *q)
{
	if (q->sortClause != NIL || q->hasWindowFuncs || q->groupClause == NIL || q->groupingSets != NIL || q->havingQual != NULL || q->hasDistinctOn || q->setOperations != NULL || q->rowMarks != NIL)
		return 0;
	return limit_plus_offset(q, SIFT_LOCK_MAX_K);
}

/* the sort columns of the query as positions of the scan tuple; false when one is not a key or an aggregate the node makes */
bool
sift_v6_topk_items(const Query *q, const List *scan_tlist, SiftV6Spec *s)
{
	if (list_length(q->sortClause) > SIFT_V6_MAX_TOPK_ITEMS)
		return false;
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		const Expr *e = (const Expr *) get_sortgroupclause_expr(sgc, q->targetList);
		int			pos = 0;
		bool		found = false;

		foreach_node(TargetEntry, te, scan_tlist)
		{
			found = equal(te->expr, e);
			if (found)
				break;
			pos++;
		}
		if (!found || (pos < s->nkeys && sift_v6_key_generic(s->key[pos].typid)))
			return false;
		s->topk_item[s->ntopk++] = (SiftV6TopKItem) {.pos = pos,.sortop = sgc->sortop,.collid = exprCollation((const Node *) e),.nulls_first = sgc->nulls_first};
	}
	return true;
}

void
sift_v6_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const uint32 k = sift_v6_topk_of(root->parse);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (k == 0 || target == NULL || input_rel->reloptkind != RELOPT_UPPER_REL)
		return;
	foreach_ptr(Path, path, input_rel->pathlist)
	{
		const CustomPath *cp = (const CustomPath *) path;
		SiftV6Spec *s;
		CustomPath *top;

		if (!IsA(path, CustomPath) || cp->methods != sift_v6_group_path_methods() || !equal(path->pathtarget->exprs, target->exprs))
			continue;
		s = sift_v6_spec_unpack(linitial(cp->custom_private));
		if (s->nsets > 0 || s->topk > 0 || s->ext != NULL || s->nparts > 0 || s->par_strategy != 0 || cp->custom_paths != NIL || path->parallel_aware)
			continue;
		s->topk = k;
		s->having = lthird(cp->custom_private) != NIL;
		if (!sift_v6_topk_items(root->parse, lsecond(cp->custom_private), s))
			continue;
		top = makeNode(CustomPath);
		*top = *cp;
		top->path.parent = output_rel;
		top->path.pathtarget = target;
		top->path.pathkeys = root->sort_pathkeys;
		top->path.rows = Min(clamp_row_est(path->rows), (double) k);
		top->path.total_cost = path->total_cost + 0.0001 * path->rows;
		top->custom_private = list_make3(sift_v6_spec_pack(s), lsecond(cp->custom_private), lthird(cp->custom_private));
		add_path(output_rel, &top->path);
	}
}
