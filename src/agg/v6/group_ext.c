/*
 * agg/v6/group_ext.c - the aggregates of agg/v6/group_ext.h in the groups of SiftAggV6: the rows folded into a V6Acc, the states merged when a group is resolved from a spill or a worker, and the
 * Datums they leave as. group.c calls these for the kinds v6g_is_ext names.
 */
#include "postgres.h"

#include "utils/builtins.h"

#include "agg/v6/group_ext.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_pct.h"

static const struct
{
	const char *name;
	SiftV6Kind	kind;
	SiftMomFinal final;
} moment_kinds[] = {
	{"stddev", SV6_STDDEV_SAMP, SMF_STDDEV_SAMP},
	{"stddev_samp", SV6_STDDEV_SAMP, SMF_STDDEV_SAMP},
	{"stddev_pop", SV6_STDDEV_POP, SMF_STDDEV_POP},
	{"variance", SV6_VAR_SAMP, SMF_VAR_SAMP},
	{"var_samp", SV6_VAR_SAMP, SMF_VAR_SAMP},
	{"var_pop", SV6_VAR_POP, SMF_VAR_POP},
};

static const struct
{
	const char *name;
	SiftV6Kind	kind;
} bool_kinds[] = {
	{"bool_or", SV6_BOOL_OR},
	{"bool_and", SV6_BOOL_AND},
	{"every", SV6_BOOL_AND},
};

bool
sift_v6_ext_kind(const char *name, Oid argtype, SiftV6Kind *kind)
{
	if (argtype == FLOAT8OID || argtype == FLOAT4OID)
		for (int i = 0; i < lengthof(moment_kinds); i++)
			if (strcmp(name, moment_kinds[i].name) == 0)
			{
				*kind = moment_kinds[i].kind;
				return true;
			}
	if (argtype == BOOLOID)
		for (int i = 0; i < lengthof(bool_kinds); i++)
			if (strcmp(name, bool_kinds[i].name) == 0)
			{
				*kind = bool_kinds[i].kind;
				return true;
			}
	return false;
}

static SiftMomFinal
moment_final(SiftV6Kind kind)
{
	for (int i = 0; i < lengthof(moment_kinds); i++)
		if (moment_kinds[i].kind == kind)
			return moment_kinds[i].final;
	Assert(false);
	return SMF_VAR_POP;
}

void
v6g_ext_fold_row(SiftV6Group *g, const SiftV6Agg *a, V6Acc *acc, const SiftVec *v, uint16 r)
{
	switch (a->kind)
	{
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
			sift_mom_add((SiftMom *) acc, ((const double *) v->data)[r]);
			break;
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
			sift_bool_offer((SiftBoolAcc *) acc, a->kind == SV6_BOOL_OR, int_at(v, r) != 0);
			break;
		case SV6_PCT_CONT:
			sift_v6_pct_add(&g->tx, acc, ((const double *) v->data)[r]);
			break;
		default:
			{
				const SiftStrView *s = &((const SiftStrView *) v->data)[r];

				sift_tslot_offer(&g->tx, (SiftTextSlot *) acc, a->kind == SV6_MIN, (const char *) sift_strview_bytes(s), s->len, NULL, NULL);
			}
	}
}

void
v6g_ext_plain(SiftV6Group *g, const SiftV6Agg *a, V6Acc *acc, const SiftVec *v, const uint64 *take, uint16 n_raw)
{
	for (uint16 r = 0; r < n_raw; r++)
		if (((take[r >> 6] >> (r & 63)) & 1) && is_valid(v, r))
			v6g_ext_fold_row(g, a, acc, v, r);
}

void
v6g_ext_merge(SiftV6Group *g, const SiftV6Agg *a, V6Acc *d, const V6Acc *s)
{
	switch (a->kind)
	{
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
			sift_mom_merge((SiftMom *) d, (const SiftMom *) s);
			break;
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
			sift_bool_merge((SiftBoolAcc *) d, (const SiftBoolAcc *) s, a->kind == SV6_BOOL_OR);
			break;
		case SV6_PCT_CONT:
			sift_v6_pct_merge(&g->tx, d, s);
			break;
		default:
			sift_tslot_merge(&g->tx, (SiftTextSlot *) d, (const SiftTextSlot *) s, a->kind == SV6_MIN, NULL, NULL);
	}
}

Datum
v6g_ext_datum(const SiftV6Group *g, const SiftV6Agg *a, const V6Acc *acc, bool *isnull)
{
	double		x = 0.0;

	*isnull = acc->n == 0;
	if (*isnull)
		return (Datum) 0;
	switch (a->kind)
	{
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
			if (((const SiftMom *) acc)->flags & SIFT_MOM_OVERFLOW)
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
			*isnull = !sift_mom_final(moment_final(a->kind), (const SiftMom *) acc, &x);
			return *isnull ? (Datum) 0 : Float8GetDatum(x);
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
			return BoolGetDatum(((const SiftBoolAcc *) acc)->v != 0);
		case SV6_PCT_CONT:
			return Float8GetDatum(sift_v6_pct_final(&g->tx, acc, a->frac));
		default:
			{
				uint32		len = 0;
				const char *p = sift_tslot_bytes(&g->tx, (const SiftTextSlot *) acc, &len);

				Assert(sift_v6_class(a->arg.typid) == SV6C_TEXT);
				return PointerGetDatum(cstring_to_text_with_len(p, len));
			}
	}
}
