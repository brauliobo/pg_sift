/*
 * agg/win/x_glue.c - SiftWinx as the executor of the window node of SiftAggV6 (v6_winx_glue.h).
 */
/* check-memory: allow-allocs every array here follows the functions of one window, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/win/datebin_pg.h"
#include "agg/datum.h"
#include "agg/plan/shape.h"
#include "agg/win/x_glue.h"

static const struct
{
	const char *name;
	SiftWinxKind kind;
}			kinds[] = {{"row_number", SWX_ROW_NUMBER}, {"rank", SWX_RANK}, {"dense_rank", SWX_DENSE_RANK}, {"count", SWX_COUNT}, {"sum", SWX_SUM},
	{"avg", SWX_AVG}, {"min", SWX_MIN}, {"max", SWX_MAX}, {"lag", SWX_LAG}, {"lead", SWX_LEAD}, {"first_value", SWX_FIRST_VALUE},
	{"var_pop", SWX_VAR_POP}, {"var_samp", SWX_VAR_SAMP}, {"variance", SWX_VAR_SAMP}, {"stddev_pop", SWX_STDDEV_POP}, {"stddev_samp", SWX_STDDEV_SAMP},
{"stddev", SWX_STDDEV_SAMP}};

static bool
kind_of(const WindowFunc *wf, SiftWinxKind *kind)
{
	const char *name;

	if (get_func_namespace(wf->winfnoid) != PG_CATALOG_NAMESPACE || (name = get_func_name(wf->winfnoid)) == NULL)
		return false;
	for (int i = 0; i < lengthof(kinds); i++)
		if (strcmp(name, kinds[i].name) == 0)
			return (*kind = kinds[i].kind, true);
	return false;
}

bool
sift_v6_winx_wanted(const List *funcs)
{
	foreach_node(WindowFunc, wf, (List *) funcs)
	{
		const Node *a = wf->args != NIL ? linitial(wf->args) : NULL;
		SiftWinxKind kind;

		if (wf->aggfilter != NULL || (a != NULL && exprType(a) == NUMERICOID) || (kind_of(wf, &kind) && sift_winx_is_var(kind)))
			return true;
	}
	return false;
}

/* the type PostgreSQL gives the result of the function over an argument of type arg */
static Oid
result_type(SiftWinxKind kind, Oid arg)
{
	switch (kind)
	{
		case SWX_ROW_NUMBER:
		case SWX_RANK:
		case SWX_DENSE_RANK:
		case SWX_COUNT_STAR:
		case SWX_COUNT:
			return INT8OID;
		case SWX_SUM:
			return arg == INT4OID ? INT8OID : arg == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		case SWX_AVG:
			return arg == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		case SWX_VAR_POP:
		case SWX_VAR_SAMP:
		case SWX_STDDEV_POP:
		case SWX_STDDEV_SAMP:
			return arg == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		default:
			return arg;
	}
}

static bool
arg_oid_ok(Oid t)
{
	return t == INT4OID || t == INT8OID || t == FLOAT8OID || t == NUMERICOID;
}

/* LAG, LEAD and FIRST_VALUE return the argument as it is, so a date and a timestamp (integers here) are as good as a number */
static bool
value_oid_ok(Oid t)
{
	return arg_oid_ok(t) || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static bool
const_int4(const Node *n, int32 *out)
{
	const Const *c = (const Const *) n;

	if (!IsA(n, Const) || c->constisnull || c->consttype != INT4OID)
		return false;
	return (*out = DatumGetInt32(c->constvalue), true);
}

static int64
int64_pow10(int e)
{
	int64		p = 1;

	while (e-- > 0)
		p *= 10;
	return p;
}

/* a numeric constant as the integer of the scale the column has: only when no digit is lost and at most 18 digits are used */
static bool
numeric_scaled(Datum d, int scale, int64 *out)
{
	const Datum rounded = DirectFunctionCall2(numeric_round, d, Int32GetDatum(scale));
	const Datum bound = DirectFunctionCall1(int8_numeric, Int64GetDatum(INT64CONST(1000000000000000000)));
	Datum		scaled;

	if (!DatumGetBool(DirectFunctionCall2(numeric_eq, d, rounded)))
		return false;
	scaled = DirectFunctionCall2(numeric_mul, rounded, DirectFunctionCall1(int8_numeric, Int64GetDatum(int64_pow10(scale))));
	if (!DatumGetBool(DirectFunctionCall2(numeric_lt, DirectFunctionCall1(numeric_abs, scaled), bound)))
		return false;
	return (*out = DatumGetInt64(DirectFunctionCall1(numeric_int8, scaled)), true);
}

/* the default of LAG or LEAD (the third argument) as the value of the function's argument type; false when it is not a constant of that type, or a numeric that does not fit */
static bool
default_of(const Node *n, Oid typid, int scale, SiftWinxFunc *fs)
{
	const Const *c = (const Const *) n;

	if (!IsA(n, Const) || c->consttype != typid)
		return false;
	if (c->constisnull)
		return true;
	fs->has_default = true;
	switch (typid)
	{
		case INT4OID:
		case DATEOID:
			fs->def_i = DatumGetInt32(c->constvalue);
			return true;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			fs->def_i = DatumGetInt64(c->constvalue);
			return true;
		case FLOAT8OID:
			fs->def_f = DatumGetFloat8(c->constvalue);
			return true;
		default:
			return fs->arg_type == SVEC_NUM64 && numeric_scaled(c->constvalue, scale, &fs->def_i);
	}
}

/* LAG (value [, offset [, default]]), LEAD likewise, FIRST_VALUE (value): the column is the first argument; the offset and the default are constants */
static bool
cell_func_of(SiftV6Input *in, const WindowFunc *wf, SiftV6Winx *w, int f, SiftWinxFunc *fs)
{
	const Node *a = wf->args != NIL ? linitial(wf->args) : NULL;
	int32		offset = 1;
	int			slot;

	if (list_length(wf->args) < 1 || list_length(wf->args) > (fs->kind == SWX_FIRST_VALUE ? 1 : 3) || !IsA(a, Var) || !value_oid_ok(exprType(a)) ||
		(slot = sift_v6_input_add_var(in, (const Var *) a)) < 0)
		return false;
	w->arg_slot[f] = slot;
	w->arg_typid[f] = exprType(a);
	fs->arg_type = in->col[slot].type;
	fs->scale = in->col[slot].scale;
	if (fs->kind == SWX_FIRST_VALUE)
		return true;
	if (list_length(wf->args) >= 2 && (!const_int4(lsecond(wf->args), &offset) || offset < 0 || offset > SIFT_WINX_MAX_OFFSET))
		return false;
	fs->offset = (uint32) offset;
	return list_length(wf->args) < 3 || default_of(lthird(wf->args), w->arg_typid[f], fs->scale, fs);
}

/* one function: its kind, argument (a column of the input) and filter; false when SiftWinx has not the function (shape.known stays false) */
static bool
func_of(SiftV6Input *in, const WindowFunc *wf, SiftV6Winx *w, int f, SiftWinShapeFunc *shape)
{
	SiftWinxFunc *fs = &w->spec.func[f];
	SiftWinxKind kind;

	*fs = (SiftWinxFunc) {.filter = -1};
	w->arg_slot[f] = -1;
	w->arg_typid[f] = InvalidOid;
	*shape = (SiftWinShapeFunc) {0};
	if (!kind_of(wf, &kind))
		return false;
	if (kind == SWX_COUNT && wf->winstar)
		kind = SWX_COUNT_STAR;
	fs->kind = kind;
	shape->kind = kind;
	shape->known = true;
	shape->has_filter = wf->aggfilter != NULL;
	if (kind == SWX_LAG || kind == SWX_LEAD || kind == SWX_FIRST_VALUE)
	{
		if (shape->has_filter || !cell_func_of(in, wf, w, f, fs))
			return false;
		shape->arg_type = fs->arg_type;
	}
	else if (kind >= SWX_COUNT)
	{
		const Node *a = wf->args != NIL ? linitial(wf->args) : NULL;
		int			slot;

		if (list_length(wf->args) != 1 || !IsA(a, Var) || !arg_oid_ok(exprType(a)) || (slot = sift_v6_input_add_var(in, (const Var *) a)) < 0)
			return false;
		if (sift_winx_is_var(kind) && in->col[slot].type == SVEC_NUM128)
			return false;
		w->arg_slot[f] = slot;
		w->arg_typid[f] = exprType(a);
		fs->arg_type = in->col[slot].type;
		fs->scale = in->col[slot].scale;
		shape->arg_type = fs->arg_type;
	}
	else if (wf->args != NIL)
		return false;
	if (shape->has_filter)
		fs->filter = w->nfilters++;
	w->res_typid[f] = result_type(kind, w->arg_typid[f]);
	return wf->wintype == w->res_typid[f];
}

bool
sift_v6_winx_spec(const List *funcs, bool range, SiftV6Input *in, SiftV6Winx *out)
{
	SiftWinShapeFunc shape[SIFT_WINX_MAX_FUNCS];
	SiftWinShape s = {.nfuncs = list_length(funcs),.func = shape,.frame_from_start = true,.window_mode = true};
	int			f = 0;

	*out = (SiftV6Winx) {0};
	if (s.nfuncs > SIFT_WINX_MAX_FUNCS)
		return false;
	out->spec.nfuncs = s.nfuncs;
	out->spec.range_frame = range;
	foreach_node(WindowFunc, wf, (List *) funcs)
	{
		if (wf->runCondition != NIL)
			s.topk = true;
		if (!func_of(in, wf, out, f, &shape[f]))
			return false;
		f++;
	}
	out->spec.max_pending = sift_winx_max_pending(&out->spec, (uint64) work_mem * 1024 / 4);
	return sift_shape_winx(&s) == SSV_OK && sift_winx_lead_fits(&out->spec);
}

bool
sift_v6_winx_filters(MemoryContext cx, const List *funcs, Index rti, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out)
{
	int			k = 0;

	foreach_node(WindowFunc, wf, (List *) funcs)
		if (wf->aggfilter != NULL && !sift_v6_vexpr_quals(cx, list_make1(wf->aggfilter), rti, params, in, &out[k++]))
			return false;
	return true;
}

/* the variance of integers or of a numeric as numeric_stddev_internal gives it: a zero numerator is the numeric 0 (scale 0), else the exact quotient by numeric_div and for a deviation the root at the scale of the quotient */
static Datum
var_numeric(const SiftWinxFunc *fn, const SiftWinxVal *v)
{
	char		num[200],
				den[48];
	Datum		quotient;

	if ((v->var.num.w[0] | v->var.num.w[1] | v->var.num.w[2] | v->var.num.w[3]) == 0)
		return NumericGetDatum(int64_to_numeric(0));
	SIFT_INVARIANT(sift_u256_to_dec(&v->var.num, 2 * fn->scale, num, sizeof(num)) && sift_u128_to_dec(v->var.den, den, sizeof(den)), "window: the variance of a numeric does not fit its text");
	quotient = DirectFunctionCall2(numeric_div, DirectFunctionCall3(numeric_in, CStringGetDatum(num), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)),
								   DirectFunctionCall3(numeric_in, CStringGetDatum(den), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
	if (!sift_winvar_is_stddev(sift_winx_var_kind(fn->kind)))
		return quotient;
	return DirectFunctionCall2(numeric_round, DirectFunctionCall1(numeric_sqrt, quotient), DirectFunctionCall1(numeric_scale, quotient));
}

static Datum
int_datum(Oid typid, int64 v)
{
	return typid == INT4OID || typid == DATEOID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

Datum
sift_v6_winx_datum(const SiftV6Winx *w, int f, const SiftWinxVal *v)
{
	const SiftWinxFunc *fs = &w->spec.func[f];
	const bool	numeric_arg = fs->arg_type == SVEC_NUM64 || fs->arg_type == SVEC_NUM128;

	if (v->nan)
		return sift_v6_numeric_nan();
	switch (fs->kind)
	{
		case SWX_COUNT_STAR:
		case SWX_COUNT:
			return Int64GetDatum((int64) v->n);
		case SWX_SUM:
			if (fs->arg_type == SVEC_F64)
				return Float8GetDatum(v->f);
			if (w->res_typid[f] == NUMERICOID)
				return sift_v6_numeric_from_i128(v->w, fs->scale);
			if (v->w.hi != ((int64) v->w.lo < 0 ? -1 : 0))
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			return Int64GetDatum((int64) v->w.lo);
		case SWX_AVG:
			if (fs->arg_type == SVEC_F64)
				return Float8GetDatum(v->f);
			if (v->avg.ok)
				return sift_numavg_datum(&v->avg);
			return DirectFunctionCall2(numeric_div, sift_v6_numeric_from_i128(v->w, fs->scale), NumericGetDatum(int64_to_numeric((int64) v->n)));
		case SWX_VAR_POP:
		case SWX_VAR_SAMP:
		case SWX_STDDEV_POP:
		case SWX_STDDEV_SAMP:
			return fs->arg_type == SVEC_F64 ? Float8GetDatum(v->f) : var_numeric(fs, v);
		case SWX_MIN:
		case SWX_MAX:
		case SWX_LAG:
		case SWX_LEAD:
		case SWX_FIRST_VALUE:
			if (fs->arg_type == SVEC_F64)
				return Float8GetDatum(v->f);
			if (fs->arg_type == SVEC_NUM128)
				return sift_v6_numeric_from_i128(v->w, fs->scale);
			if (numeric_arg)
				return sift_v6_numeric_from_i128((SiftI128) {.lo = (uint64) v->i,.hi = v->i < 0 ? -1 : 0}, fs->scale);
			return int_datum(w->arg_typid[f], v->i);
		default:
			return Int64GetDatum(v->i);
	}
}

typedef struct Feed
{
	SiftV6Source *src;
	const SiftV6Winx *w;
	const int  *part_slot;
	const int  *ord_slot;
	const SiftV6Vexpr *filters;
	SiftVexprCtx *vctx;
	SiftV6Batch b;
	uint64		bits[SIFT_WINX_MAX_FILTERS][SIFT_VEC_WORDS];
} Feed;

static bool
feed_next(void *p, SiftWinxBatch *out)
{
	Feed	   *fd = p;
	const SiftWinxSpec *s = &fd->w->spec;

	if (!fd->src->next(fd->src, &fd->b))
		return false;
	*out = (SiftWinxBatch) {.n = fd->b.n,.tag = fd->b.tag,.mask = fd->b.mask};
	for (int i = 0; i < s->n_part; i++)
		out->part[i] = &fd->b.col[fd->part_slot[i]];
	for (int i = 0; i < s->n_ord; i++)
		out->ord[i] = &fd->b.col[fd->ord_slot[i]];
	for (int f = 0; f < s->nfuncs; f++)
	{
		out->arg[f] = fd->w->arg_slot[f] >= 0 ? &fd->b.col[fd->w->arg_slot[f]] : NULL;
		if (s->func[f].filter >= 0)
		{
			sift_v6_vexpr_filter(fd->vctx, &fd->filters[s->func[f].filter], &fd->b, fd->bits[s->func[f].filter]);
			out->filter[s->func[f].filter] = fd->bits[s->func[f].filter];
		}
	}
	return true;
}

void
sift_v6_winx_run(MemoryContext cx, const SiftV6Winx *w, const int *part_slot, const int *ord_slot, SiftV6Source *src, const SiftV6Vexpr *filters, SiftWinxEmit emit, void *emit_arg,
				 SiftWinxStats *stats)
{
	Feed	   *fd = MemoryContextAllocAligned(cx, sizeof(Feed), _Alignof(Feed), MCXT_ALLOC_ZERO);
	SiftWinx   *wx;

	fd->src = src;
	fd->w = w;
	fd->part_slot = part_slot;
	fd->ord_slot = ord_slot;
	fd->filters = filters;
	fd->vctx = sift_vexpr_ctx_new(cx);
	wx = sift_winx_create(cx, &w->spec, feed_next, fd, emit, emit_arg);
	sift_winx_run(wx);
	if (stats != NULL)
		*stats = *sift_winx_stats(wx);
	sift_winx_free(wx);
	sift_vexpr_ctx_free(fd->vctx);
	pfree(fd);
}
