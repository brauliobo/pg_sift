/*
 * agg/update/select.c - the selection and the values of a chunk
 * The aggregate map of a column, the rows a chunk keeps, and the values of an aggregate's
 * expression.
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/dense_int.h"
#include "col/am.h"

/* the id (1..) over all dictionaries of entry c of d, for the values count(DISTINCT) of aggregate ai counts */
uint32
amap_resolve(AggExec *ex, const SiftSrcDict *d, int ai, uint32 c)
{
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint32		len;
	char		buf[SIFT_SRC_DICT_ENTRY];
	const char *str = sift_srcdict_str(d, c, &len, buf);
	uint32		id = ot_id(&ex->aot[ai], text_datum(ex->spec->agg[ai].argtype, -1, str, len));

	MemoryContextSwitchTo(old);
	return id;
}

uint32 *
dict_amap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ai)
{
	int			slot = ex->spec->nkeys + ai;
	uint32	   *map = s->kmap[slot];

	if (map)
		return map;
	map = MemoryContextAlloc(ex->tcxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	memset(map, 0xFF, d->n * sizeof(uint32));
	map[d->n] = 0;
	s->kmap[slot] = map;
	return map;
}

/* okb[j] = 0 for the tuples whose value of the leaf is NULL; true when the leaf has NULLs */
static bool
clear_null_rows(AggExec *ex, int leaf, int nsel)
{
	const SiftSrcVec *v = get_vec(ex, leaf);
	const uint32 *idx;

	if (v->isnull == NULL)
		return false;
	gather_idx(ex, leaf, nsel, &idx);
	for (int j = 0; j < nsel; j++)
		ex->okb[j] &= !v->isnull[idx[j]];
	return true;
}

/* the aggregate's selection among the nsel tuples: its FILTER, a non-null argument; the bitmap goes to ex->okm */
void
agg_selection(AggExec *ex, SiftAggAgg *a, int start, int n, int nsel, bool *all)
{
	bool		any = false;

	memset(ex->okb, 1, nsel);
	if (a->filter && a->filter->level == 2)
	{
		const SiftAggPred *filter = a->filter;
		const uint32 *out = combo_outs(ex, filter->leaf);

		Assert(filter->kind == SAP_COMBO);
		for (int j = 0; j < nsel; j++)
			ex->okb[j] = combo_val(ex, filter->leaf, out[j], filter->op) != 0;
		any = true;
	}
	else if (a->filter)
	{
		uint64		fm[VECW + PADW],
					al[VECW + PADW];

		/* the FILTER is evaluated for the tuples of the chunk only: it may raise an error for a row the query does not reach */
		if (a->filter->late)
		{
			clear_words(al, n);
			for (int j = 0; j < nsel; j++)
				al[ex->sel[j] >> 6] |= UINT64CONST(1) << (ex->sel[j] & 63);
			ex->alive = al;
		}
		eval_pred(ex, a->filter, ex->qlevel, start, n, fm);
		ex->alive = NULL;
		for (int j = 0; j < nsel; j++)
			ex->okb[j] = sift_colsrc_bit(fm, ex->sel[j]);
		any = true;
	}
	if (a->leaf >= 0)
		any |= clear_null_rows(ex, a->leaf, nsel);
	if (a->wsum > 0)
		any |= clear_null_rows(ex, a->wsum - 1, nsel);
	for (int k = 0; k < a->nvx; k++)
		if (a->vx[k].op == SVX_LEAF)
			any |= clear_null_rows(ex, a->vx[k].leaf, nsel);
	for (int k = 0; k < a->num.n; k++)
		if (a->num.step[k].op == SNX_LEAF)
			any |= clear_null_rows(ex, a->num.step[k].leaf, nsel);
	*all = !any;
	if (any)
		sift_colk_nonzero(ex->okb, nsel, ex->okm);
}

/* The value of the float8 argument program of aggregate a for the nsel tuples (in double, NULL operands are not looked at) */
const double *
eval_vx(AggExec *ex, SiftAggAgg *a, int nsel)
{
	int			sp = 0;
	const int	stride = SIFT_AGG_VEC + PADW;

#define VS(i)	(ex->vstk + (Size) (i) * stride)
	for (int k = 0; k < a->nvx; k++)
	{
		const SiftAggVx *x = &a->vx[k];
		double	   *top = VS(sp);

		switch (x->op)
		{
			case SVX_LEAF:
				{
					const SiftSrcVec *v = get_vec(ex, x->leaf);
					const uint32 *idx;

					gather_idx(ex, x->leaf, nsel, &idx);
					if (ex->lay->leaf[x->leaf].vtype == SCV_I32)
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int32 *) v->data)[idx[j]];
					else if (ex->lay->leaf[x->leaf].vtype == SCV_I64)	/* numeric: the integer over 10^scale, in c */
						for (int j = 0; j < nsel; j++)
							top[j] = v->ovf && sift_colsrc_bit(v->ovf, idx[j]) ? get_float8_nan() : (double) ((const int64 *) v->data)[idx[j]] / x->c;
					else
						for (int j = 0; j < nsel; j++)
							top[j] = ((const double *) v->data)[idx[j]];
					sp++;
				}
				break;
			case SVX_CONST:
				for (int j = 0; j < nsel; j++)
					top[j] = x->c;
				sp++;
				break;
			case SVX_NEG:
				for (int j = 0; j < nsel; j++)
					VS(sp - 1)[j] = -VS(sp - 1)[j];
				break;
			case SVX_ADD:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] += VS(sp - 1)[j];
				sp--;
				break;
			case SVX_SUB:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] -= VS(sp - 1)[j];
				sp--;
				break;
			case SVX_MUL:
				for (int j = 0; j < nsel; j++)
					VS(sp - 2)[j] *= VS(sp - 1)[j];
				sp--;
				break;
		}
	}
#undef VS
	return ex->vstk;
}

/*
 * PostgreSQL's float8 aggregates raise errors on overflow that a plain double sum would hide as Infinity. sum(): the error is
 * raised at the end, when a group's sum is infinite and none of its inputs was (nf marks those). avg() keeps a sum of squares
 * (float8_accum) that overflows first: it is not tracked, the error is raised when the inputs are so large that it could have.
 */
void
float_guard(AggExec *ex, int ai, const double *x, int nsel)
{
	AggAcc	   *acc = &ex->acc[ai];
	double		m = 0;
	bool		bad = false;

	for (int j = 0; j < nsel; j++)
	{
		bad |= ex->okb[j] && !isfinite(x[j]);
		m = Max(m, ex->okb[j] ? fabs(x[j]) : 0);
	}
	if (bad)
	{
		for (int j = 0; j < nsel; j++)
			if (ex->okb[j] && !isfinite(x[j]))
				acc->nf[ex->gid[j]] = 1;
		return;
	}
	ex->amax[ai] = Max(ex->amax[ai], m);
	if (ex->spec->agg[ai].kind == SAA_AVG_F64 && 8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
		float_overflow_error();
}
