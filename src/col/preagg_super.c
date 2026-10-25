/*
 * col/preagg_super.c - the summary of a group of units (a superunit) from the pre-aggregates of its units: the merged exact sums of an INT
 * leaf and the merged cube (docs/COL-PREAGG.md section 10). Portable (core/host.h only). The writer and sift.col_verify both call these, so
 * the stored bytes are what the units say, byte for byte.
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/preagg_int.h"
#include "core/cmp.h"

bool
sift_cpagg_super_leaf(const SiftCAgg *u, uint32 n, bool sums, SiftCSuperLeaf *out)
{
	SiftCI128	sum = 0,
				sumsq = 0;
	uint64		nvals = 0,
				nnull = 0;
	int64		mn = 0,
				mx = 0;
	bool		any = false,
				cst = true,
				sq_ovf = false;

	memset(out, 0, sizeof(*out));
	for (uint32 i = 0; i < n; i++)
	{
		const SiftCAgg *a = &u[i];

		if ((sums && !a->has_sums) || (a->zflags & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE)))
			return false;
		nvals += a->nvals;
		nnull += a->nnull;
		if (sums)
		{
			sum += a->sum;		/* at most 256 units of 74,496 values of 2^63 */
			if ((a->flags & SIFT_CAGG_SUMSQ_OVF) || __builtin_add_overflow(sumsq, a->sumsq, &sumsq))
				sq_ovf = true;
		}
		if (a->nnull == a->nvals)
			continue;
		if (!any)
		{
			mn = a->min;
			mx = a->max;
			cst = (a->zflags & SIFT_COLZ_CONST) != 0;
			any = true;
		}
		else
		{
			cst = cst && (a->zflags & SIFT_COLZ_CONST) && a->min == mn && a->max == mx;
			mn = Min(mn, a->min);
			mx = Max(mx, a->max);
		}
	}
	if (nvals > UINT32_MAX)
		return false;
	out->min = mn;
	out->max = mx;
	out->nvals = (uint32) nvals;
	out->nnull = (uint32) nnull;
	out->sum_lo = (uint64) sum;
	out->sum_hi = (uint64) (sum >> 64);
	if (!sq_ovf)
	{
		out->sumsq_lo = (uint64) sumsq;
		out->sumsq_hi = (uint64) (sumsq >> 64);
	}
	out->flags = SIFT_SUPER_LEAF_OK | (sq_ovf ? SIFT_CAGG_SUMSQ_OVF : 0);
	out->zflags = any && cst ? SIFT_COLZ_CONST : 0;
	return true;
}

typedef struct SuGroup
{
	int64		key;
	uint32		isnull;
	uint32		unit;
	uint32		g;
} SuGroup;

static int
cmp_sugroup(const void *a, const void *b)
{
	const SuGroup *x = a,
			   *y = b;

	if (x->isnull != y->isnull)
		return x->isnull ? 1 : -1;
	return SIFT_CMP3(x->key, y->key);
}

/* The body of the merged dim d0 of unit 0 in all n units, as build_dim writes a unit's; false when a unit lacks the dim, differs in its measures or the groups are too many */
static bool
merge_dim(const SiftCHist *h, uint32 n, int d0, StringInfo body)
{
	const SiftCHistDim *first = &h[0].dim[d0];
	const SiftCHistDim **dd = palloc(n * sizeof(SiftCHistDim *));
	int			mslot[SIFT_PAGG_MAX_MEAS],
				nm = 0;
	uint64		total = 0;
	SuGroup    *all;
	uint32		ng = 0;
	bool		hasnull = false,
				ok = true;
	uint32	   *cnt;
	uint32	   *nn[SIFT_PAGG_MAX_MEAS];
	SiftCI128  *sum[SIFT_PAGG_MAX_MEAS];
	int64	   *mn[SIFT_PAGG_MAX_MEAS],
			   *mx[SIFT_PAGG_MAX_MEAS];
	int64	   *key;
	int64		prev = 0;

	for (uint32 i = 0; i < n && ok; i++)
	{
		const SiftCHistDim *d = NULL;

		for (int x = 0; x < h[i].ndims; x++)
			if (h[i].dim[x].leaf == first->leaf)
				d = &h[i].dim[x];
		if (d == NULL || d->nmeas != first->nmeas || d->minmax != first->minmax)
			ok = false;
		for (int m = 0; ok && m < first->nmeas; m++)
			if (d->meas[m] != first->meas[m] || (d->sum[m] == NULL) != (first->sum[m] == NULL))
				ok = false;
		dd[i] = d;
		total += ok ? d->ngroups : 0;
	}
	if (!ok || total == 0 || total > (uint64) n * SIFT_PAGG_CUBE_GROUPS)
	{
		pfree(dd);
		return false;
	}
	for (int m = 0; m < first->nmeas; m++)
		if (first->sum[m] != NULL)	/* a float measure adds in an order the units fix: it is left out */
			mslot[nm++] = m;
	all = palloc(total * sizeof(SuGroup));
	for (uint32 i = 0; i < n; i++)
		for (uint32 g = 0; g < dd[i]->ngroups; g++)
			all[ng++] = (SuGroup) {dd[i]->key[g], dd[i]->has_null && g == dd[i]->ngroups - 1, i, g};
	qsort(all, ng, sizeof(SuGroup), cmp_sugroup);
	cnt = palloc0(ng * sizeof(uint32));
	key = palloc(ng * sizeof(int64));
	for (int j = 0; j < nm; j++)
	{
		nn[j] = palloc0(ng * sizeof(uint32));
		sum[j] = palloc0(ng * sizeof(SiftCI128));
		mn[j] = palloc0(ng * sizeof(int64));
		mx[j] = palloc0(ng * sizeof(int64));
	}
	{
		uint32		o = 0;

		for (uint32 i = 0; i < ng && ok; i++)
		{
			const SuGroup *e = &all[i];
			const SiftCHistDim *d = dd[e->unit];
			uint64		c;

			if (i > 0 && e->isnull == all[i - 1].isnull && (e->isnull || e->key == all[i - 1].key))
				o--;			/* the group of the previous element */
			else
			{
				key[o] = e->isnull ? 0 : e->key;
				cnt[o] = 0;
				hasnull |= e->isnull;
				for (int j = 0; j < nm; j++)
					nn[j][o] = 0, sum[j][o] = 0;
			}
			c = (uint64) cnt[o] + d->count[e->g];
			if (c > UINT32_MAX)
				ok = false;
			cnt[o] = (uint32) c;
			for (int j = 0; j < nm && ok; j++)
			{
				int			m = mslot[j];
				uint32		k = d->nn[m][e->g];

				if ((uint64) nn[j][o] + k > UINT32_MAX)
					ok = false;
				sum[j][o] += d->sum[m][e->g];
				if (k > 0 && d->min[m] != NULL)
				{
					mn[j][o] = nn[j][o] == 0 ? d->min[m][e->g] : Min(mn[j][o], d->min[m][e->g]);
					mx[j][o] = nn[j][o] == 0 ? d->max[m][e->g] : Max(mx[j][o], d->max[m][e->g]);
				}
				nn[j][o] += k;
			}
			o++;
		}
		ng = o;
	}
	if (ok && ng > SIFT_PAGG_CUBE_GROUPS)
		ok = false;
	if (ok)
	{
		appendStringInfoChar(body, (char) nm);
		sift_col_put_varint(body, ng);
		appendStringInfoChar(body, (hasnull ? PAGG_NULLGROUP : 0) | (first->minmax ? PAGG_MINMAX : 0));
		for (int j = 0; j < nm; j++)
			sift_col_put_varint(body, first->meas[mslot[j]]);
		for (uint32 i = 0; i < ng - (hasnull ? 1 : 0); i++)
		{
			if (i == 0)
				put_zz(body, key[i]);
			else
				sift_col_put_varint(body, (uint64) key[i] - (uint64) prev);
			prev = key[i];
		}
		for (uint32 i = 0; i < ng; i++)
			sift_col_put_varint(body, cnt[i]);
		for (int j = 0; j < nm; j++)
		{
			for (uint32 i = 0; i < ng; i++)
				sift_col_put_varint(body, nn[j][i]);
			for (uint32 i = 0; i < ng; i++)
				put_zz128(body, sum[j][i]);
			for (int pass = 0; first->minmax && pass < 2; pass++)
				for (uint32 i = 0; i < ng; i++)
					if (nn[j][i] > 0)
						put_zz(body, pass ? mx[j][i] : mn[j][i]);
		}
	}
	for (int j = 0; j < nm; j++)
	{
		pfree(nn[j]);
		pfree(sum[j]);
		pfree(mn[j]);
		pfree(mx[j]);
	}
	pfree(key);
	pfree(cnt);
	pfree(all);
	pfree(dd);
	return ok;
}

bool
sift_cpagg_super_cube(const SiftColSchema *s, const uint8 *const *blob, const uint32 *len, uint32 n, StringInfo out)
{
	SiftCHist  *h = palloc0(n * sizeof(SiftCHist));
	StringInfoData body[SIFT_PAGG_MAX_DIMS];
	int			leaf[SIFT_PAGG_MAX_DIMS],
				kept = 0;
	bool		ok = true;

	for (uint32 i = 0; i < n && ok; i++)
	{
		if (len[i] == 0)
			goto done;			/* a unit with no cube: the group has none */
		ok = sift_cpagg_decode(s, blob[i], len[i], CurrentMemoryContext, &h[i]);
	}
	if (!ok)
		goto done;
	for (int d = 0; d < h[0].ndims; d++)
	{
		/* the calendar cells of a unit are not merged: a group of 16 units holds up to 16 times their cells for a query the units answer as fast (+1.2% of the index) */
		if (h[0].dim[d].leaf >= SIFT_PAGG_CAL)
			continue;
		initStringInfo(&body[kept]);
		if (merge_dim(h, n, d, &body[kept]))
			leaf[kept++] = h[0].dim[d].leaf;
		else
			pfree(body[kept].data);
	}
	if (kept > 0)
	{
		appendStringInfoChar(out, (char) kept);
		for (int d = 0; d < kept; d++)
		{
			sift_col_put_varint(out, leaf[d]);
			sift_col_put_varint(out, body[d].len);
		}
		for (int d = 0; d < kept; d++)
		{
			appendBinaryStringInfo(out, body[d].data, body[d].len);
			pfree(body[d].data);
		}
	}
done:
	pfree(h);
	return ok;
}
