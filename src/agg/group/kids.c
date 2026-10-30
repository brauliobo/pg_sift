/*
 * agg/group/kids.c - the key ids of the tuples of a chunk
 * The NULL ids, the range of a key's values, and the ids of each key of the selected
 * tuples (compute_kids).
 */
#include "postgres.h"

#include "col/kern.h"
#include "agg/group/int.h"

/* the tuples of key ki whose value is NULL, as a bitmap in ex->knul (one pass without branches); false: none */
static bool
key_nulls(AggExec *ex, int ki, const uint8 *isnull, const uint32 *idx, int nsel)
{
	uint64	   *nul = ex->knul + (Size) ki * (VECW + PADW);
	uint64		any = 0;

	for (int w = 0; w * 64 < nsel; w++)
	{
		const int	lim = Min(64, nsel - w * 64);
		uint64		b = 0;

		for (int j = 0; j < lim; j++)
			b |= (uint64) (isnull[idx[w * 64 + j]] != 0) << j;
		nul[w] = b;
		any |= b;
	}
	return any != 0;
}

/* vals[i] = x for the tuples key_nulls found */
static void
key_nulls_set(AggExec *ex, int ki, int nsel, uint32 *vals, uint32 x)
{
	const uint64 *nul = ex->knul + (Size) ki * (VECW + PADW);

	for (int w = 0; w * 64 < nsel; w++)
		for (uint64 b = nul[w]; b; b &= b - 1)
			vals[w * 64 + pg_rightmost_one_pos64(b)] = x;
}

/*
 * The key of the selected rows of a timestamp key: the range table of bucket numbers (map and base, as an integer key's), or the
 * ids themselves (*map_out stays NULL) when an infinity or a wide range is among the rows.
 */
static void
bucket_ids(AggExec *ex, int ki, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 **map_out, int32 *base_out, uint32 *vals_out)
{
	KeyState   *k = &ex->ks[ki];
	KeyFill		f = {ex, ki};
	MemoryContext old;
	int32		mn,
				mx;
	bool		table;

	mn = mx = 0;
	if (k->zcut)
		table = false;
	else if (k->zone)
		table = bucket_numbers_zoned(k->zone, k->bucket, v, idx, nsel, vals_out, &mn, &mx);
	else if (k->bucket == 1)	/* each value is a step of its own (init.c): the hash table of values */
		table = false;
	else
		table = bucket_numbers_utc(k->bucket, v, idx, nsel, vals_out, &mn, &mx);
	if (!table || rt_span(&k->rt, mn, mx) > MAX_RANGE)
	{
		bucket_hash_ids(ex, k, v, idx, nsel, vals_out);
		return;
	}
	old = MemoryContextSwitchTo(ex->tcxt);
	rt_cover(&k->rt, mn, mx, key_range_fill, &f);
	MemoryContextSwitchTo(old);
	if (v->isnull && key_nulls(ex, ki, v->isnull, idx, nsel))
		key_nulls_set(ex, ki, nsel, vals_out, (uint32) k->rt.lo);
	*map_out = (uint32 *) k->rt.tab;
	*base_out = k->rt.lo;
}

/*
 * The ids of the integer values in [mn, mx] of key ki, of nsel rows, are made; false: the key keeps its ids in a hash table instead of the
 * range table (ident_wide_id), which is too wide (a lookup key's too: a date column holding both infinities spans 2^32 values) or would be filled
 * with many more values than the rows have (the ids of a column clustered by another, whose units hold few values far apart).
 */
bool
key_cover(AggExec *ex, int ki, bool any, int32 mn, int32 mx, int nsel)
{
	KeyState   *k = &ex->ks[ki];
	KeyFill		f = {ex, ki};
	MemoryContext old;
	int64		span = rt_span(&k->rt, mn, mx),
				added = span - (k->rt.lo <= k->rt.hi ? (int64) k->rt.hi - k->rt.lo + 1 : 0);

	if (k->wcap || (any && (span > MAX_RANGE || (k->ident && span > SPARSE_RANGE && added > (int64) SPARSE_FACTOR * nsel))))
	{
		if (!k->wcap)
			ident_go_wide(ex, k);
		return false;
	}
	old = MemoryContextSwitchTo(ex->tcxt);
	/* only NULLs (!any): any covered value will do for the kernel, the rows are corrected afterwards */
	rt_cover(&k->rt, any ? mn : 0, any ? mx : 0, key_range_fill, &f);
	MemoryContextSwitchTo(old);
	return true;
}

/*
 * Adds the ids of the selected tuples' key j to gid[i] (times mult); the data of the key's leaf is gathered through idx.
 * Ids that do not exist yet are made first, so key_used() is final when this returns.
 */
void
key_prepare(AggExec *ex, int ki, int nsel, const uint32 *idx, uint32 **map_out, int32 *base_out, uint32 *vals_out)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcVec *v;
	const SiftSrcLeaf *lf;

	*map_out = NULL;			/* the values are the ids */
	*base_out = 0;
	if (!(ex->arm_mask >> ki & 1))
	{
		memset(vals_out, 0, nsel * sizeof(uint32));	/* NULL in this arm, or in this grouping set */
		return;
	}
	if (sk->kind == SAK_SET)
	{
		for (int i = 0; i < nsel; i++)
			vals_out[i] = ex->curset + 1;
		return;
	}
	if (sk->kind == SAK_COMBO)
	{
		const uint32 *out = combo_outs(ex, sk->leaf);
		const uint32 *val = ex->cs[sk->leaf].val;
		const int	ns = ex->spec->combo[sk->leaf].nslots;

		/* an evaluation may reallocate the table */
		for (int i = 0; i < nsel; i++)
		{
			uint32		r = val[(size_t) out[i] * ns + sk->slot];

			if (r == COMBO_UNEVAL)
			{
				r = combo_eval_slot(ex, sk->leaf, out[i], sk->slot);
				val = ex->cs[sk->leaf].val;
			}
			vals_out[i] = r;
		}
		return;
	}
	if (sk->kind == SAK_NUM)
	{
		const sift_i128 *x = eval_nx(ex, &sk->num, nsel, ex->tidx, ex->ridx, ex->nb, NULL);

		for (int i = 0; i < nsel; i++)
			vals_out[i] = ex->nb[i] ? 0 : idmap_id(&k->im, x[i]);
		return;
	}
	v = get_vec(ex, sk->leaf);
	lf = &ex->lay->leaf[sk->leaf];
	if (k->bucket)
	{
		bucket_ids(ex, ki, v, idx, nsel, map_out, base_out, vals_out);
		return;
	}
	if (lf->vtype == SCV_F64)
	{
		for (int i = 0; i < nsel; i++)
		{
			double		d = ((const double *) v->data)[idx[i]];
			uint32		n = k->nident;

			if (v->isnull && v->isnull[idx[i]])
				vals_out[i] = 0;
			else if ((vals_out[i] = ident_wide_id(ex, k, float_key(d))) > n && d == 0 && signbit(d))
				k->negzero = vals_out[i];
		}
		return;
	}
	sift_colk->gather_u32((const uint32 *) v->data, idx, nsel, vals_out);
	if (lf->vtype == SCV_CODE)
	{
		DictSlot   *s = dict_slot(ex, v->dict);
		uint32	   *map = dict_kmap(ex, s, v->dict, ki);

		if (v->isnull && key_nulls(ex, ki, v->isnull, idx, nsel))
			key_nulls_set(ex, ki, nsel, vals_out, v->dict->n);
		kmap_fill(ex, map, v->dict, ki, vals_out, nsel);
		*map_out = map;
		*base_out = 0;
	}
	else
	{
		int32		mn = 0,
					mx = 0;
		bool		nulls = v->isnull && key_nulls(ex, ki, v->isnull, idx, nsel),
					any;

		/* the range of the non-null values */
		if (nulls)
		{
			fill_ones(ex->tmpm, nsel);
			sift_colk->bits_andnot(ex->tmpm, ex->knul + (Size) ki * (VECW + PADW), (nsel + 63) / 64);
		}
		any = sift_aggk->minmax_i32(&mn, &mx, (const int32 *) vals_out, nulls ? ex->tmpm : NULL, nsel);

		if (!key_cover(ex, ki, any, mn, mx, nsel))
		{
			int32		last = 0;
			uint32		lastid = 0;

			/* the rows of one value follow each other (the elements of one row's array, the rows of one parent): the id of the row before is theirs */
			for (int i = 0; i < nsel; i++)
			{
				int32		x = (int32) vals_out[i];

				if (v->isnull && v->isnull[idx[i]])
				{
					vals_out[i] = 0;
					continue;
				}
				if (lastid == 0 || x != last)
				{
					if (i + GROUP_AHEAD < nsel)
					{
						uint32		h = wide_hash((int32) vals_out[i + GROUP_AHEAD], k->wcap);

						__builtin_prefetch(&k->wid[h]);
						__builtin_prefetch(&k->wval[h]);
					}
					lastid = ident_wide_id(ex, k, x);
					last = x;
				}
				vals_out[i] = lastid;
			}
			*map_out = NULL;		/* the values are the ids */
			*base_out = 0;
			return;
		}
		if (nulls)
			key_nulls_set(ex, ki, nsel, vals_out, (uint32) k->rt.lo);
		*map_out = (uint32 *) k->rt.tab;
		*base_out = k->rt.lo;
	}
}

/* the id of the key for a NULL integer: 0, or the id of expr(NULL) of a LUT key */
uint32
key_null_id(AggExec *ex, int ki)
{
	KeyState   *k = &ex->ks[ki];

	if (ex->spec->key[ki].kind != SAK_LUT)
		return 0;
	if (k->nullid == UINT32_MAX)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

		k->nullid = key_value_id(ex, ki, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	return k->nullid;
}

/* ids of the key for the tuples whose value was NULL are 0 (or the id of expr(NULL)): fixes gid after the kernels */
void
key_null_fix(AggExec *ex, int ki, int nsel, const uint32 *vals, uint32 *gid, uint32 mult, uint32 *ids)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcVec *v = get_vec(ex, sk->leaf);
	const uint64 *nul = ex->knul + (Size) ki * (VECW + PADW);
	uint32		nullid;

	if (v->isnull == NULL || ex->lay->leaf[sk->leaf].vtype == SCV_CODE)
		return;
	nullid = key_null_id(ex, ki);
	for (int w = 0; w * 64 < nsel; w++)
		for (uint64 b = nul[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			uint32		cur = ((uint32 *) k->rt.tab)[(int32) vals[i] - k->rt.lo];

			if (gid)
				gid[i] += (nullid - cur) * mult;
			else
				ids[i] = nullid;
		}
}

/* The ids of the tuples' keys, kid[j * (VEC + PADW) + i] for key j and tuple i (0: NULL, or a key the tuples do not have). */
void
compute_kids(AggExec *ex, int nsel, uint32 *kid)
{
	uint32	   *maps[SIFT_AGG_MAX_KEYS];
	int32		bases[SIFT_AGG_MAX_KEYS];

	for (int j = 0; j < ex->spec->nkeys; j++)
	{
		const uint32 *idx = NULL;
		const SiftSrcLeaf *lf = key_lf(ex, j);
		uint32	   *vals = ex->kids + (Size) j * (VEC + PADW),
				   *out = kid + (Size) j * (VEC + PADW);

		if (lf)
			gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
		key_prepare(ex, j, nsel, idx, &maps[j], &bases[j], vals);
		if (maps[j] == NULL)
		{
			memcpy(out, vals, nsel * sizeof(uint32));
			continue;
		}
		memset(out, 0, nsel * sizeof(uint32));
		if (lf->vtype == SCV_CODE)
			sift_aggk->gid_code(out, vals, maps[j], nsel, 1);
		else
		{
			sift_aggk->gid_i32(out, (const int32 *) vals, bases[j], maps[j], nsel, 1);
			key_null_fix(ex, j, nsel, vals, NULL, 1, out);
		}
	}
}
