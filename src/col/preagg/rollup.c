/*
 * col/preagg/rollup.c - the rollups of the lists of a unit: tuples, their encoding and distinct counts
 * the rollup blob of a list and of the rows of the unit
 * emitted after the cube
 */
#include "core/host.h"
#include "core/cmp.h"
#include "core/hash.h"
#include "col/preagg/pagg_build_int.h"

typedef struct RuBuild
{
	int			nd,
				nm;
	const SiftCPaggCol *dc[SIFT_PAGG_MAX_RDIMS],
			   *mc[SIFT_PAGG_MAX_MEAS];
	int16		dleaf[SIFT_PAGG_MAX_RDIMS],
				mleaf[SIFT_PAGG_MAX_MEAS];
	bool		mfl[SIFT_PAGG_MAX_MEAS],
				measlen;
	uint32		n,					/* elements */
				cap,				/* the most tuples */
				nt,
				hsize;
	uint32	   *gid,				/* [n] the tuple of each element */
			   *cnt;				/* [cap] */
	int64	   *tkey;				/* [cap * nd] */
	uint8	   *tnull;				/* [cap] bit j: dim j is NULL */
	int32	   *hslot;				/* [hsize] */
} RuBuild;

/* the order of the tuples: dim 0 first, the NULL of a dim after its values; the sort callback has no argument, the backend is one thread */
typedef struct RuOrder
{
	int			nd;
	const int64 *key;
	const uint8 *null;
} RuOrder;

static const RuOrder *ru_order;

static int
cmp_tuple(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	for (int j = 0; j < ru_order->nd; j++)
	{
		bool		nx = ru_order->null[x] >> j & 1,
					ny = ru_order->null[y] >> j & 1;
		int64		kx = ru_order->key[(Size) x * ru_order->nd + j],
					ky = ru_order->key[(Size) y * ru_order->nd + j];

		if (nx != ny)
			return nx ? 1 : -1;
		if (kx != ky)
			return SIFT_CMP3(kx, ky);
	}
	return SIFT_CMP3(x, y);
}

/* The distinct tuples of the dims over the elements, with the elements of each; false past the cap */
static bool
ru_tuples(RuBuild *b)
{
	uint32		mask = b->hsize - 1;

	memset(b->hslot, 0xFF, b->hsize * sizeof(int32));
	b->nt = 0;
	for (uint32 i = 0; i < b->n; i++)
	{
		int64		k[SIFT_PAGG_MAX_RDIMS];
		uint8		nm = 0;
		uint64		h = 0;
		uint32		pos,
					t;

		for (int j = 0; j < b->nd; j++)
		{
			if (is_null(b->dc[j]->nulls, i))
			{
				nm |= 1 << j;
				k[j] = 0;
			}
			else
				k[j] = b->dc[j]->key[i];
			h = (h ^ (uint64) k[j]) * SIFT_FIB64;
			h ^= h >> 31;
		}
		h = (h ^ nm) * SIFT_FIB64;
		pos = (uint32) (h >> 24) & mask;
		for (;;)
		{
			int32		sl = b->hslot[pos];

			if (sl < 0)
			{
				if (b->nt == b->cap)
					return false;
				t = b->nt++;
				memcpy(b->tkey + (Size) t * b->nd, k, b->nd * sizeof(int64));
				b->tnull[t] = nm;
				b->cnt[t] = 0;
				b->hslot[pos] = t;
				break;
			}
			if (b->tnull[sl] == nm && memcmp(b->tkey + (Size) sl * b->nd, k, b->nd * sizeof(int64)) == 0)
			{
				t = sl;
				break;
			}
			pos = (pos + 1) & mask;
		}
		b->gid[i] = t;
		b->cnt[t]++;
	}
	return true;
}

/* The blob of the tuples found by ru_tuples (format at the top of the file) */
static void
ru_encode(const RuBuild *b, StringInfo out)
{
	uint32		nt = b->nt,
			   *ord = palloc(nt * sizeof(uint32)),
			   *nn = palloc(nt * sizeof(uint32)),
			   *perm = palloc(Max(b->n, 1) * sizeof(uint32)),
			   *gstart = palloc((nt + 1) * sizeof(uint32));
	RuOrder		od = {b->nd, b->tkey, b->tnull};
	SiftCI128  *si = palloc(nt * sizeof(SiftCI128));
	int64	   *mn = palloc(nt * sizeof(int64)),
			   *mx = palloc(nt * sizeof(int64));
	double	   *sf = palloc(nt * sizeof(double)),
			   *fmn = palloc(nt * sizeof(double)),
			   *fmx = palloc(nt * sizeof(double));

	cpagg_group_rows(b->gid, b->n, b->cnt, nt, nn, perm, gstart);
	for (uint32 t = 0; t < nt; t++)
		ord[t] = t;
	ru_order = &od;
	qsort(ord, nt, sizeof(uint32), cmp_tuple);
	appendStringInfoChar(out, (char) b->nd);
	appendStringInfoChar(out, (char) (b->nm | (b->measlen ? PAGG_MEASLEN : 0)));
	sift_col_put_varint(out, nt);
	for (int j = 0; j < b->nd; j++)
		sift_col_put_varint(out, b->dleaf[j]);
	for (int m = 0; m < b->nm; m++)
		sift_col_put_varint(out, b->mleaf[m]);
	for (int j = 0; j < b->nd; j++)
	{
		bool		any = false;
		int64		prev = 0;

		for (uint32 t = 0; t < nt; t++)
			any |= (b->tnull[t] >> j & 1) != 0;
		appendStringInfoChar(out, any);
		if (any)
		{
			uint32		nb = (nt + 7) / 8,
						at = out->len;

			for (uint32 i = 0; i < nb; i++)
				appendStringInfoChar(out, 0);
			for (uint32 i = 0; i < nt; i++)
				if (b->tnull[ord[i]] >> j & 1)
					out->data[at + i / 8] |= (char) (1 << (i % 8));
		}
		for (uint32 i = 0; i < nt; i++)
		{
			int64		k;

			if (b->tnull[ord[i]] >> j & 1)
				continue;
			k = b->tkey[(Size) ord[i] * b->nd + j];
			put_zz(out, (int64) ((uint64) k - (uint64) prev));
			prev = k;
		}
	}
	for (uint32 i = 0; i < nt; i++)
		sift_col_put_varint(out, b->cnt[ord[i]]);
	for (int m = 0; m < b->nm; m++)
	{
		const SiftCPaggCol *mc = b->mc[m];
		StringInfoData ms;

		initStringInfo(&ms);
		memset(nn, 0, nt * sizeof(uint32));
		if (b->mfl[m])
			for (uint32 i = 0; i < b->n; i++)
			{
				uint32		g = b->gid[i];
				double		v;

				if (is_null(mc->nulls, i))
					continue;
				v = sift_col_f8_from_key(mc->key[i]);
				if (nn[g]++ == 0)
					sf[g] = fmn[g] = fmx[g] = v;
				else
				{
					sf[g] += v;
					fmn[g] = v < fmn[g] ? v : fmn[g];
					fmx[g] = v > fmx[g] ? v : fmx[g];
				}
			}
		else if (!cpagg_measure_int(mc, perm, gstart, nt, b->n, si, nn, mn, mx, true))
			memcpy(nn, b->cnt, nt * sizeof(uint32));
		for (uint32 i = 0; i < nt; i++)
			sift_col_put_varint(&ms, nn[ord[i]]);
		for (uint32 i = 0; i < nt; i++)
		{
			uint32		g = ord[i];

			if (b->mfl[m])
			{
				double		v = nn[g] ? sf[g] : 0;

				appendBinaryStringInfo(&ms, (const char *) &v, sizeof(double));
			}
			else
				put_zz128(&ms, nn[g] ? si[g] : 0);
		}
		for (int pass = 0; pass < 2; pass++)
			for (uint32 i = 0; i < nt; i++)
			{
				uint32		g = ord[i];

				if (nn[g] == 0)
					continue;
				if (b->mfl[m])
					appendBinaryStringInfo(&ms, (const char *) (pass ? &fmx[g] : &fmn[g]), sizeof(double));
				else
					put_zz(&ms, pass ? mx[g] : mn[g]);
			}
		if (b->measlen)
			sift_col_put_varint(out, ms.len);
		appendBinaryStringInfo(out, ms.data, ms.len);
		pfree(ms.data);
	}
	pfree(ord);
	pfree(nn);
	pfree(perm);
	pfree(gstart);
	pfree(si);
	pfree(mn);
	pfree(mx);
	pfree(sf);
	pfree(fmn);
	pfree(fmx);
}

/* The distinct values (a NULL one more) of an element column, or limit + 1 when there are more than limit */
static uint32
ru_distinct(const SiftCPaggCol *c, uint32 n, uint32 limit, int64 *hk, uint8 *hu, uint32 hsize)
{
	uint32		mask = hsize - 1,
				nd = 0;
	bool		nullseen = false;

	memset(hu, 0, hsize);
	for (uint32 i = 0; i < n; i++)
	{
		int64		k;
		uint32		h;

		if (is_null(c->nulls, i))
		{
			if (!nullseen && ++nd > limit)
				return limit + 1;
			nullseen = true;
			continue;
		}
		k = c->key[i];
		h = (uint32) (sift_fib64((uint64) k) >> 40) & mask;
		while (hu[h] && hk[h] != k)
			h = (h + 1) & mask;
		if (!hu[h])
		{
			if (++nd > limit)
				return limit + 1;
			hu[h] = 1;
			hk[h] = k;
		}
	}
	return nd;
}

/*
 * Builds the rollup of one list over the unit (of the rows of the unit when ru->list is -1: the dims are top-level leaves and the cap is a 10th of
 * the rows, at most SIFT_PAGG_ROWS_CELLS). fixed: exactly the dims of ru (the cap of tuples is the largest one, an explicit rollup(...) or
 * the check of a stored one); else the dims go from the most distinct values down until the tuples are at most a 32nd of the elements, and
 * the blob fits room (limited: the budget of the list). false when no rollup fits or the list cannot be used.
 */
static bool
build_rollup(const SiftColSchema *s, const SiftCPaggRollup *ru, const SiftCPaggCol *cols, uint32 nrows, bool fixed, bool limited, bool measlen, Size room, StringInfo out)
{
	RuBuild		b;
	int			cand[SIFT_PAGG_MAX_RDIMS],
				nc = 0,
				nmeas = 0;
	uint32		dist[SIFT_PAGG_MAX_RDIMS],
				n = 0,
				hs = 16,
				mhs = 16;
	int64	   *hk;
	uint8	   *hu;
	bool		done = false,
				rows = ru->list < 0;
	StringInfoData tmp;

	if ((!rows && !col_usable(&cols[ru->list], nrows)) || (limited && room == 0))
		return false;
	for (int j = 0; j < ru->ndims; j++)
	{
		const SiftCPaggCol *c = &cols[ru->dim[j]];

		if (!c->ready || cpagg_has_ovf(c) || (nc > 0 && c->n != n) || (rows && c->n != nrows))
			continue;
		n = c->n;
		cand[nc++] = j;
	}
	if (n == 0 || nc == 0)
		return false;
	b.n = n;
	b.measlen = measlen;
	b.cap = rows ? Min((uint32) SIFT_PAGG_ROWS_CELLS, Max((uint32) SIFT_PAGG_ROLLUP_MIN, nrows / SIFT_PAGG_ROWS_RATIO)) : fixed ? SIFT_PAGG_ROLLUP_TUPLES : Min((uint32) SIFT_PAGG_ROLLUP_TUPLES, Max((uint32) SIFT_PAGG_ROLLUP_MIN, n / 32));
	while (mhs < 2 * (b.cap + 2))
		mhs *= 2;
	hk = palloc(mhs * sizeof(int64));
	hu = palloc(mhs);
	for (int i = 0; i < nc; i++)
		dist[i] = ru_distinct(&cols[ru->dim[cand[i]]], n, b.cap, hk, hu, mhs);
	pfree(hk);
	pfree(hu);
	for (int m = 0; m < ru->nmeas && nmeas < SIFT_PAGG_MAX_MEAS; m++)
	{
		const SiftCPaggCol *c = &cols[ru->meas[m]];

		if (c->ready && !cpagg_has_ovf(c) && c->n == n)
		{
			b.mc[nmeas] = c;
			b.mleaf[nmeas] = ru->meas[m];
			b.mfl[nmeas++] = s->cols[ru->meas[m]].store == SIFT_COLS_F4 || s->cols[ru->meas[m]].store == SIFT_COLS_F8;
		}
	}
	b.nm = nmeas;
	/* the order of the attempts: the dims of fewer distinct values first (the position in ru breaks a tie); the stored order is by leaf */
	for (int i = 1; i < nc; i++)
		for (int j = i; j > 0 && dist[j] < dist[j - 1]; j--)
		{
			uint32		d = dist[j];
			int			c = cand[j];

			dist[j] = dist[j - 1];
			cand[j] = cand[j - 1];
			dist[j - 1] = d;
			cand[j - 1] = c;
		}
	{
		uint64		prod = 1;
		int			k = 0;

		if (fixed)
			k = nc;
		else
		{
			while (k < nc && dist[k] <= b.cap && prod * dist[k] <= (uint64) b.cap * 8)
				prod *= dist[k++];
			k = Max(k, 1);
		}
		while (hs < 2 * (b.cap + 1))
			hs *= 2;
		b.hsize = hs;
		b.gid = palloc(n * sizeof(uint32));
		b.cnt = palloc((b.cap + 1) * sizeof(uint32));
		b.tkey = palloc((Size) (b.cap + 1) * SIFT_PAGG_MAX_RDIMS * sizeof(int64));
		b.tnull = palloc(b.cap + 1);
		b.hslot = palloc(hs * sizeof(int32));
		initStringInfo(&tmp);
		for (; k >= 1 && !done; k--)
		{
			int16		leaves[SIFT_PAGG_MAX_RDIMS];

			if (!fixed && dist[k - 1] > b.cap)
				continue;
			for (int i = 0; i < k; i++)
				leaves[i] = ru->dim[cand[i]];
			sift_sort_i16(leaves, k);
			b.nd = k;
			for (int i = 0; i < k; i++)
			{
				b.dleaf[i] = leaves[i];
				b.dc[i] = &cols[leaves[i]];
			}
			if (ru_tuples(&b))
			{
				resetStringInfo(&tmp);
				ru_encode(&b, &tmp);
				if (!limited || tmp.len <= room)
				{
					appendBinaryStringInfo(out, tmp.data, tmp.len);
					done = true;
				}
			}
			if (fixed)
				break;
		}
	}
	pfree(b.gid);
	pfree(b.cnt);
	pfree(b.tkey);
	pfree(b.tnull);
	pfree(b.hslot);
	pfree(tmp.data);
	return done;
}

/* The element blob: the rollups of the spec's lists that a unit can hold, in ascending list order; limited: each within room bytes (a list's own budget, the cube's is not shared) */
void
cpagg_emit_rollups(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, bool limited, Size room, StringInfo out)
{
	StringInfoData body[SIFT_PAGG_MAX_ROLLUPS];
	int			list[SIFT_PAGG_MAX_ROLLUPS],
				nl = 0;

	for (int i = 0; i < spec->nrollups; i++)
	{
		bool		rows = spec->rollup[i].list < 0;

		initStringInfo(&body[nl]);
		if (build_rollup(s, &spec->rollup[i], cols, nrows, spec->rfixed || rows, limited && !rows, spec->measlen, room, &body[nl]))
			list[nl++] = spec->rollup[i].list;
		else
			pfree(body[nl].data);
	}
	if (nl == 0)
		return;
	appendStringInfoChar(out, (char) nl);
	for (int i = 0; i < nl; i++)
	{
		sift_col_put_varint(out, list[i] < 0 ? s->ncols : list[i]);	/* the rows of the unit: the number of leaves, which no list is */
		sift_col_put_varint(out, body[i].len);
	}
	for (int i = 0; i < nl; i++)
	{
		appendBinaryStringInfo(out, body[i].data, body[i].len);
		pfree(body[i].data);
	}
}
