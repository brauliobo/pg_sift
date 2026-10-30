/*
 * agg/update/sums.c - the sums of a chunk into the groups
 * The add of a value to a mapped, a direct and a wide group, and the numeric sum.
 */
#include "postgres.h"

#include "utils/datum.h"
#include "utils/float.h"

#include "col/am.h"
#include "agg/update/int.h"

/*
 * An aggregate of an expression over a column of a joined table: the combo gives the id (0: NULL) of the value of each tuple, in the
 * table of the distinct values of the aggregate. count(x) counts the ids; the others add the (group, id) pairs to a set (with the
 * times each was added, for sum and avg) that mapped_finish turns into the result with PostgreSQL's own comparison and arithmetic.
 */
void
mapped_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const uint32 *out = combo_outs(ex, a->combo);

	for (int j = 0; j < nsel; j++)
	{
		uint32		id = combo_val(ex, a->combo, out[j], a->slot);

		if (j + DSET_AHEAD < nsel && a->kind != SAA_M_COUNT)
		{
			uint32		ahead = combo_val(ex, a->combo, out[j + DSET_AHEAD], a->slot);

			if (ahead)
				dset_prefetch(&ex->dset[ai], ex->gid[j + DSET_AHEAD], ahead);
		}
		if (id == 0 || (ok && !sift_colsrc_bit(ok, j)))
			continue;
		if (a->kind == SAA_M_COUNT || (dset_add(ex, &ex->dset[ai], ex->gid[j], id) && a->kind == SAA_M_DISTINCT))
			acc->n[ex->gid[j]]++;
	}
}

/*
 * x (m: its integer and scale) added to the sum of group g. The sum is an integer at the largest scale seen (the dscale PostgreSQL
 * gives a sum of numerics) until a value or a sum does not fit 38 digits, or is NaN or infinite: from then on it is a numeric
 * (acc->nf) and numeric_add does the rest.
 */
void
numeric_sum_add(AggAcc *acc, uint32 g, Datum x, const DNum *m)
{
	const sift_i128 *p10;
	sift_i128	w = acc->w[g],
				v = m->m;
	int			sc = (int) acc->i[g];

	/* the usual row: the sum is an integer at the row's scale and the row's integer fits */
	if (!acc->nf[g] && m->ok && acc->n[g] > 0 && m->scale == sc)
	{
		sift_i128	t;

		if (!__builtin_add_overflow(w, v, &t))
		{
			acc->w[g] = t;
			return;
		}
	}
	p10 = pow10_tab();
	if (!acc->nf[g] && m->ok)
	{
		bool		bad = false;

		if (acc->n[g] == 0)
		{
			w = 0;
			sc = m->scale;
		}
		else if (m->scale > sc)
		{
			bad = __builtin_mul_overflow(w, p10[m->scale - sc], &w);
			sc = m->scale;
		}
		else if (m->scale < sc)
			bad = __builtin_mul_overflow(v, p10[sc - m->scale], &v);
		if (!bad && !__builtin_add_overflow(w, v, &w))
		{
			acc->w[g] = w;
			acc->i[g] = sc;
			return;
		}
	}
	if (x == (Datum) 0)
		x = sift_col_w128_numeric(m->m, m->scale);
	if (!acc->nf[g])
	{
		/* leaves the integer form */
		acc->nf[g] = 1;
		if (acc->n[g] > 0)
			acc->v[g] = sift_col_w128_numeric(acc->w[g], (int) acc->i[g]);
	}
	if (acc->n[g] == 0)
		acc->v[g] = datumCopy(x, false, -1);
	else
	{
		Datum		sum = DirectFunctionCall2(numeric_add, acc->v[g], x);

		pfree(DatumGetPointer(acc->v[g]));
		acc->v[g] = sum;
	}
}

/* the value x of aggregate ai (an SAA_X_*) for a tuple of group g, added with the aggregate's own arithmetic in tuple order, as the normal plan's transition functions are */
static void
direct_add(AggExec *ex, int ai, uint32 g, uint32 id)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const ComboSt *cs = &ex->cs[a->combo];
	Datum		x = cs->dat[id - 1];
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

	switch (a->kind)
	{
		case SAA_X_MIN:
		case SAA_X_MAX:
			{
				/* numeric_smaller and its kin keep the new value when it ties */
				int32		c = acc->n[g] ? DatumGetInt32(FunctionCall2Coll(ex->xcmp[ai], a->collid, x, acc->v[g])) : 0;

				if (acc->n[g] == 0 || (a->kind == SAA_X_MIN ? c <= 0 : c >= 0))
				{
					if (acc->n[g] && !ex->xbyval[ai])
						pfree(DatumGetPointer(acc->v[g]));
					acc->v[g] = datumCopy(x, ex->xbyval[ai], ex->xlen[ai]);
				}
			}
			break;
		default:
			if (a->restype == NUMERICOID)
				numeric_sum_add(acc, g, x, &cs->dnum[id - 1]);
			else
			{
				double		d = DatumGetFloat8(x);

				acc->d[g] = acc->n[g] ? acc->d[g] + d : d;
				if (!isfinite(d))
					acc->nf[g] = 1;
				else
					ex->amax[ai] = Max(ex->amax[ai], fabs(d));
			}
	}
	acc->n[g]++;
	MemoryContextSwitchTo(old);
}

/*
 * sum and avg of a numeric leaf read as a dictionary (wide): a row adds the integer and scale of its entry, decoded once per dictionary the first time a
 * row reads it, where the combo made a datum of every entry and PostgreSQL's numeric_add added it. The entries that are not a finite number of 38 digits
 * (NaN, infinities) go to numeric_add with the entry itself. A dictionary of records (SiftSrcDict.numrec) has the integer and scale of each row, and the
 * address of the numeric when it has none.
 */
void
wide_sum_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const SiftSrcVec *v = get_vec(ex, a->wsum - 1);
	const SiftSrcDict *d = v->dict;
	const uint32 *idx,
			   *codes = v->data;
	const DNum *recs = d->numrec ? (const DNum *) d->bytes : NULL;
	DictSlot   *ds = recs ? NULL : dict_slot(ex, d);
	MemoryContext old;

	gather_idx(ex, a->wsum - 1, nsel, &idx);
	if (ds && ds->num == NULL)
	{
		ds->num = MemoryContextAlloc(ex->cxt, (size_t) d->n * sizeof(DNum));
		for (uint32 i = 0; i < d->n; i++)
			ds->num[i].scale = WNUM_UNSET;
	}
	old = MemoryContextSwitchTo(ex->tcxt);
	for (int j = 0; j < nsel; j++)
	{
		uint32		c;
		const DNum *e;
		Datum		x = (Datum) 0;
		uint32		g = ex->gid[j];

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		c = codes[idx[j]];
		if (recs)
		{
			e = &recs[c];
			if (unlikely(e->scale == WNUM_UNSET))
				d->rec(d->gctx, c);
			if (!e->ok)
				x = PointerGetDatum((void *) (uintptr_t) e->m);
		}
		else
		{
			DNum	   *ne = &ds->num[c];

			if (unlikely(ne->scale == WNUM_UNSET))
				numeric_decode(PointerGetDatum(d->bytes + d->off[c]), ne);
			if (!ne->ok)
				x = datumCopy(PointerGetDatum(d->bytes + d->off[c]), false, -1);
			e = ne;
		}
		numeric_sum_add(acc, g, x, e);
		acc->n[g]++;
	}
	MemoryContextSwitchTo(old);
}

/* count, min, max, sum and avg of an expression PostgreSQL evaluates (a direct slot of a combo): its value for each tuple the aggregate takes */
void
direct_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	const uint32 *out;

	/* a FILTER keeps the expression off the tuples it takes out */
	if (ok)
	{
		combo_rows(ex, a->combo, ok, ex->calive);
		out = ex->calive;
	}
	else
		out = combo_outs(ex, a->combo);
	for (int j = 0; j < nsel; j++)
	{
		uint32		id;

		if (out[j] == COMBO_NONE || (id = combo_val(ex, a->combo, out[j], a->slot)) == 0)
			continue;
		if (a->kind == SAA_X_COUNT)
			acc->n[ex->gid[j]]++;
		else
			direct_add(ex, ai, ex->gid[j], id);
	}
}
