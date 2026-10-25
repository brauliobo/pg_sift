/*
 * agg/update.c - the update of the aggregates of a chunk: selection, tuple lists, joins, the dense chunk
 */
#include "postgres.h"

#include <float.h>

#include "utils/datum.h"
#include "utils/float.h"

#include "col/kern.h"
#include "agg/dense_int.h"

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

/*
 * An aggregate of an expression over a column of a joined table: the combo gives the id (0: NULL) of the value of each tuple, in the
 * table of the distinct values of the aggregate. count(x) counts the ids; the others add the (group, id) pairs to a set (with the
 * times each was added, for sum and avg) that mapped_finish turns into the result with PostgreSQL's own comparison and arithmetic.
 */
static void
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
static void
numeric_sum_add(AggAcc *acc, uint32 g, Datum x, const DNum *m)
{
	const sift_i128 *p10 = pow10_tab();
	sift_i128	w = acc->w[g],
				v = m->m;
	int			sc = (int) acc->i[g];

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
	if (!acc->nf[g])
	{
		/* leaves the integer form */
		acc->nf[g] = 1;
		if (acc->n[g] > 0)
			acc->v[g] = scaled_numeric_exact(acc->w[g], (int) acc->i[g]);
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

/* count, min, max, sum and avg of an expression PostgreSQL evaluates (a direct slot of a combo): its value for each tuple the aggregate takes */
static void
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

/* true when the number is new in the group: distinct numbers have one id, a (group, id) pair is counted once */
static bool
num_first(AggExec *ex, int ai, uint32 g, sift_i128 v)
{
	return dset_add(ex, &ex->dset[ai], g, idmap_id(&ex->amap[ai], v));
}

/* What the update of one aggregate over a chunk works on: its selected rows (ok: those its FILTER passes, NULL for all) and column */
typedef struct AggUpd
{
	AggExec    *ex;
	int			ai;
	SiftAggAgg *a;
	AggAcc	   *acc;
	const uint64 *ok;
	int			nsel;
	const uint32 *idx;			/* the column's rows of the selection */
	const SiftSrcVec *v;
	const SiftSrcLeaf *lf;
} AggUpd;

/* n of the aggregate += its tuples of the chunk; those that take every tuple wait for the end of the chunk, when one pass counts them all with seen */
static void
upd_count(const AggUpd *u)
{
	if (u->ok == NULL)
		u->ex->ncnt[u->ex->nncnt++] = u->acc->n;
	else
		sift_aggk->count_gid(u->acc->n, u->ex->gid, u->ok, u->nsel);
}

static void
upd_sum_int(const AggUpd *u)
{
	AggExec    *ex = u->ex;

	sift_colk->gather_u32((const uint32 *) u->v->data, u->idx, u->nsel, ex->cu32);
	sift_aggk->sum_gid_i32(u->acc->i, ex->gid, (const int32 *) ex->cu32, u->ok, u->nsel);
	upd_count(u);
}

static void
upd_sum_f64(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	const double *x;
	int			nsel = u->nsel;

	if (u->a->nvx)
		x = eval_vx(ex, u->a, nsel);
	else
	{
		sift_colk->for_i64_sel[SIFT_COLW_64](u->v->data, u->idx, nsel, 0, (int64 *) ex->c64);
		x = (const double *) ex->c64;
	}
	float_guard(ex, u->ai, x, nsel);
	sift_aggk->sum_gid_f64(u->acc->d, ex->gid, x, u->ok, nsel);
	upd_count(u);
}

/* min and max of a decimal expression */
static void
upd_minmax_num(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	const uint64 *ok = u->ok;
	int			nsel = u->nsel;
	AggAcc	   *acc = u->acc;
	bool		ismin = u->a->kind == SAA_MIN;
	const sift_i128 *nx = eval_nx(ex, &u->a->num, nsel, ex->tidx, ex->ridx, NULL, ok);

	for (int j = 0; j < nsel; j++)
	{
		uint32		g = ex->gid[j];

		if ((ok && !sift_colsrc_bit(ok, j)))
			continue;
		if (acc->n[g] == 0 || (ismin ? nx[j] < acc->w[g] : nx[j] > acc->w[g]))
			acc->w[g] = nx[j];
		acc->n[g]++;
	}
}

/* min and max of a column of integers, dates, timestamps or floats */
static void
upd_minmax(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	AggAcc	   *acc = u->acc;
	const SiftSrcLeaf *lf = u->lf;
	bool		ismin = u->a->kind == SAA_MIN;
	const uint64 *ok = u->ok;
	int			nsel = u->nsel;

	if (u->a->num.n)
	{
		upd_minmax_num(u);
		return;
	}
	if (lf->vtype == SCV_I32)
		sift_colk->gather_u32((const uint32 *) u->v->data, u->idx, nsel, ex->cu32);
	else
		sift_colk->for_i64_sel[SIFT_COLW_64](u->v->data, u->idx, nsel, 0, (int64 *) ex->c64);
	for (int j = 0; j < nsel; j++)
	{
		uint32		g = ex->gid[j];

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if (lf->vtype == SCV_F64)
		{
			double		x = ((const double *) ex->c64)[j];

			if (acc->n[g] == 0 || (ismin ? float8_cmp_internal(x, acc->d[g]) < 0 : float8_cmp_internal(x, acc->d[g]) > 0))
				acc->d[g] = x;
		}
		else
		{
			int64		x = lf->vtype == SCV_I32 ? ((const int32 *) ex->cu32)[j] : ((const int64 *) ex->c64)[j];

			if (acc->n[g] == 0 || (ismin ? x < acc->i[g] : x > acc->i[g]))
				acc->i[g] = x;
		}
		acc->n[g]++;
	}
}

static void
upd_count_rows(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	AggAcc	   *acc = u->acc;
	const uint64 *ok = u->ok;

	/* the row of the preserved side that no row matched holds no key: the key is NULL, and DISTINCT does not count it */
	if (ex->null_input)
		return;
	for (int j = 0; j < u->nsel; j++)
	{
		uint32		g = ex->gid[j];
		uint64		row = ex->row_base + ex->ridx[j];

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if (acc->last[g] != row)
		{
			acc->last[g] = row;
			acc->n[g]++;
		}
	}
}

static void
upd_sum_num(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	const uint64 *ok = u->ok;
	int			nsel = u->nsel;
	AggAcc	   *acc = u->acc;
	const sift_i128 *nx = eval_nx(ex, &u->a->num, nsel, ex->tidx, ex->ridx, NULL, ok);

	for (int j = 0; j < nsel; j++)
		if ((!ok || sift_colsrc_bit(ok, j)) && (!u->a->distinct || num_first(ex, u->ai, ex->gid[j], nx[j])))
		{
			if (nx[j] == SIFT_NX_NAN)
				acc->nf[ex->gid[j]] = 1;
			else
				agg_sum_add(acc, ex->gid[j], nx[j]);
			acc->n[ex->gid[j]]++;
		}
}

static void
upd_percentile(const AggUpd *u)
{
	if (u->a->share == u->ai)
		osa_update(u->ex, u->ai, u->ok, u->nsel);
	upd_count(u);
}

static void
upd_count_distinct(const AggUpd *u)
{
	AggExec    *ex = u->ex;
	const SiftSrcVec *v = u->v;
	const uint64 *ok = u->ok;
	int			nsel = u->nsel;
	const uint32 *idx = u->idx;

	if (u->a->num.n)
	{
		const sift_i128 *nx = eval_nx(ex, &u->a->num, nsel, ex->tidx, ex->ridx, NULL, ok);

		for (int j = 0; j < nsel; j++)
			if (!ok || sift_colsrc_bit(ok, j))
				ex->cu32[j] = idmap_id(&ex->amap[u->ai], nx[j]);
		dist_update(ex, u->ai, ok, nsel, ex->cu32);
		return;
	}
	sift_colk->gather_u32((const uint32 *) v->data, idx, nsel, ex->cu32);
	if (u->lf->vtype == SCV_CODE)
	{
		uint32	   *map = dict_amap(ex, dict_slot(ex, v->dict), v->dict, u->ai);

		for (int j = 0; j < nsel; j++)
			ex->cu32[j] = amap_id(ex, map, v->dict, u->ai, v->isnull && v->isnull[idx[j]] ? v->dict->n : ex->cu32[j]);
	}
	dist_update(ex, u->ai, ok, nsel, ex->cu32);
}

/* The aggregate ai over the nsel rows of the chunk at start: one function per kind of aggregate */
static void
agg_update(AggExec *ex, int ai, int start, int n, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggUpd		u = {.ex = ex, .ai = ai, .a = a, .acc = &ex->acc[ai], .nsel = nsel};
	bool		all;

	agg_selection(ex, a, start, n, nsel, &all);
	u.ok = all ? NULL : ex->okm;
	if (a->leaf >= 0)
	{
		u.v = get_vec(ex, a->leaf);
		u.lf = &ex->lay->leaf[a->leaf];
		gather_idx(ex, a->leaf, nsel, &u.idx);
	}
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
			upd_count(&u);
			break;
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			upd_sum_int(&u);
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			upd_sum_f64(&u);
			break;
		case SAA_MIN:
		case SAA_MAX:
			upd_minmax(&u);
			break;
		case SAA_COUNT_ROWS:
			upd_count_rows(&u);
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			upd_sum_num(&u);
			break;
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			upd_percentile(&u);
			break;
		case SAA_COUNT_DISTINCT:
			upd_count_distinct(&u);
			break;
		case SAA_GROUPING:
			break;
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_M_SUM:
		case SAA_M_AVG:
		case SAA_M_PG:
			mapped_update(ex, ai, u.ok, nsel);
			break;
		case SAA_GEN:
			gen_update(ex, ai, u.ok, nsel);
			break;
		case SAA_X_COUNT:
		case SAA_X_MIN:
		case SAA_X_MAX:
		case SAA_X_SUM:
		case SAA_X_AVG:
			direct_update(ex, ai, u.ok, nsel);
			break;
		default:
			SIFT_INVARIANT(false, "sift: aggregate kind %d is not a scan aggregate", a->kind);
	}
}

/* does conjunct i of the joined rows apply to set `set` (-1: the conjuncts of every set) */
static bool
post_applies(const SiftAggSpec *s, int i, int set)
{
	uint32		sets = s->postsets ? s->postsets[i] : 0;

	return set < 0 ? sets == 0 : (sets >> set & 1) != 0;
}

/* the tuple passes conjunct p over the joined rows: a boolean slot of its combo, or a row of the join */
static bool
post_holds(AggExec *ex, const SiftAggPred *p, const Tup *t, int j)
{
	const ComboSt *cs = &ex->cs[p->leaf];

	if (p->kind == SAP_ELEM_EXISTS)
		return elem_exists(ex, t, j, p);
	if (p->kind == SAP_MATCHED)
		return cs->maprow[t->cout[p->leaf][j]] != UINT32_MAX;
	/* a combo of the scanned table's columns only (a WHERE after a preserved join) has no join: its rows are resolved for the tuples */
	if (ex->spec->combo[p->leaf].join < 0)
		return combo_val(ex, p->leaf, combo_outs(ex, p->leaf)[j], p->op) != 0;
	return cs->val[(size_t) t->cout[p->leaf][j] * ex->spec->combo[p->leaf].nslots + p->op] != 0;
}

static void
tup_move(AggExec *ex, Tup *t, int to, int from, int stages)
{
	t->sel[to] = t->sel[from];
	t->tidx[to] = t->tidx[from];
	t->ridx[to] = t->ridx[from];
	for (int x = 0; x < stages; x++)
		t->cout[ex->jcombo[x]][to] = t->cout[ex->jcombo[x]][from];
}

/* the conjuncts over the joined rows [first, end) that apply to the set: the tuples that pass, moved to the front; the new count */
static int
post_filter(AggExec *ex, Tup *t, int first, int end, int stages, int set)
{
	int			k = 0;

	for (int j = 0; j < t->n; j++)
	{
		bool		keep = true;

		for (int i = first; keep && i < end; i++)
			keep = !post_applies(ex->spec, i, set) || post_holds(ex, ex->spec->post[i], t, j);
		if (!keep)
			continue;
		tup_move(ex, t, k++, j, stages);
	}
	return k;
}

/* the conjuncts of the set's arm that the pass does not apply (the rows of its chunk, as an aggregate's FILTER): the tuples that pass, moved to the front; the new count */
static int
set_filter(AggExec *ex, Tup *t, const SiftAggPred *p)
{
	uint64		pass[VECW + PADW];
	int			k = 0;

	eval_pred(ex, (SiftAggPred *) p, ex->qlevel, ex->cstart, ex->cn, pass);
	for (int j = 0; j < t->n; j++)
		if (sift_colsrc_bit(pass, t->sel[j]))
			tup_move(ex, t, k++, j, ex->nj);
	return k;
}

/* the tuples of a set that has conjuncts of its own: a copy of the tuples (the other sets read them whole) that passes them */
static Tup *
set_tuples(AggExec *ex, const Tup *t, int set)
{
	Tup		   *own = ex->set_tup;

	if (own == NULL)
	{
		own = ex->set_tup = MemoryContextAllocZero(ex->cxt, sizeof(Tup));
		own->sel = MemoryContextAlloc(ex->cxt, (VEC + PADW + 16) * sizeof(uint32));
		own->tidx = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
		own->ridx = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
		own->cout = MemoryContextAllocZero(ex->cxt, ex->spec->ncombo * sizeof(uint32 *));
		for (int x = 0; x < ex->nj; x++)
			own->cout[ex->jcombo[x]] = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
	}
	own->n = t->n;
	memcpy(own->sel, t->sel, t->n * sizeof(uint32));
	memcpy(own->tidx, t->tidx, t->n * sizeof(uint32));
	memcpy(own->ridx, t->ridx, t->n * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
		memcpy(own->cout[ex->jcombo[x]], t->cout[ex->jcombo[x]], t->n * sizeof(uint32));
	own->n = post_filter(ex, own, ex->spec->nprepost, ex->spec->npost, ex->nj, set);
	if (own->n > 0 && ex->spec->setpred && ex->spec->setpred[set])
		own->n = set_filter(ex, own, ex->spec->setpred[set]);
	return own;
}

static bool
set_has_post(const SiftAggSpec *s, int set)
{
	if (s->setpred && s->setpred[set])
		return true;
	for (int i = s->nprepost; i < s->npost; i++)
		if (post_applies(s, i, set))
			return true;
	return false;
}

/* the tuples that passed the joins and the filters over the joined rows: their groups and aggregates */
void
agg_tuples(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;
	bool		shared;

	ex->cur = t;
	ex->sel = t->sel;
	ex->tidx = t->tidx;
	ex->ridx = t->ridx;
	memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
	if (s->npost > s->nprepost)
	{
		t->n = post_filter(ex, t, s->nprepost, s->npost, ex->nj, -1);
		/* the free combos were resolved for the tuples as they came */
		memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
	}
	if (t->n == 0)
		return;
	if (s->narms > 0)
	{
		arm_chunk(ex, t);
		return;
	}
	ex->n_tuples += t->n;
	shared = false;
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		Tup		   *st = s->nsets && set_has_post(s, set) ? set_tuples(ex, t, set) : t;

		if (st->n == 0)
			continue;
		/* the free combos are resolved for the tuples they were asked for: the set's own are not the chunk's */
		if (st != t || ex->cur != t)
			memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
		ex->cur = st;
		ex->sel = st->sel;
		ex->tidx = st->tidx;
		ex->ridx = st->ridx;
		ex->curset = set;
		ex->arm_mask = s->nsets ? s->setmask[set] : ~(uint32) 0;
		if (s->nsets > 1 && ex->hash && st == t)
		{
			if (!shared)
			{
				ex->arm_mask = ~(uint32) 0;
				compute_kids(ex, t->n, ex->setkid);
				ex->arm_mask = s->setmask[set];
				shared = true;
			}
			compute_set_gids(ex, st->n, ex->setkid);
		}
		else
			compute_gids(ex, st->n);
		ex->ncnt[0] = ex->seen;
		ex->nncnt = 1;
		for (int i = 0; i < s->naggs; i++)
			if (!s->setaggs || (s->setaggs[i] >> set & 1))
				agg_update(ex, i, ex->cstart, ex->cn, st->n);
		sift_aggk_count_gid_n(ex->ncnt, ex->nncnt, ex->gid, NULL, st->n);
	}
	ex->arm_mask = ~(uint32) 0;
}

/* the stage's tuples: the outputs of the earlier joins are copied only by a stage that repeats or drops tuples, so a chain of one-row lookups holds one array per stage */
Tup *
stage_tuples(AggExec *ex, int s)
{
	Tup		   *own = &ex->stage[s];

	if (s > 0 && own->cout[ex->jcombo[0]] == NULL)
		for (int y = 0; y < s; y++)
			own->cout[ex->jcombo[y]] = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
	return own;
}

static void
semi_stage(AggExec *ex, int s, Tup *in)
{
	int			ci = ex->jcombo[s];
	ComboSt    *cs = &ex->cs[ci];
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const bool	want = ex->spec->join[cb->join].kind != SJ_ANTI;
	const bool	pass = cs->spass;
	const SiftSrcVec *v = pass ? NULL : get_vec(ex, cb->leaf[0]);
	const uint32 *idx = leaf_lvl(ex, cb->leaf[0]) == ex->qlevel ? in->tidx : in->ridx;
	const int32 *x = pass ? NULL : (const int32 *) v->data;
	Tup		   *own = stage_tuples(ex, s);
	int			k = 0;

	for (int i = 0; i < in->n; i++)
	{
		bool		hit = pass || (!(v->isnull && v->isnull[idx[i]]) && (uint64) ((int64) x[idx[i]] - cs->slo) < cs->snlut && cs->slut[(int64) x[idx[i]] - cs->slo]);

		if (hit != want)
			continue;
		own->sel[k] = in->sel[i];
		own->tidx[k] = in->tidx[i];
		own->ridx[k] = in->ridx[i];
		for (int y = 0; y < s; y++)
			own->cout[ex->jcombo[y]][k] = in->cout[ex->jcombo[y]][i];
		own->cout[ci][k] = 0;
		k++;
	}
	own->n = k;
	if (k > 0)
		join_stage(ex, s + 1, own);
}

/* applies the lookup joins one at a time: a tuple is repeated per match of its key, and dropped (INNER) or kept once (LEFT) without one */
void
join_stage(AggExec *ex, int s, Tup *in)
{
	int			ci,
				n = in->n;
	ComboSt    *cs;
	Tup		   *own;
	uint32	   *eid;
	bool		plain = true;

	if (s == ex->nj)
	{
		agg_tuples(ex, in);
		return;
	}
	ci = ex->jcombo[s];
	cs = &ex->cs[ci];
	if (cs->map->keep && ex->spec->nprepost > 0)
	{
		in->n = n = post_filter(ex, in, 0, ex->spec->nprepost, s, -1);
		if (n == 0)
			return;
	}
	if (cs->slut || cs->spass)
	{
		semi_stage(ex, s, in);
		return;
	}
	own = &ex->stage[s];
	eid = ex->peid;
	combo_resolve(ex, ci, ex->qlevel, n, in->tidx, in->ridx, eid, NULL, in);
	for (int i = 0; i < n; i++)
		plain &= cs->ent[eid[i]].nout == 1;
	if (plain)
	{
		Tup			al = *in;

		for (int i = 0; i < n; i++)
			own->cout[ci][i] = cs->ent[eid[i]].base;
		al.cout = memcpy(ex->alcout[s], in->cout, ex->spec->ncombo * sizeof(uint32 *));
		al.cout[ci] = own->cout[ci];
		join_stage(ex, s + 1, &al);
		return;
	}
	/* the entries of this stage's tuples stay in eid only until the next stage resolves its own: keep a copy */
	own = stage_tuples(ex, s);
	eid = memcpy(ex->seid[s], eid, n * sizeof(uint32));
	for (int j = 0, e = 0; j < n;)
	{
		int			k = 0;

		for (; j < n && k < VEC; k++)
		{
			const ComboEnt *en = &cs->ent[eid[j]];

			if (e >= en->nout)
			{
				j++;
				e = 0;
				k--;
				continue;
			}
			own->sel[k] = in->sel[j];
			own->tidx[k] = in->tidx[j];
			own->ridx[k] = in->ridx[j];
			for (int x = 0; x < s; x++)
				own->cout[ex->jcombo[x]][k] = in->cout[ex->jcombo[x]][j];
			own->cout[ci][k] = en->base + e;
			if (++e >= en->nout)
			{
				j++;
				e = 0;
			}
		}
		own->n = k;
		if (k > 0)
			join_stage(ex, s + 1, own);
	}
}

/* one chunk of tuples [start, start + n) selected by mask */
void
agg_chunk(AggExec *ex, int start, int n, const uint64 *mask)
{
	Tup		   *t = &ex->t0;
	int			nw = (n + 63) / 64;

	combo_trim(ex);
	if (ex->dense && !ex->hash && ex->rsel == NULL && !ex->nullpass && dense_chunk(ex, start, n, mask))
		return;
	t->n = sift_colk->bits_to_idx(mask, nw, 0, t->sel);
	if (t->n == 0)
		return;
	ex->cstart = start;
	ex->cn = n;
	for (int j = 0; j < t->n; j++)
		t->tidx[j] = start + t->sel[j];
	if (ex->spec->group >= 0 && !ex->nullpass)
	{
		const uint32 *par = unit_parent(ex);

		for (int j = 0; j < t->n; j++)
			t->ridx[j] = par[t->tidx[j]];
	}
	else
		memcpy(t->ridx, t->tidx, t->n * sizeof(uint32));
	join_stage(ex, 0, t);
}
