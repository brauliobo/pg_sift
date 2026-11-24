/*
 * agg/v6/group_out.c - the groups of SiftAggV6 as the Datums of the plan: the numeric of a scaled 128-bit integer, the key columns of a slot, and the value of each aggregate from its state.
 */
#include "postgres.h"

#include <math.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/numeric.h"

#include "agg/group/ht.h"
#include "agg/group/state.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_gen.h"
#include "agg/v6/group_nan.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_argext.h"
#include "agg/v6/v6_datum.h"


static Datum
nan_datum(void)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

/* sum and avg of a float: PostgreSQL's float8 sum raises the overflow of finite numbers, and the bins of a group hold at most V6FSUM_MAX_ROWS */
static Datum
sum_f64_datum(const SiftV6Agg *a, const V6Acc *acc, bool *isnull)
{
	const int64 n = sift_agg_exact_float ? acc->n : v6fsum_count(acc);
	const double sum = sift_agg_exact_float ? v6acc_f64(acc) : v6fsum_value(acc);

	*isnull = n == 0;
	if (n == 0)
		return (Datum) 0;
	if ((uint64) n >= V6FSUM_MAX_ROWS && !sift_agg_exact_float)
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("float8 sum over more than %lld rows", (long long) V6FSUM_MAX_ROWS)));
	if (isinf(sum) && !sift_agg_exact_float && v6fsum_flags(acc) == 0)
		float_overflow_error();
	if (sift_v6_kind_base(a->kind) == SV6_SUM && a->arg.typid == FLOAT4OID)
		return Float4GetDatum((float4) sum);
	return Float8GetDatum(sift_v6_kind_base(a->kind) == SV6_SUM ? sum : sum / (double) n);
}

static Datum
scaled_numeric_text(SiftI128 v, int scale)
{
	char		rev[48],
				out[64];
	int			n = 0,
				o = 0;
	unsigned __int128 u = ((unsigned __int128) (uint64) v.hi << 64) | v.lo;
	const bool	neg = v.hi < 0;

	if (neg)
		u = -u;
	do
	{
		rev[n++] = '0' + (int) (u % 10);
		u /= 10;
	} while (u != 0 || n <= scale);
	if (neg)
		out[o++] = '-';
	for (int i = n - 1; i >= 0; i--)
	{
		out[o++] = rev[i];
		if (i == scale && scale > 0)
			out[o++] = '.';
	}
	out[o] = '\0';
	return DirectFunctionCall3(numeric_in, CStringGetDatum(out), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

/* The digits of PostgreSQL's numeric straight from the 128-bit integer (v6_acc.h), through its wire format: no decimal string is made and parsed; only a value too wide for them takes the text */
Datum
sift_v6_scaled_numeric(SiftI128 v, int scale)
{
	int16		digits[V6_NUMERIC_MAX_DIGITS];
	StringInfoData wire;
	Datum		out;
	bool		neg;
	int			weight,
				nd = sift_v6_i128_digits(v, scale, &neg, digits, &weight);

	if (nd < 0)
		return scaled_numeric_text(v, scale);
	/* MEM[MEM-AGG-03]: sift_v6_scaled_numeric wire buffer (StringInfo of 1KB per numeric value, freed after numeric_recv; was left in the group context until the query ended) */
	initStringInfo(&wire);
	pq_sendint16(&wire, nd);
	pq_sendint16(&wire, weight);
	pq_sendint16(&wire, neg ? V6_NUMERIC_SIGN_NEG : 0);
	pq_sendint16(&wire, scale);
	for (int i = 0; i < nd; i++)
		pq_sendint16(&wire, digits[i]);
	out = DirectFunctionCall3(numeric_recv, PointerGetDatum(&wire), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	pfree(wire.data);
	return out;
}

Datum
sift_v6_i128_numeric(SiftI128 v)
{
	return sift_v6_scaled_numeric(v, 0);
}

Datum
v6g_int_datum(Oid typid, int64 x)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) x);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) x);
		case BOOLOID:
			return BoolGetDatum(x != 0);
		case NUMERICOID:
			return NumericGetDatum(int64_to_numeric(x));
		default:
			return Int64GetDatum(x);
	}
}

static Datum
grouping_datum(const SiftV6Func *f, uint32 mask)
{
	int32		v = 0;

	for (int i = 0; i < f->nargs; i++)
		v = (v << 1) | !((mask >> f->key[i]) & 1);
	return Int32GetDatum(v);
}

Datum
v6g_agg_datum(const SiftV6Group *g, const SiftV6Agg *a, const V6Acc *acc, uint32 mask, bool *isnull)
{
	const SiftV6Spec *spec = g->spec;

	*isnull = false;
	if (v6g_is_gen(a))
		return v6_gen_datum(g->gen, (int) (a - spec->agg), (V6Acc *) acc, isnull);
	if (v6g_is_ext(a))
		return v6g_ext_datum(g, a, acc, isnull);
	switch (a->kind)
	{
		case SV6_GROUPING:
			return grouping_datum(&spec->func[a->arg.attno], mask);
		case SV6_COUNT_STAR:
			return Int64GetDatum(acc->n);
		case SV6_COUNT:
			return Int64GetDatum(v6acc_count(a, acc));
		case SV6_ARG_MAX:
		case SV6_ARG_MIN:
			*isnull = acc->n == 0 || acc->aux != 0;
			return *isnull ? (Datum) 0 : v6g_int_datum(a->arg.typid, acc->v1);
		case SV6_COUNT_DISTINCT:
			return Int64GetDatum(v6acc_count(a, acc));
		case SV6_SUM:
		case SV6_AVG:
		case SV6_SUM_DISTINCT:
		case SV6_AVG_DISTINCT:
			if (sift_v6_class(a->arg.typid) == SV6C_F64)
				return sum_f64_datum(a, acc, isnull);
			if (v6acc_is_nan(a, acc))
				return nan_datum();
			*isnull = acc->n == 0;
			if (acc->n == 0)
				return (Datum) 0;
			if (sift_v6_kind_base(a->kind) == SV6_AVG)
				return DirectFunctionCall2(numeric_div, sift_v6_scaled_numeric(v6acc_i128(acc), a->arg.scale), NumericGetDatum(int64_to_numeric(acc->n)));
			if (a->restype == INT8OID)
			{
				const SiftI128 s = v6acc_i128(acc);

				if ((s.hi == 0 && (int64) s.lo >= 0) || (s.hi == -1 && (int64) s.lo < 0))
					return Int64GetDatum((int64) s.lo);
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			}
			return sift_v6_scaled_numeric(v6acc_i128(acc), a->arg.scale);
		default:
			if (v6acc_is_nan(a, acc))
				return nan_datum();
			*isnull = acc->n == 0;
			if (acc->n == 0)
				return (Datum) 0;
			if (sift_v6_class(a->arg.typid) == SV6C_TEXT)
			{
				uint8		buf[16];

				return PointerGetDatum(cstring_to_text_with_len((const char *) sift_strmm_bytes((const SiftStrMM *) acc, buf), ((const SiftStrMM *) acc)->len));
			}
			if (a->arg.typid == FLOAT4OID)
				return Float4GetDatum((float4) v6acc_f64(acc));
			if (a->arg.typid == FLOAT8OID)
				return Float8GetDatum(v6acc_f64(acc));
			if (a->arg.typid == NUMERICOID)
				return sift_v6_scaled_numeric((SiftI128) {.lo = acc->v0,.hi = (int64) acc->v0 < 0 ? -1 : 0}, a->arg.scale);
			return v6g_int_datum(a->arg.typid, (int64) acc->v0);
	}
}

static void
raw_key_datums(const SiftV6Group *g, const V6Set *set, const SiftSaltedHT *ht, const SiftAggSlot64 *s, Datum *values, bool *isnull, uint32 want)
{
	const SiftV6Spec *spec = g->spec;

	if (set->packed)
	{
		for (int i = 0; i < set->nk; i++)
		{
			int64		x = 0;
			const int	k = set->kidx[i];

			if (!((want >> k) & 1))
				continue;
			isnull[k] = !sift_pack_get(&set->pack, &s->key, i, &x);
			values[k] = isnull[k] ? (Datum) 0 : v6g_int_datum(spec->key[k].typid, x);
		}
		return;
	}
	if (!set->wide)
	{
		isnull[set->kidx[0]] = s->key.i64[1] != 0;
		values[set->kidx[0]] = isnull[set->kidx[0]] ? (Datum) 0 : v6g_int_datum(spec->key[set->kidx[0]].typid, s->key.i64[0]);
		return;
	}
	{
		uint32		len;
		const uint8 *p = sift_ht_wide_key(ht, s, &len);

		for (int i = 0; i < set->nk; i++)
		{
			const int	k = set->kidx[i];

			isnull[k] = (s->key.wide.key_nulls >> i) & 1;
			if (((want >> k) & 1) && isnull[k])
				values[k] = (Datum) 0;
			if (isnull[k])
				continue;
			if (sift_v6_class(spec->key[k].typid) == SV6C_TEXT)
			{
				uint32		n;

				memcpy(&n, p, sizeof(n));
				if ((want >> k) & 1)
					values[k] = PointerGetDatum(cstring_to_text_with_len((const char *) p + sizeof(n), n));
				p += sizeof(n) + n;
			}
			else
			{
				int64		x;

				memcpy(&x, p, sizeof(x));
				if ((want >> k) & 1)
					values[k] = v6g_int_datum(spec->key[k].typid, x);
				p += sizeof(x);
			}
		}
	}
}

void
v6g_key_datums(const SiftV6Group *g, const V6Set *set, const SiftSaltedHT *ht, const SiftAggSlot64 *s, Datum *values, bool *isnull, uint32 want)
{
	raw_key_datums(g, set, ht, s, values, isnull, want);
	if (g->gkeys != NULL)
		sift_v6_genkeys_resolve(g->gkeys, values, isnull, want);
}
