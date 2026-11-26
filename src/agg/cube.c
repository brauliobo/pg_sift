/*
 * agg/cube.c - the statements a pre-aggregated cube can answer (v6_cube.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/cube.h"
#include "agg/expr/pg.h"

static bool
int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static int64
const_int(const Const *k)
{
	return k->consttype == INT2OID ? DatumGetInt16(k->constvalue) : k->consttype == INT4OID ? DatumGetInt32(k->constvalue) : DatumGetInt64(k->constvalue);
}

/* The integer +, - or * of PostgreSQL (int8pl, int48mi, int2mul, ..): its operation, '\0' for any other function */
static char
int_op(Oid func)
{
	const char *fn = get_func_name(func);
	const char *op;

	if (fn == NULL || strncmp(fn, "int", 3) != 0)
		return '\0';
	op = fn + 3;
	while (*op == '2' || *op == '4' || *op == '8')
		op++;
	if (op - fn < 4 || op - fn > 5)
		return '\0';
	return strcmp(op, "pl") == 0 ? '+' : strcmp(op, "mi") == 0 ? '-' : strcmp(op, "mul") == 0 ? '*' : '\0';
}

/* e is x op k or k op x over the one input of an expression: the line it is */
static bool
linear_of(const Expr *e, SiftV6Linear *lin)
{
	const OpExpr *op = (const OpExpr *) e;
	const Node *l,
			   *r;
	const Const *k;
	bool		var_left;
	char		o;
	int64		kv;

	if (!IsA(e, OpExpr) || list_length(op->args) != 2 || !int_type(op->opresulttype) || (o = int_op(op->opfuncid)) == '\0')
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	var_left = IsA(l, Var) && IsA(r, Const);
	if (!var_left && !(IsA(l, Const) && IsA(r, Var)))
		return false;
	k = (const Const *) (var_left ? r : l);
	if (k->constisnull || !int_type(k->consttype) || !int_type(((const Var *) (var_left ? l : r))->vartype))
		return false;
	kv = const_int(k);
	if (o == '-' && var_left && kv == PG_INT64_MIN)
		return false;
	*lin = (SiftV6Linear) {.mul = o == '*' ? kv : o == '-' && !var_left ? -1 : 1,.add = o == '*' ? 0 : o == '+' || !var_left ? kv : -kv,.type = op->opresulttype};
	return true;
}

bool
sift_v6_agg_linear(const SiftV6Spec *spec, const SiftV6Agg *a, SiftV6Linear *lin)
{
	const SiftV6Derive *d;
	SiftV6PgItem it;

	if (a->arg.derive <= 0 || (a->kind != SV6_SUM && a->kind != SV6_AVG))
		return false;
	d = &spec->derive[a->arg.derive - 1];
	if (d->kind != SVD_PG_EXPR)
		return false;
	sift_v6_pgexpr_item(spec, d, &it);
	return it.nin == 1 && it.col[0].src == SV6SRC_ATTR && it.col[0].jleaf == 0 && int_type(it.col[0].typid) && linear_of(it.expr, lin);
}

static __int128
line_at(const SiftV6Linear *lin, int64 x)
{
	return (__int128) lin->mul * x + lin->add;
}

bool
sift_v6_linear_fits(const SiftV6Linear *lin, int64 lo, int64 hi)
{
	const int64 tmin = lin->type == INT2OID ? PG_INT16_MIN : lin->type == INT4OID ? PG_INT32_MIN : PG_INT64_MIN,
				tmax = lin->type == INT2OID ? PG_INT16_MAX : lin->type == INT4OID ? PG_INT32_MAX : PG_INT64_MAX;

	return line_at(lin, lo) >= tmin && line_at(lin, lo) <= tmax && line_at(lin, hi) >= tmin && line_at(lin, hi) <= tmax;
}

bool
sift_v6_linear_sum(const SiftV6Linear *lin, SiftI128 sum, int64 n, SiftI128 *out)
{
	const __int128 s = (__int128) (((unsigned __int128) (uint64) sum.hi << 64) | sum.lo);
	__int128	r;

	if (__builtin_mul_overflow(s, (__int128) lin->mul, &r) || __builtin_add_overflow(r, (__int128) lin->add * n, &r))
		return false;
	*out = (SiftI128) {(uint64) r, (int64) (r >> 64)};
	return true;
}

static bool
agg_cubeable(const SiftV6Spec *spec, const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);
	SiftV6Linear lin;

	if (a->fclause != 0 || a->nxcols != 0 || a->arg.src != SV6SRC_ATTR || a->arg.jleaf > 0)
		return false;
	if (a->arg.derive > 0)
		return sift_v6_agg_linear(spec, a, &lin);
	switch (a->kind)
	{
		case SV6_COUNT_STAR:
			return true;
		case SV6_COUNT:
			return a->arg.attno > 0;
		case SV6_SUM:
		case SV6_AVG:
		case SV6_MIN:
		case SV6_MAX:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64;
		case SV6_COUNT_DISTINCT:
			return (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_TEXT) && a->arg.typid != NUMERICOID;
		default:
			return false;
	}
}

bool
sift_v6_spec_cubeable(const SiftV6Spec *spec)
{
	if (spec->nsets != 0 || spec->nbins != 0 || spec->nfilters != 0 || spec->nfuncs != 0 || spec->ext != NULL || spec->text_queries != NIL || spec->jsonqual != NIL ||
		spec->vqual != NIL || spec->genref != NIL || spec->constant_groups || spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1)
		return false;
	for (int i = 0; i < spec->nkeys; i++)
	{
		const SiftV6Col *k = &spec->key[i];
		const SiftV6Class cls = sift_v6_class(k->typid);

		if (k->src != SV6SRC_ATTR || k->jleaf > 0 || k->derive > 0 || (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_BOOL && cls != SV6C_TEXT) || k->typid == NUMERICOID)
			return false;
	}
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_cubeable(spec, &spec->agg[j]))
			return false;
	return true;
}
