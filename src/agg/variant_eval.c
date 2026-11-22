/*
 * agg/variant_eval.c - the exception stream of a shredded JSONB path read without the heap (agg/variant_eval.h).
 */
#include "core/host.h"

#include <math.h>

#include "agg/group/state.h"
#include "agg/kern6.h"
#include "agg/variant_eval.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

typedef struct ExcVal
{
	int64		i;
	double		f;
} ExcVal;

/* float8 order: a NaN is greater than every number and equal to itself */
static inline bool
f8_lt(double a, double b)
{
	return !isnan(a) && (isnan(b) || a < b);
}

static void
numeric_value(Numeric n, SiftVecType type, ExcVal *out)
{
	if (type == SVEC_F64)
		out->f = DatumGetFloat8(DirectFunctionCall1(numeric_float8, NumericGetDatum(n)));
	else
		out->i = DatumGetInt64(DirectFunctionCall1(numeric_int8, NumericGetDatum(n)));
}

static void
text_value(const char *s, SiftVecType type, ExcVal *out)
{
	if (type == SVEC_F64)
		out->f = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(s)));
	else
		out->i = DatumGetInt64(DirectFunctionCall1(int8in, CStringGetDatum(s)));
}

/* false: the value is NULL */
static bool
exc_eval(SiftVariantKind kind, const uint8 *payload, uint32 len, SiftVecType type, ExcVal *out)
{
	bool		isnull = false;

	if (kind == SVAR_CAST_ERROR)
	{
		char	   *s = pnstrdup((const char *) payload, len);

		text_value(s, type, out);
		pfree(s);
		return true;
	}
	{
		uint8	   *buf = palloc(len);

		memcpy(buf, payload, len);
		if (kind == SVAR_NUM_SPECIAL)
			numeric_value((Numeric) buf, type, out);
		else
		{
			Jsonb	   *jb = (Jsonb *) buf;
			JsonbValue	jv;

			if (!JsonbExtractScalar(&jb->root, &jv))
				text_value(JsonbToCString(NULL, &jb->root, (int) len), type, out);
			else if (jv.type == jbvNull)
				isnull = true;
			else if (jv.type == jbvNumeric)
				numeric_value(jv.val.numeric, type, out);
			else if (jv.type == jbvBool)
				text_value(jv.val.boolean ? "true" : "false", type, out);
			else
			{
				char	   *s = pnstrdup(jv.val.string.val, jv.val.string.len);

				text_value(s, type, out);
				pfree(s);
			}
		}
		pfree(buf);
	}
	return !isnull;
}

void
sift_var_agg_init(SiftVarAgg *a, SiftVecType type)
{
	Assert(type == SVEC_F64 || type == SVEC_I64);
	memset(a, 0, sizeof(*a));
	a->type = type;
}

void
sift_var_agg_add_vec(SiftVarAgg *a, const SiftVec *v)
{
	for (uint16 i = 0; i < v->len; i++, a->count++)
		if (a->type == SVEC_F64)
		{
			const double x = ((const double *) v->data)[i];

			a->fsum += x;
			if (a->count == 0 || f8_lt(x, a->min.f))
				a->min.f = x;
			if (a->count == 0 || f8_lt(a->max.f, x))
				a->max.f = x;
		}
		else
		{
			const int64 x = ((const int64 *) v->data)[i];

			sift_i128_add(&a->isum, sift_i128_of_i64(x));
			if (a->count == 0 || x < a->min.i)
				a->min.i = x;
			if (a->count == 0 || x > a->max.i)
				a->max.i = x;
		}
}

void
sift_var_agg_merge(SiftVarAgg *dst, const SiftVarAgg *src)
{
	Assert(dst->type == src->type);
	dst->rows += src->rows;
	if (src->count == 0)
		return;
	if (dst->type == SVEC_F64)
	{
		if (dst->count == 0 || f8_lt(src->min.f, dst->min.f))
			dst->min.f = src->min.f;
		if (dst->count == 0 || f8_lt(dst->max.f, src->max.f))
			dst->max.f = src->max.f;
		dst->fsum += src->fsum;
	}
	else
	{
		if (dst->count == 0 || src->min.i < dst->min.i)
			dst->min.i = src->min.i;
		if (dst->count == 0 || src->max.i > dst->max.i)
			dst->max.i = src->max.i;
		sift_i128_add(&dst->isum, src->isum);
	}
	dst->count += src->count;
}

uint16
sift_variant_primary_mask(uint64 *mask, const SiftVariantExcStream *exc, uint32 slice, uint16 n)
{
	if (exc == NULL)
		return sift_aggk6->mask_and(mask, mask, mask, n);
	return sift_aggk6->mask_andn_del(mask, exc->exc_mask + (size_t) slice * SIFT_VEC_WORDS, n);
}

SiftVariantVals *
sift_variant_vals_new(void)
{
	return MemoryContextAllocAligned(CurrentMemoryContext, sizeof(SiftVariantVals), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

/* The first entry whose row is at or after row */
static uint32
first_entry(const SiftVariantExcStream *exc, uint32 row)
{
	uint32		lo = 0,
				hi = exc->n_entries;

	while (lo < hi)
	{
		const uint32 mid = (lo + hi) / 2;

		if (exc->entries[mid].row_idx < row)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

void
sift_variant_eval(SiftVariantVals *out, const SiftVariantExcStream *exc, uint32 slice, const uint64 *active, uint16 n,
				  SiftVecType type)
{
	const uint32 base = slice * SIFT_VEC_LEN,
				end = base + n;
	uint16		k = 0;

	out->n_active = 0;
	for (uint32 i = exc ? first_entry(exc, base) : 0; exc && i < exc->n_entries && exc->entries[i].row_idx < end; i++)
	{
		const SiftVariantExcEntry *e = &exc->entries[i];
		const uint32 row = e->row_idx - base;
		ExcVal		val;

		if (active && !((active[row >> 6] >> (row & 63)) & 1))
			continue;
		out->n_active++;
		if (!exc_eval(e->kind, exc->payload_buf + e->payload_off, e[1].payload_off - e->payload_off, type, &val))
			continue;
		out->rows[k] = row;
		if (type == SVEC_F64)
			out->v.f64[k] = val.f;
		else
			out->v.i64[k] = val.i;
		k++;
	}
	out->vec = (SiftVec) {.data = &out->v, .len = k, .all_valid = true, .dense = true, .type = type};
}

void
sift_variant_fold_granule(SiftVarAgg *a, const SiftVariantExcStream *exc, const uint64 *active, uint32 n_rows)
{
	SiftVariantVals *vals;

	if (exc == NULL)
		return;
	vals = sift_variant_vals_new();
	for (uint32 slice = 0; slice * SIFT_VEC_LEN < n_rows; slice++)
	{
		const uint16 n = (uint16) Min(SIFT_VEC_LEN, n_rows - slice * SIFT_VEC_LEN);

		sift_variant_eval(vals, exc, slice, active ? active + (size_t) slice * SIFT_VEC_WORDS : NULL, n, a->type);
		a->rows += vals->n_active;
		sift_var_agg_add_vec(a, &vals->vec);
	}
	pfree(vals);
}
