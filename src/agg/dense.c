/*
 * agg/dense.c - the dense chunks of the aggregate scan: counts, sums and averages taken from the bitmap of a chunk, with no list of tuples (agg/update.c).
 */
#include "postgres.h"

#include <float.h>

#include "utils/float.h"

#include "col/kern.h"
#include "agg/dense_int.h"

/*
 * A chunk whose aggregates are counts, sums and averages of one leaf each, grouped by dictionary leaves or not at all, is aggregated
 * from its bitmap: no list of tuples and no gather by position. An aggregate takes the bits that pass its FILTER and have a value,
 * a count is their popcount (or a per group count of them), a sum runs over them in row order, as the list of tuples does, so a
 * float sum has the same bits. The groups of the rows come from the codes of the chunk.
 */
static bool
dense_agg_ok(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			break;
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			if (a->leaf < 0 || a->nvx || a->num.n)
				return false;
			break;
		default:
			return false;
	}
	return !a->distinct && (a->filter == NULL || !a->filter->late);
}

void
dense_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	bool		ok = sift_agg_dense && s->narms == 0 && s->nsets == 0 && s->npost == 0 && ex->nj == 0 && ex->topn == NULL;

	for (int j = 0; ok && j < s->nkeys; j++)
		ok = s->key[j].kind == SAK_LEAF && key_lf(ex, j)->vtype == SCV_CODE;
	for (int i = 0; ok && i < s->naggs; i++)
		ok = dense_agg_ok(&s->agg[i]);
	ex->dense = ok;
	if (ok)
		ex->zgid = MemoryContextAllocZero(ex->cxt, (VEC + PADW) * sizeof(uint32));
}

/* the bits of [start, start + n) of the chunk whose leaf is NULL; false when the leaf has no NULL */
static bool
chunk_nulls(AggExec *ex, int leaf, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, leaf);

	if (v->isnull == NULL)
		return false;
	if (leaf_lvl(ex, leaf) == ex->qlevel)
		sift_colk_nonzero(v->isnull + start, n, out);
	else
	{
		int			nrow = ex->unit.nrow;
		uint64	   *rn = MemoryContextAlloc(ex->ucxt, (SIFT_COL_NWORDS(nrow) + PADW) * sizeof(uint64));

		sift_colk_nonzero(v->isnull, nrow, rn);
		sift_aggk->gather_bits(out, rn, unit_parent(ex) + start, n);
	}
	return true;
}

/* the values of the leaf for the chunk, 4 bytes each (wide: 8): in place at the chunk's level, else gathered through the parents into tmp */
static const void *
chunk_vals(AggExec *ex, int leaf, int start, int n, bool wide, void *tmp)
{
	const SiftSrcVec *v = get_vec(ex, leaf);

	if (leaf_lvl(ex, leaf) == ex->qlevel)
		return (const char *) v->data + (size_t) start * (wide ? 8 : 4);
	if (wide)
		sift_colk->for_i64_sel[SIFT_COLW_64](v->data, unit_parent(ex) + start, n, 0, tmp);
	else
		sift_colk->gather_u32(v->data, unit_parent(ex) + start, n, tmp);
	return tmp;
}

/* the group of every row of the chunk in ex->gid; false when the keys outgrew the direct table (the chunk goes the way of tuples) */
static bool
dense_gids(AggExec *ex, int start, int n)
{
	SiftAggSpec *s = ex->spec;
	const uint32 *maps[SIFT_AGG_MAX_KEYS],
			   *codes[SIFT_AGG_MAX_KEYS];
	bool		grow = false;

	for (int j = 0; j < s->nkeys; j++)
	{
		const SiftSrcVec *v = get_vec(ex, s->key[j].leaf);
		bool		same = leaf_lvl(ex, s->key[j].leaf) == ex->qlevel;
		uint32	   *own = ex->kids + (Size) j * (VEC + PADW);

		codes[j] = chunk_vals(ex, s->key[j].leaf, start, n, false, own);
		if (v->isnull)
		{
			const uint32 *par = same ? NULL : unit_parent(ex);

			if (codes[j] != own)
				memcpy(own, codes[j], n * sizeof(uint32));
			for (int i = 0; i < n; i++)
				if (v->isnull[same ? start + i : par[start + i]])
					own[i] = v->dict->n;
			codes[j] = own;
		}
		maps[j] = dict_kmap(ex, dict_slot(ex, v->dict), v->dict, j);
		grow |= key_used(&ex->ks[j]) > ex->ks[j].cap;
	}
	if (grow)
	{
		direct_grow(ex);
		if (ex->hash)
			return false;
	}
	memset(ex->gid, 0, n * sizeof(uint32));
	for (int j = 0; j < s->nkeys; j++)
		sift_aggk->gid_code(ex->gid, codes[j], maps[j], n, ex->ks[j].mult);
	return true;
}

/* the bits of the chunk that aggregate a takes: its FILTER and a value; mask itself when that is all of them */
static const uint64 *
dense_taken(AggExec *ex, SiftAggAgg *a, int start, int n, const uint64 *mask, uint64 *ok)
{
	const uint64 *res = mask;
	int			nw = (n + 63) / 64;
	uint64		nul[VECW + PADW];

	if (a->filter)
	{
		eval_pred(ex, a->filter, ex->qlevel, start, n, ok);
		sift_colk->bits_and(ok, mask, nw);
		res = ok;
	}
	if (a->leaf >= 0 && chunk_nulls(ex, a->leaf, start, n, nul))
	{
		if (res != ok)
			memcpy(ok, mask, nw * sizeof(uint64));
		sift_colk->bits_andnot(ok, nul, nw);
		res = ok;
	}
	return res;
}

/* the rows per group of the bits of `bits`, in h (gcap entries) */
static void
dense_hist(AggExec *ex, int64 *h, const uint64 *bits, int n)
{
	memset(h, 0, ex->gcap * sizeof(int64));
	sift_aggk->count_gid(h, ex->gid, bits, n);
}

/*
 * cnt[g] += the rows of ok in group g. With few groups the rows of the chunk were counted per group once (ex->hist): an aggregate that
 * takes them all adds that, and one that leaves out fewer rows than it takes counts the rows left out and subtracts them.
 */
static void
dense_count(AggExec *ex, int64 *cnt, const uint64 *ok, const uint64 *mask, int n)
{
	int			nw = (n + 63) / 64;

	if (ex->spec->nkeys == 0)
		cnt[0] += sift_colk->bits_count(ok, nw);
	else if (ex->hist == NULL && ok == mask)
		ex->ncnt[ex->nncnt++] = cnt;		/* counted with seen at the end of the chunk */
	else if (ex->hist == NULL)
		sift_aggk->count_gid(cnt, ex->gid, ok, n);
	else if (ok == mask)
		for (uint32 g = 0; g < ex->gcap; g++)
			cnt[g] += ex->hist[g];
	else
	{
		uint64		out[VECW + PADW];

		memcpy(out, mask, nw * sizeof(uint64));
		sift_colk->bits_andnot(out, ok, nw);
		if (sift_colk->bits_count(out, nw) < sift_colk->bits_count(ok, nw))
		{
			dense_hist(ex, ex->hist2, out, n);
			for (uint32 g = 0; g < ex->gcap; g++)
				cnt[g] += ex->hist[g] - ex->hist2[g];
		}
		else
			sift_aggk->count_gid(cnt, ex->gid, ok, n);
	}
}

/* float_guard for the bits of ok: a non-finite input marks its group, else the largest absolute input is noted */
static void
dense_float_guard(AggExec *ex, int ai, const double *x, const uint64 *ok, int n, const uint32 *gid)
{
	double		m;

	if (!sift_aggk->absmax_f64(x, ok, n, &m))
	{
		for (int w = 0; w < (n + 63) / 64; w++)
			for (uint64 b = ok[w]; b; b &= b - 1)
			{
				int			j = w * 64 + pg_rightmost_one_pos64(b);

				if (!isfinite(x[j]))
					ex->acc[ai].nf[gid[j]] = 1;
			}
		return;
	}
	ex->amax[ai] = Max(ex->amax[ai], m);
	if (ex->spec->agg[ai].kind == SAA_AVG_F64 && 8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
		float_overflow_error();
}

static void
dense_update(AggExec *ex, int ai, int start, int n, const uint64 *mask)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint64		okb[VECW + PADW];
	const uint64 *ok = dense_taken(ex, a, start, n, mask, okb);
	const uint32 *gid = ex->spec->nkeys ? ex->gid : ex->zgid;
	bool		ungrouped = ex->spec->nkeys == 0;
	const void *x;

	switch (a->kind)
	{
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			x = chunk_vals(ex, a->leaf, start, n, false, ex->cu32);
			if (ungrouped)
				acc->i[0] += sift_aggk->sum_i32(x, ok, n);
			else
				sift_aggk->sum_gid_i32(acc->i, gid, x, ok, n);
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			x = chunk_vals(ex, a->leaf, start, n, true, ex->c64);
			dense_float_guard(ex, ai, x, ok, n, gid);
			sift_aggk->sum_gid_f64(acc->d, gid, x, ok, n);
			break;
		default:
			break;
	}
	dense_count(ex, acc->n, ok, mask, n);
}

/* the chunk [start, start + n) of the unit's rows or elements selected by mask, aggregated from the bitmap; false: not a dense chunk */
bool
dense_chunk(AggExec *ex, int start, int n, const uint64 *mask)
{
	int			nw = (n + 63) / 64;
	uint64		cnt = sift_colk->bits_count(mask, nw);

	if (cnt == 0)
		return true;
	if (ex->spec->nkeys && !dense_gids(ex, start, n))
		return false;
	ex->cstart = start;
	ex->cn = n;
	memset(ex->cfree_ok, 0, sizeof(ex->cfree_ok));
	ex->n_tuples += cnt;
	ex->n_dense++;
	ex->hist = NULL;
	ex->nncnt = 0;
	if (ex->spec->nkeys == 0)
		ex->seen[0] += cnt;
	else if (ex->gcap <= n / 8)
	{
		if (ex->histcap < ex->gcap)
		{
			if (ex->histbuf)
				pfree(ex->histbuf);
			ex->histcap = Max(ex->gcap, 64);
			ex->histbuf = MemoryContextAlloc(ex->cxt, (Size) ex->histcap * 2 * sizeof(int64));
		}
		ex->hist = ex->histbuf;
		ex->hist2 = ex->histbuf + ex->histcap;
		dense_hist(ex, ex->hist, mask, n);
		for (uint32 g = 0; g < ex->gcap; g++)
			ex->seen[g] += ex->hist[g];
	}
	else
		ex->ncnt[ex->nncnt++] = ex->seen;
	for (int i = 0; i < ex->spec->naggs; i++)
		dense_update(ex, i, start, n, mask);
	if (ex->nncnt > 0)
		sift_aggk_count_gid_n(ex->ncnt, ex->nncnt, ex->gid, mask, n);
	return true;
}
