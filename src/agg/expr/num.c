/*
 * agg/expr/num.c - the exact numeric arguments of the scan (agg/expr/num.h) and the CASE that is a FILTER.
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

#include "agg/expr/num.h"

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

static inline bool
row_wanted(const uint64 *todo, uint32 r)
{
	return (todo[r >> 6] >> (r & 63)) & 1;
}

static inline __int128
factor_wide(const Fac *f, uint32 r)
{
	return (__int128) f->m * f->x[r] + f->k;
}

/* the rows of todo in 128 bits; false when the product of a row leaves them */
static bool
eval128(const Fac *f, int nf, const SiftVec *pre, uint32 n, const uint64 *todo, SiftI128 *out)
{
	bool		ovf = false;

	for (uint32 r = 0; r < n; r++)
	{
		__int128	v;
		int			i = 0;

		if (!row_wanted(todo, r))
			continue;
		if (pre != NULL)
			v = i128_at(pre, r);
		else
			v = factor_wide(&f[i++], r);
		for (; i < nf; i++)
			ovf |= __builtin_mul_overflow(v, factor_wide(&f[i], r), &v);
		out[r] = (SiftI128) {.lo = (uint64) v,.hi = (int64) (v >> 64)};
	}
	return !ovf;
}

/* the magnitude of a product below 2^191, in 64-bit limbs, low first */
typedef struct Mag
{
	uint64		l[3];
} Mag;

static inline unsigned __int128
abs128(__int128 v)
{
	return v < 0 ? -(unsigned __int128) v : (unsigned __int128) v;
}

static Mag
mag_of(__int128 v)
{
	const unsigned __int128 u = abs128(v);

	return (Mag) {{(uint64) u, (uint64) (u >> 64), 0}};
}

/* m * f; false when the product is 2^191 or more */
static bool
mag_mul(Mag *m, unsigned __int128 f)
{
	const uint64 fl[2] = {(uint64) f, (uint64) (f >> 64)};
	uint64		r[5] = {0};

	for (int i = 0; i < 3; i++)
	{
		unsigned __int128 carry = 0;

		for (int j = 0; j < 2; j++)
		{
			const unsigned __int128 t = (unsigned __int128) m->l[i] * fl[j] + r[i + j] + carry;

			r[i + j] = (uint64) t;
			carry = t >> 64;
		}
		r[i + 2] = (uint64) carry;
	}
	if (r[3] != 0 || r[4] != 0 || (r[2] >> 63) != 0)
		return false;
	memcpy(m->l, r, sizeof(m->l));
	return true;
}

static SiftI256
i256_of(bool neg, Mag m)
{
	SiftI256	o = {{m.l[0], m.l[1], m.l[2], 0}};

	if (neg && (m.l[0] | m.l[1] | m.l[2]) != 0)
	{
		o.l[0] = -m.l[0];
		o.l[1] = ~m.l[1] + (m.l[0] == 0);
		o.l[2] = ~m.l[2] + (m.l[0] == 0 && m.l[1] == 0);
		o.l[3] = ~UINT64CONST(0);
	}
	return o;
}

/* the sign and magnitude of row r of the vector of the first factors */
static bool
row_mag(const SiftVec *v, uint32 r, Mag *m)
{
	SiftI256	x;

	if (v->type != SVEC_NUM256)
	{
		const __int128 i = i128_at(v, r);

		*m = mag_of(i);
		return i < 0;
	}
	x = ((const SiftI256 *) v->data)[r];
	if ((int64) x.l[2] >= 0)
		*m = (Mag) {{x.l[0], x.l[1], x.l[2]}};
	else
		*m = (Mag) {{-x.l[0], ~x.l[1] + (x.l[0] == 0), ~x.l[2] + (x.l[0] == 0 && x.l[1] == 0)}};
	return (int64) x.l[2] < 0;
}

/* the rows of todo in 192 bits: an error when a product leaves them too (a numeric product of 57 digits is the most the scan computes) */
static void
eval256(const Fac *f, int nf, const SiftVec *pre, uint32 n, const uint64 *todo, SiftI256 *out)
{
	for (uint32 r = 0; r < n; r++)
	{
		Mag			m;
		bool		neg;
		int			i = 0;

		if (!row_wanted(todo, r))
			continue;
		if (pre != NULL)
			neg = row_mag(pre, r, &m);
		else
		{
			const __int128 x = factor_wide(&f[i++], r);

			m = mag_of(x);
			neg = x < 0;
		}
		for (; i < nf; i++)
		{
			const __int128 x = factor_wide(&f[i], r);

			neg ^= x < 0;
			if (!mag_mul(&m, abs128(x)))
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("a numeric expression is out of the range the aggregate scan computes"),
								errhint("Run the statement with sift.enable_agg = off.")));
		}
		out[r] = i256_of(neg, m);
	}
}

/*
 * The rows of o that hold a value (o->valid: every factor has one) and the rows that are NaN (o->nan): a NULL factor makes the row NULL, else a NaN factor makes it NaN, as numeric_mul
 * and numeric_add do. Returns whether a row is NaN; *all_valid whether every row has a value.
 */
static bool
derive_validity(const SiftVec *const *used, int nused, uint32 n, SiftVxOut *o, bool *all_valid)
{
	const uint32 words = (n + 63) / 64;
	uint64		known[SIFT_VEC_WORDS],
				nan_any[SIFT_VEC_WORDS] = {0};
	bool		has_nan = false;

	memset(o->valid, 0, sizeof(o->valid));
	memset(o->valid, 0xff, words * sizeof(uint64));
	if (n & 63)
		o->valid[words - 1] = (UINT64CONST(1) << (n & 63)) - 1;
	memcpy(known, o->valid, sizeof(known));
	*all_valid = true;
	for (int i = 0; i < nused; i++)
	{
		if (!used[i]->all_valid)
			*all_valid = false;
		for (uint32 w = 0; w < words; w++)
		{
			const uint64 nan = used[i]->nan != NULL ? used[i]->nan[w] : 0;

			if (!used[i]->all_valid)
			{
				o->valid[w] &= used[i]->valid[w];
				known[w] &= used[i]->valid[w] | nan;
			}
			nan_any[w] |= nan;
		}
	}
	for (uint32 w = 0; w < words; w++)
	{
		o->nan[w] = nan_any[w] & known[w];
		has_nan |= o->nan[w] != 0;
	}
	return has_nan;
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
	bool		all_valid,
				has_nan;

	for (int i = 0; i < nf; i++)
	{
		const int	c = nprefix + i;
		const int64 m = pow10_i64(d->else_bytes[4 * c + 1]);

		f[i] = (Fac) {.x = cond[c]->data,.k = d->cval[c],.m = d->else_bytes[4 * c] ? -m : m};
		used[nused++] = cond[c];
	}
	if (prefix != NULL)
		used[nused++] = prefix;
	has_nan = derive_validity(used, nused, n, o, &all_valid);
	for (uint32 w = 0; w < words; w++)
		todo[w] = bs->mask[w] & o->valid[w];
	o->vec = (SiftVec) {.data = o->data,.valid = all_valid ? NULL : o->valid,.nan = has_nan ? o->nan : NULL,.len = n,.all_valid = all_valid,.dense = true,.type = SVEC_NUM64,
						.scale = (int16) d->aux};
	if ((prefix == NULL || prefix->type == SVEC_NUM64) && eval64(f, nf, prefix != NULL ? prefix->data : NULL, n, o->data))
		return;
	o->vec.type = SVEC_NUM128;
	if ((prefix == NULL || prefix->type != SVEC_NUM256) && eval128(f, nf, prefix, n, todo, o->data))
		return;
	eval256(f, nf, prefix, n, todo, o->data);
	o->vec.type = SVEC_NUM256;
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

/* a numeric constant with digits after the point: a sum that adds it shows them, the sum of the values alone does not */
static bool
has_decimals(const Expr *e)
{
	int64		v;
	int			scale;

	return const_scaled((const Const *) e, &v, &scale) && scale > 0;
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
	ca->else_zero = const_is(ce->defresult, 0) && !has_decimals(ce->defresult);
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

Aggref *
sift_vn_filter_to_case(const Aggref *a)
{
	Aggref	   *c;
	CaseWhen   *w;
	CaseExpr   *ce;
	Expr	   *val;

	if (a->aggfilter == NULL || a->aggdistinct != NIL || a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL || a->aggvariadic || a->aggstar || list_length(a->args) != 1 ||
		!(func_is(a->aggfnoid, "sum") || func_is(a->aggfnoid, "count")))
		return NULL;
	c = copyObject((Aggref *) a);
	val = linitial_node(TargetEntry, c->args)->expr;
	w = makeNode(CaseWhen);
	w->expr = c->aggfilter;
	w->result = val;
	ce = makeNode(CaseExpr);
	ce->casetype = exprType((Node *) val);
	ce->casecollid = exprCollation((Node *) val);
	ce->args = list_make1(w);
	ce->defresult = (Expr *) makeNullConst(ce->casetype, exprTypmod((Node *) val), ce->casecollid);
	linitial_node(TargetEntry, c->args)->expr = (Expr *) ce;
	c->aggfilter = NULL;
	return c;
}

/* CASE WHEN none = 0 THEN when_none ELSE inner END */
static Node *
unless_none(const Aggref *none_count, Expr *when_none, Expr *inner)
{
	Oid			eq = OpernameGetOprid(list_make1(makeString("=")), INT8OID, INT8OID);
	OpExpr	   *none = (OpExpr *) make_opclause(eq, BOOLOID, false, (Expr *) none_count, (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true),
												InvalidOid, InvalidOid);
	CaseWhen   *w = makeNode(CaseWhen);
	CaseExpr   *c = makeNode(CaseExpr);

	none->opfuncid = get_opcode(eq);
	w->expr = (Expr *) none;
	w->result = when_none;
	c->casetype = exprType((Node *) inner);
	c->args = list_make1(w);
	c->defresult = inner;
	return (Node *) c;
}

/* count(*) FILTER (WHERE cond IS NOT TRUE): the rows that take the ELSE 0 */
static Aggref *
count_else(const Aggref *a, const CaseAgg *ca)
{
	Aggref	   *c = count_star_of(a);
	BooleanTest *t = makeNode(BooleanTest);

	t->arg = copyObject(ca->cond);
	t->booltesttype = IS_NOT_TRUE;
	t->location = -1;
	c->aggfilter = (Expr *) t;
	return c;
}

/*
 * The sum of the rows of cond, and 0 when a row took the ELSE: a group whose every row took cond sums the values alone, NULL when they all are NULL,
 * and a group of no rows is NULL as the sum of none
 */
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
		return a->aggtype == INT8OID && const_is(ca->val, 1) ?
			unless_none(count_star_of(a), (Expr *) makeNullConst(INT8OID, -1, InvalidOid), (Expr *) filtered(a, ca, true)) : NULL;
	if (a->aggtype != INT8OID && a->aggtype != NUMERICOID)
		return NULL;
	zero = makeNode(CoalesceExpr);
	zero->coalescetype = a->aggtype;
	zero->args = list_make2(filtered(a, ca, false), a->aggtype == INT8OID ? makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true) :
							makeConst(NUMERICOID, -1, InvalidOid, -1, NumericGetDatum(int64_to_numeric(0)), false, false));
	return unless_none(count_else(a, ca), (Expr *) filtered(a, ca, false), (Expr *) zero);
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
