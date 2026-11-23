/*
 * agg/v6/v6_expr_num.c - the exact numeric arguments of the scan (agg/v6/v6_expr_num.h) and the CASE that is a FILTER.
 */
/* check-memory: allow-allocs every node here follows the expressions of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/v6/v6_expr_num.h"

#define MAX_RESULT_SCALE	30

typedef struct Factor
{
	SiftV6Col	col;
	int64		k;
	bool		neg;
	int			p;
	int			s;
} Factor;

static int64
pow10_i64(int p)
{
	int64		v = 1;

	while (p-- > 0)
		v *= 10;
	return v;
}

/* the numeric constant as the integer of its digits and their number after the point: false for a NaN, an infinity or more than 18 digits */
static bool
const_scaled(const Const *c, int64 *v, int *scale)
{
	const char *s;
	int			digits = 0;
	bool		neg = false;

	if (!IsA(c, Const) || c->constisnull || c->consttype != NUMERICOID)
		return false;
	s = DatumGetCString(DirectFunctionCall1(numeric_out, c->constvalue));
	*v = 0;
	*scale = -1;
	if (*s == '-')
	{
		neg = true;
		s++;
	}
	for (; *s != '\0'; s++)
	{
		if (*s == '.' && *scale < 0)
			*scale = 0;
		else if (*s >= '0' && *s <= '9' && ++digits <= 18)
		{
			*v = *v * 10 + (*s - '0');
			*scale += *scale >= 0;
		}
		else
			return false;
	}
	*scale = Max(*scale, 0);
	*v = neg ? -*v : *v;
	return digits > 0;
}

static const char *
numeric_op(const Expr *e)
{
	const OpExpr *op = (const OpExpr *) e;
	const char *name;

	if (!IsA(e, OpExpr) || list_length(op->args) != 2 || get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE || (name = get_opname(op->opno)) == NULL ||
		exprType(linitial(op->args)) != NUMERICOID || exprType(lsecond(op->args)) != NUMERICOID || strlen(name) != 1 || strchr("+-*", name[0]) == NULL)
		return NULL;
	return name;
}

static bool
column_factor(const SiftV6Resolver *rs, const Expr *e, Factor *f)
{
	*f = (Factor) {0};
	if (!rs->column(rs->ctx, e, true, &f->col) || f->col.typid != NUMERICOID || f->col.scale > 18)
		return false;
	f->s = f->col.scale;
	return true;
}

/* a column, or a column plus or minus a constant */
static bool
factor_of(const SiftV6Resolver *rs, const Expr *e, Factor *f)
{
	const OpExpr *op = (const OpExpr *) e;
	const char *name;
	const Node *l,
			   *r;
	int64		c;
	int			sc;
	bool		col_first;

	if (IsA(e, Var))
		return column_factor(rs, e, f);
	if ((name = numeric_op(e)) == NULL || name[0] == '*')
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	col_first = IsA(l, Var);
	if (!column_factor(rs, (const Expr *) (col_first ? l : r), f) || !const_scaled((const Const *) (col_first ? r : l), &c, &sc))
		return false;
	f->s = Max(f->col.scale, sc);
	f->p = f->s - f->col.scale;
	if (f->s - sc > 18 || (c != 0 && pow10_i64(f->s - sc) > PG_INT64_MAX / (c < 0 ? -c : c)))
		return false;
	f->k = c * pow10_i64(f->s - sc);
	f->neg = name[0] == '-' && !col_first;
	if (name[0] == '-' && col_first)
		f->k = -f->k;
	return true;
}

static bool
collect(const SiftV6Resolver *rs, const Expr *e, Factor *f, int *n)
{
	const char *name = numeric_op(e);

	if (name != NULL && name[0] == '*')
		return collect(rs, linitial(((const OpExpr *) e)->args), f, n) && collect(rs, lsecond(((const OpExpr *) e)->args), f, n);
	return *n < SIFT_VN_MAX_FACTORS && factor_of(rs, e, &f[(*n)++]);
}

bool
sift_vn_derive_of(const SiftV6Resolver *rs, const Expr *e, SiftV6Derive *d)
{
	Factor		f[SIFT_VN_MAX_FACTORS];
	int			n = 0,
				scale = 0;

	if (numeric_op(e) == NULL || !collect(rs, e, f, &n) || (n == 1 && f[0].k == 0 && !f[0].neg && f[0].p == 0))
		return false;
	for (int i = 0; i < n; i++)
	{
		scale += f[i].s;
		d->cond[i] = f[i].col;
		d->cval[i] = f[i].k;
		d->else_bytes[4 * i] = f[i].neg;
		d->else_bytes[4 * i + 1] = (uint8) f[i].p;
		d->else_bytes[4 * i + 2] = (uint8) f[i].s;
		d->else_bytes[4 * i + 3] = (uint8) f[i].col.scale;
	}
	d->kind = SVD_NUM_EXPR;
	d->ncond = n;
	d->else_len = 4 * n;
	d->aux = scale;
	d->src = f[0].col;
	return scale <= MAX_RESULT_SCALE;
}

void
sift_vn_derive_restore(SiftV6Derive *d)
{
	if (d->kind != SVD_NUM_EXPR)
		return;
	for (int i = 0; i < d->ncond; i++)
		d->cond[i].scale = d->else_bytes[4 * i + 3];
	d->src = d->cond[0];
}

bool
sift_vn_prefix_of(const SiftV6Derive *a, const SiftV6Derive *b)
{
	if (a->kind != SVD_NUM_EXPR || b->kind != SVD_NUM_EXPR || a->ncond >= b->ncond)
		return false;
	for (int i = 0; i < a->ncond; i++)
		if (a->cond[i].attno != b->cond[i].attno || a->cval[i] != b->cval[i] || memcmp(&a->else_bytes[4 * i], &b->else_bytes[4 * i], 4) != 0)
			return false;
	return true;
}

/* ---- the evaluation ---- */

typedef struct Fac
{
	const int64 *x;
	int64		k;
	int64		m;
} Fac;

static inline bool
factor_value(const Fac *f, uint32 r, int64 *out)
{
	int64		t;

	return __builtin_mul_overflow(f->m, f->x[r], &t) | __builtin_add_overflow(t, f->k, out);
}

/* the product of the factors (and of the vector pre) of each row in int64; false when an operation of any row leaves it */
static bool
eval64(const Fac *f, int nf, const int64 *pre, uint32 n, int64 *out)
{
	bool		ovf = false;

	for (uint32 r = 0; r < n; r++)
	{
		int64		v = 0,
					x;
		int			i = 0;

		if (pre != NULL)
			v = pre[r];
		else
			ovf |= factor_value(&f[i++], r, &v);
		for (; i < nf; i++)
		{
			ovf |= factor_value(&f[i], r, &x);
			ovf |= __builtin_mul_overflow(v, x, &v);
		}
		out[r] = v;
	}
	return !ovf;
}

static __int128
i128_at(const SiftVec *v, uint32 r)
{
	if (v->type != SVEC_NUM128)
		return ((const int64 *) v->data)[r];
	return (__int128) (((unsigned __int128) (uint64) ((const SiftI128 *) v->data)[r].hi << 64) | ((const SiftI128 *) v->data)[r].lo);
}

/* a row of a wide vector is below 2^95, so that 2^32 of them add up in the 128 bits of the sum of the groups */
#define SUM_ROW_LIMIT	(((__int128) 1) << 95)

/* the rows of todo in 128 bits: an error when a product leaves them too */
static void
eval128(const Fac *f, int nf, const SiftVec *pre, uint32 n, const uint64 *todo, SiftI128 *out)
{
	for (uint32 r = 0; r < n; r++)
	{
		__int128	v = 0;
		bool		ovf = false;
		int			i = 0;

		if (!((todo[r >> 6] >> (r & 63)) & 1))
			continue;
		if (pre != NULL)
			v = i128_at(pre, r);
		else
		{
			v = (__int128) f[0].m * f[0].x[r] + f[0].k;
			i = 1;
		}
		for (; i < nf; i++)
			ovf |= __builtin_mul_overflow(v, (__int128) f[i].m * f[i].x[r] + f[i].k, &v);
		ovf |= v >= SUM_ROW_LIMIT || v <= -SUM_ROW_LIMIT;
		if (ovf)
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("a numeric expression is out of the range the aggregate scan computes"),
							errhint("Run the statement with sift.enable_agg = off.")));
		out[r] = (SiftI128) {.lo = (uint64) v,.hi = (int64) (v >> 64)};
	}
}

void
sift_vn_derive(const SiftV6Derive *d, SiftVec *const *cond, const SiftVec *prefix, int nprefix, const SiftBatchSel *bs, SiftVxOut *o)
{
	Fac			f[SIFT_VN_MAX_FACTORS];
	const SiftVec *used[SIFT_VN_MAX_FACTORS + 1];
	uint64		todo[SIFT_VEC_WORDS];
	const int	nf = d->ncond - nprefix;
	const uint32 n = bs->n_raw,
				words = (n + 63) / 64;
	int			nused = 0;
	bool		all_valid = true;

	for (int i = 0; i < nf; i++)
	{
		const int	c = nprefix + i;
		const int64 m = pow10_i64(d->else_bytes[4 * c + 1]);

		f[i] = (Fac) {.x = cond[c]->data,.k = d->cval[c],.m = d->else_bytes[4 * c] ? -m : m};
		used[nused++] = cond[c];
	}
	if (prefix != NULL)
		used[nused++] = prefix;
	memset(o->valid, 0, sizeof(o->valid));
	memset(o->valid, 0xff, words * sizeof(uint64));
	if (n & 63)
		o->valid[words - 1] = (UINT64CONST(1) << (n & 63)) - 1;
	for (int i = 0; i < nused; i++)
		if (!used[i]->all_valid)
		{
			all_valid = false;
			for (uint32 w = 0; w < words; w++)
				o->valid[w] &= used[i]->valid[w];
		}
	for (uint32 w = 0; w < words; w++)
		todo[w] = bs->mask[w] & o->valid[w];
	o->vec = (SiftVec) {.data = o->data,.valid = all_valid ? NULL : o->valid,.len = n,.all_valid = all_valid,.dense = true,.type = SVEC_NUM64,.scale = (int16) d->aux};
	if ((prefix == NULL || prefix->type != SVEC_NUM128) && eval64(f, nf, prefix != NULL ? prefix->data : NULL, n, o->data))
		return;
	eval128(f, nf, prefix, n, todo, o->data);
	o->vec.type = SVEC_NUM128;
}

/* ---- CASE WHEN .. THEN .. END as a FILTER ---- */

static bool
func_is(Oid funcid, const char *name)
{
	const char *fname = get_func_name(funcid);

	return fname != NULL && get_func_namespace(funcid) == PG_CATALOG_NAMESPACE && strcmp(fname, name) == 0;
}

/* the integer or numeric constant is n */
static bool
const_is(const Expr *e, int64 n)
{
	const Const *c = (const Const *) e;
	int64		v;
	int			scale;

	if (!IsA(e, Const) || c->constisnull)
		return false;
	switch (c->consttype)
	{
		case INT2OID:
			return DatumGetInt16(c->constvalue) == n;
		case INT4OID:
			return DatumGetInt32(c->constvalue) == n;
		case INT8OID:
			return DatumGetInt64(c->constvalue) == n;
		case NUMERICOID:
			return const_scaled(c, &v, &scale) && v == n * pow10_i64(scale);
		default:
			return false;
	}
}

typedef struct CaseAgg
{
	Expr	   *cond;
	Expr	   *val;
	bool		else_zero;
	Oid			type;
} CaseAgg;

/* sum(CASE WHEN cond THEN val [ELSE 0 | NULL] END) and count(CASE WHEN cond THEN val [ELSE NULL] END) */
static bool
case_agg_of(const Aggref *a, CaseAgg *ca)
{
	const CaseExpr *ce;
	const CaseWhen *w;

	if (a->aggfilter != NULL || a->aggdistinct != NIL || a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL || a->aggvariadic || a->aggstar || list_length(a->args) != 1 ||
		!(func_is(a->aggfnoid, "sum") || func_is(a->aggfnoid, "count")))
		return false;
	ce = (const CaseExpr *) linitial_node(TargetEntry, a->args)->expr;
	if (!IsA(ce, CaseExpr) || ce->arg != NULL || list_length(ce->args) != 1)
		return false;
	w = linitial_node(CaseWhen, ce->args);
	*ca = (CaseAgg) {.cond = w->expr,.val = w->result,.type = ce->casetype};
	if (ce->defresult == NULL || (IsA(ce->defresult, Const) && ((Const *) ce->defresult)->constisnull))
		return true;
	ca->else_zero = const_is(ce->defresult, 0);
	return ca->else_zero;
}

static Aggref *
count_star_of(const Aggref *a)
{
	Aggref	   *c = copyObject((Aggref *) a);

	c->aggfnoid = F_COUNT_;
	c->aggstar = true;
	c->args = NIL;
	c->aggargtypes = NIL;
	c->aggtype = c->aggtranstype = INT8OID;
	return c;
}

static Aggref *
filtered(const Aggref *a, const CaseAgg *ca, bool count_star)
{
	Aggref	   *f = count_star ? count_star_of(a) : copyObject((Aggref *) a);

	f->aggfilter = copyObject(ca->cond);
	if (!count_star)
	{
		f->args = list_make1(makeTargetEntry(copyObject(ca->val), 1, NULL, false));
		f->aggargtypes = list_make1_oid(exprType((Node *) ca->val));
	}
	return f;
}

/* CASE WHEN count(*) = 0 THEN NULL ELSE inner END: the sum of a group that has rows and no row for the filter is 0, that of no rows is NULL */
static Node *
unless_empty(const Aggref *a, Expr *inner)
{
	Oid			eq = OpernameGetOprid(list_make1(makeString("=")), INT8OID, INT8OID);
	OpExpr	   *none = (OpExpr *) make_opclause(eq, BOOLOID, false, (Expr *) count_star_of(a), (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true),
												InvalidOid, InvalidOid);
	CaseWhen   *w = makeNode(CaseWhen);
	CaseExpr   *c = makeNode(CaseExpr);

	none->opfuncid = get_opcode(eq);
	w->expr = (Expr *) none;
	w->result = (Expr *) makeNullConst(a->aggtype, -1, InvalidOid);
	c->casetype = a->aggtype;
	c->args = list_make1(w);
	c->defresult = inner;
	return (Node *) c;
}

static Node *
rewritten(const Aggref *a, const CaseAgg *ca)
{
	const bool	constant = IsA(ca->val, Const);
	CoalesceExpr *zero;

	if (exprType((Node *) ca->val) != ca->type || (constant && ((const Const *) ca->val)->constisnull))
		return NULL;
	if (func_is(a->aggfnoid, "count"))
		return ca->else_zero ? NULL : (Node *) filtered(a, ca, constant);
	if (!ca->else_zero)
		return constant ? NULL : (Node *) filtered(a, ca, false);
	if (constant)
		return a->aggtype == INT8OID && const_is(ca->val, 1) ? unless_empty(a, (Expr *) filtered(a, ca, true)) : NULL;
	if (a->aggtype != INT8OID && a->aggtype != NUMERICOID)
		return NULL;
	zero = makeNode(CoalesceExpr);
	zero->coalescetype = a->aggtype;
	zero->args = list_make2(filtered(a, ca, false), a->aggtype == INT8OID ? makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true) :
							makeConst(NUMERICOID, -1, InvalidOid, -1, NumericGetDatum(int64_to_numeric(0)), false, false));
	return unless_empty(a, (Expr *) zero);
}

static Node *
case_mutator(Node *n, void *context)
{
	CaseAgg		ca;
	Node	   *r;

	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref) && case_agg_of((const Aggref *) n, &ca) && (r = rewritten((const Aggref *) n, &ca)) != NULL)
		return r;
	return expression_tree_mutator(n, case_mutator, context);
}

Node *
sift_vn_case_to_filter(Node *expr)
{
	return case_mutator(expr, NULL);
}

Node *
sift_vn_rewrite_aggs(Node *expr)
{
	return sift_v6_collapse_offset_sums(sift_vn_case_to_filter(expr));
}
