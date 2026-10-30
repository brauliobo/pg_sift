/*
 * agg/update/upd.c - the update of each kind of aggregate
 * The count, the sum, the min and max, the percentile and the count(DISTINCT) of a chunk,
 * and the dispatch of agg_update.
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/dense_int.h"
#include "agg/vnum.h"
#include "col/am.h"
#include "agg/update/int.h"

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

/* true when the number is new in the group: distinct numbers have one id, a (group, id) pair is counted once */
static bool
num_first(AggExec *ex, int ai, uint32 g, sift_i128 v)
{
	return dset_add(ex, &ex->dset[ai], g, idmap_id(&ex->amap[ai], v));
}

/* n of the aggregate += its tuples of the chunk; those that take every tuple wait for the end of the chunk, when one pass counts them all with seen */
static void
upd_count(const AggUpd *u)
{
	if (u->ok == NULL)
		u->ex->ncnt[u->ex->nncnt++] = u->acc->n;
	else if (u->ex->spec->nkeys == 0)
		u->acc->n[0] += sift_colk->bits_count(u->ok, (u->nsel + 63) / 64);
	else
		sift_aggk->count_gid(u->acc->n, u->ex->gid, u->ok, u->nsel);
}

static void
upd_sum_int(const AggUpd *u)
{
	AggExec    *ex = u->ex;

	sift_colk->gather_u32((const uint32 *) u->v->data, u->idx, u->nsel, ex->cu32);
	if (ex->spec->nkeys == 0)
		u->acc->i[0] += sift_aggk->sum_i32((const int32 *) ex->cu32, u->ok, u->nsel);
	else
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
	const sift_i128 *nx,
			   *ny;
	bool		product = u->a->num.dig > SIFT_NUM_MAX_DIGITS;

	if (!product && !u->a->distinct && vnum_program_ok(&u->a->num))
	{
		vnum_sum_update(ex, u->ai, &u->a->num, nsel, ok);
		upd_count(u);
		return;
	}
	nx = eval_nx(ex, &u->a->num, nsel, ex->tidx, ex->ridx, NULL, ok);
	ny = nx + (VEC + PADW);
	for (int j = 0; j < nsel; j++)
		if ((!ok || sift_colsrc_bit(ok, j)) && (!u->a->distinct || num_first(ex, u->ai, ex->gid[j], nx[j])))
		{
			if (nx[j] == SIFT_NX_NAN)
				acc->nf[ex->gid[j]] = 1;
			else if (product)
				agg_sum_add_product(acc, ex->gid[j], nx[j], ny[j]);
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
void
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
			if (a->wsum > 0)
				wide_sum_update(ex, ai, u.ok, nsel);
			else
				direct_update(ex, ai, u.ok, nsel);
			break;
		default:
			SIFT_INVARIANT(false, "sift: aggregate kind %d is not a scan aggregate", a->kind);
	}
}
