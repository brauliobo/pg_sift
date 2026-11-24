/*
 * agg/v6/v6_win_func.c - the window functions of pg_catalog the engine runs (agg/window.h), and the spec of each one of a SiftAggV6Window plan: its kind, the column of its argument, its result type.
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_win_func.h"

static bool
is_arg_type(const SiftV6InCol *c)
{
	return c->type == SVEC_I32 || c->type == SVEC_I64 || c->type == SVEC_F64;
}

static int64
datum_int(Oid typid, Datum d)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static bool
lag_value(const Node *n, Oid typid, SiftWinFuncSpec *fs)
{
	const Const *c = (const Const *) n;

	if (!IsA(n, Const) || c->consttype != typid)
		return false;
	if (c->constisnull)
		return true;
	fs->has_default = true;
	if (typid == FLOAT8OID)
		fs->default_val.f = DatumGetFloat8(c->constvalue);
	else
		fs->default_val.i = datum_int(typid, c->constvalue);
	return true;
}

/* the name of a window function of pg_catalog and the kind of the engine that runs it */
static bool
kind_of(const WindowFunc *wf, SiftWinFuncKind *kind)
{
	static const struct
	{
		const char *name;
		SiftWinFuncKind kind;
	}			kinds[] = {{"row_number", SWIN_ROW_NUMBER}, {"rank", SWIN_RANK}, {"dense_rank", SWIN_DENSE_RANK}, {"lag", SWIN_LAG}, {"lead", SWIN_LEAD},
	{"first_value", SWIN_FIRST_VALUE}, {"count", SWIN_COUNT}, {"sum", SWIN_SUM}, {"avg", SWIN_AVG}, {"min", SWIN_MIN}, {"max", SWIN_MAX}};
	const char *name;

	if (get_func_namespace(wf->winfnoid) != PG_CATALOG_NAMESPACE || (name = get_func_name(wf->winfnoid)) == NULL)
		return false;
	for (int i = 0; i < lengthof(kinds); i++)
		if (strcmp(name, kinds[i].name) == 0)
			return (*kind = kinds[i].kind, true);
	return false;
}

/* the type PostgreSQL gives the result of the function over an argument of type arg */
static Oid
result_type(SiftWinFuncKind kind, Oid arg)
{
	switch (kind)
	{
		case SWIN_ROW_NUMBER:
		case SWIN_RANK:
		case SWIN_DENSE_RANK:
		case SWIN_COUNT_STAR:
		case SWIN_COUNT:
			return INT8OID;
		case SWIN_SUM:
			return arg == INT2OID || arg == INT4OID ? INT8OID : arg == INT8OID ? NUMERICOID : FLOAT8OID;
		case SWIN_AVG:
			return arg == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		default:
			return arg;
	}
}

/* the argument types of a function: numbers for all of them, and for the functions that only compare or hand back a value a date or a timestamp too (an integer here) */
static bool
arg_type_ok(SiftWinFuncKind kind, Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT8OID:
			return true;
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return kind != SWIN_SUM && kind != SWIN_AVG;
		default:
			return kind == SWIN_COUNT;
	}
}

bool
sift_v6_win_func_spec(SiftV6Input *in, const WindowFunc *wf, bool range, SiftV6WindowSpec *s, int f)
{
	SiftWinFuncSpec *fs = &s->win.funcs[f];
	const int	nargs = list_length(wf->args);
	SiftWinFuncKind kind;
	const bool	takes_arg = nargs > 0 && !wf->winstar;

	*fs = (SiftWinFuncSpec) {0};
	s->arg_slot[f] = -1;
	if (wf->aggfilter != NULL || !kind_of(wf, &kind))
		return false;
	if (kind == SWIN_COUNT && wf->winstar)
		kind = SWIN_COUNT_STAR;
	fs->kind = kind;
	if (takes_arg)
	{
		const Node *a = linitial(wf->args);
		int			slot;

		if (!IsA(a, Var) || (slot = sift_v6_input_add_var(in, (Var *) a)) < 0 || !is_arg_type(&in->col[slot]))
			return false;
		s->arg_slot[f] = slot;
		s->arg_typid[f] = in->col[slot].typid;
		fs->arg_type = in->col[slot].type;
		if (!arg_type_ok(kind, in->col[slot].typid))
			return false;
	}
	if (kind >= SWIN_COUNT_STAR)
		fs->range_frame = range;
	if (kind == SWIN_LAG || kind == SWIN_LEAD)
	{
		const Const *off = nargs > 1 ? lsecond(wf->args) : NULL;

		fs->offset = 1;
		if (nargs > 1 && (!IsA(off, Const) || off->constisnull || off->consttype != INT4OID || DatumGetInt32(off->constvalue) < 0 ||
						  DatumGetInt32(off->constvalue) > SIFT_WIN_MAX_OFFSET))
			return false;
		if (nargs > 1)
			fs->offset = (uint32) DatumGetInt32(off->constvalue);
		if (nargs > 2 && !lag_value(lthird(wf->args), s->arg_typid[f], fs))
			return false;
	}
	if ((takes_arg != (kind != SWIN_ROW_NUMBER && kind != SWIN_RANK && kind != SWIN_DENSE_RANK && kind != SWIN_COUNT_STAR)) ||
		(kind != SWIN_LAG && kind != SWIN_LEAD && nargs > 1) || nargs > 3)
		return false;
	s->res_typid[f] = result_type(kind, s->arg_typid[f]);
	return wf->wintype == s->res_typid[f];
}
