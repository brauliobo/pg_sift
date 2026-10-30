/*
 * agg/blob/merge.c - partial group tables: the merge of a dump into the table
 * The keys, values and aggregates of a dump read back and merged into the groups of the
 * reading table: blob_merge.
 */
#include "postgres.h"

#include "utils/datum.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"

#include "agg/blob/int.h"

/* ids of the values of an integer key in the table (the ids of the range table, or of the wide one) */
static void
merge_ident(AggExec *g, int j, const int64 *vals, uint32 nv, uint32 *xid)
{
	KeyState   *k = &g->ks[j];
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN;

	for (uint32 i = 0; i < nv; i++)
	{
		mn = Min(mn, vals[i]);
		mx = Max(mx, vals[i]);
	}
	if (nv == 0)
		return;
	if (k->wcap || mn < PG_INT32_MIN || mx > PG_INT32_MAX || rt_span(&k->rt, (int32) mn, (int32) mx) > MAX_RANGE)
	{
		if (!k->wcap)
			ident_go_wide(g, k);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ident_wide_id(g, k, vals[i]);
	}
	else
	{
		KeyFill		f = {g, j};
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		rt_cover(&k->rt, (int32) mn, (int32) mx, key_range_fill, &f);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ((uint32 *) k->rt.tab)[(int32) vals[i] - k->rt.lo];
	}
}

/* xid[1..] = the ids in table t of the values a blob lists; the reader is left after them */
static void
merge_values(AggExec *g, OutTable *t, const char **p, uint32 *xid)
{
	const uint32 *hd = br_get(p, 8);
	const char *at = *p;

	for (uint32 i = 0; i < hd[0]; i++)
	{
		bool		isnull;
		Datum		d = datumRestore((char **) &at, &isnull);
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		xid[i + 1] = ot_id(t, d);
		MemoryContextSwitchTo(old);
	}
	*p += TYPEALIGN(8, hd[1]);
}

/* the ids in the table of key j of the values a blob lists (blob_key_table): xid[0] is NULL's, the reader is left after the values */
static uint32 *
merge_key_table(AggExec *g, int j, const char **p)
{
	KeyState   *k = &g->ks[j];
	const uint32 *hd = (const uint32 *) *p;
	uint32	   *xid = palloc0((hd[0] + 1) * sizeof(uint32));

	if (k->ident)
	{
		const int64 *iv;

		br_get(p, 8);
		iv = br_get(p, (Size) hd[0] * sizeof(int64));
		if (g->spec->key[j].kind == SAK_SET)
			for (uint32 i = 0; i < hd[0]; i++)
				xid[i + 1] = iv[i] + 1;	/* the ids of the sets are fixed: set + 1 */
		else
			merge_ident(g, j, iv, hd[0], xid);
	}
	else if (k->numk)
	{
		const sift_i128 *nv;

		br_get(p, 8);
		nv = br_get(p, (Size) hd[0] * sizeof(sift_i128));
		for (uint32 i = 0; i < hd[0]; i++)
			xid[i + 1] = idmap_id(&k->im, nv[i]);
	}
	else
		merge_values(g, &k->ot, p, xid);
	return xid;
}

static void
merge_minmax(AggExec *g, int ai, const BlobAgg *ba, uint32 r, uint32 t)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];
	bool		ismin = a->kind == SAA_MIN,
				f64 = a->num.n == 0 && g->lay->leaf[a->leaf].vtype == SCV_F64;

	if (ba->a.n[r] == 0)
		return;
	if (a->num.n)
	{
		if (acc->n[t] == 0 || (ismin ? ba->a.w[r] < acc->w[t] : ba->a.w[r] > acc->w[t]))
			acc->w[t] = ba->a.w[r];
		acc->n[t] += ba->a.n[r];
		return;
	}
	if (acc->n[t] == 0)
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	else if (f64 ? (ismin ? float8_cmp_internal(ba->a.d[r], acc->d[t]) < 0 : float8_cmp_internal(ba->a.d[r], acc->d[t]) > 0) :
			 (ismin ? ba->a.i[r] < acc->i[t] : ba->a.i[r] > acc->i[t]))
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	acc->n[t] += ba->a.n[r];
}

static void
merge_agg(AggExec *g, int ai, BlobAgg *ba, const uint32 *gids, uint32 r0, uint32 n)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];

	for (uint32 r = r0; r < r0 + n; r++)
	{
		uint32		t = gids[r];

		switch (a->kind)
		{
			case SAA_COUNT_STAR:
			case SAA_COUNT:
			case SAA_COUNT_ROWS:
			case SAA_O_COUNT_STAR:
			case SAA_O_COUNT_ROWS:
			case SAA_M_COUNT:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_O_SUM_I64:
				acc->n[t] += ba->a.n[r];
				acc->w[t] += ba->a.w[r];
				break;
			case SAA_O_SUM_F64:
			case SAA_O_AVG_F64:
				acc->n[t] += ba->a.n[r];
				acc->d[t] += ba->a.d[r];
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_SUM_INT:
			case SAA_AVG_INT:
				acc->n[t] += ba->a.n[r];
				acc->i[t] += ba->a.i[r];
				break;
			case SAA_X_SUM:
			case SAA_X_AVG:
				if (a->restype == NUMERICOID)
				{
					DNum		m = {.m = ba->a.w[r], .scale = (int16) ba->a.i[r], .ok = !ba->a.nf[r]};
					MemoryContext old = MemoryContextSwitchTo(g->tcxt);

					if (ba->a.n[r] > 0)
						numeric_sum_add(acc, t, m.ok ? (Datum) 0 : PointerGetDatum(ba->nv[r]), &m);
					MemoryContextSwitchTo(old);
					acc->n[t] += ba->a.n[r];
					break;
				}
				sift_fallthrough;
			case SAA_SUM_F64:
			case SAA_AVG_F64:
				acc->n[t] += ba->a.n[r];
				acc->d[t] += ba->a.d[r];
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_X_COUNT:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_MIN:
			case SAA_MAX:
				merge_minmax(g, ai, ba, r, t);
				break;
			case SAA_SUM_NUM:
			case SAA_AVG_NUM:
				if (a->distinct)
					break;
				acc->n[t] += ba->a.n[r];
				agg_sum_merge(acc, t, ba->a.w[r], ba->a.i[r]);
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_COUNT_DISTINCT:
			case SAA_O_COUNT_KEY:
			case SAA_M_DISTINCT:
			case SAA_GROUPING:
				break;
			case SAA_PCT_CONT:
			case SAA_PCT_DISC:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_GEN:
				gen_state_merge(g, g->gst[ai], acc, t, &ba->gcur);
				break;
			default:
				SIFT_INVARIANT(false, "sift: aggregate kind %d has no partial table", a->kind);
		}
	}
}

/* the arrays of a blob's aggregate (the layout of bw_acc), with the pairs of its set */
static void
merge_read_agg(AggExec *g, int ai, const char **p, uint32 ng, BlobAgg *ba)
{
	const AggAcc *acc = &g->acc[ai];

	ba->a.n = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->i)
		ba->a.i = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->d)
		ba->a.d = (double *) br_get(p, (Size) ng * 8);
	if (acc->nf)
		ba->a.nf = (uint8 *) br_get(p, ng);
	if (acc->w)
		ba->a.w = (sift_i128 *) br_get(p, (Size) ng * sizeof(sift_i128));
	if (numeric_sum_agg(&g->spec->agg[ai]))
	{
		const uint32 *hd = br_get(p, 8);

		ba->nv = palloc0(Max(ng, 1) * sizeof(*ba->nv));
		for (uint32 i = 0; i < hd[0]; i++)
		{
			const uint32 *rec = br_get(p, 8);

			ba->nv[rec[0]] = br_get(p, rec[1]);
		}
	}
	if (set_agg(&g->spec->agg[ai]))
	{
		const SiftAggAgg *a = &g->spec->agg[ai];
		const uint32 *hd = br_get(p, 16);

		ba->npairs = hd[0];
		ba->nchunks = hd[1];
		ba->nwg = hd[2];
		ba->nk = hd[3];
		ba->pg = br_get(p, (Size) hd[0] * 4);
		ba->pv = br_get(p, (Size) hd[0] * 4);
		ba->cg = br_get(p, (Size) hd[1] * 4);
		ba->cc = br_get(p, (Size) hd[1] * 4);
		ba->bits = br_get(p, (Size) hd[1] * DB_WORDS * sizeof(uint64));
		if (ba->nwg)
		{
			ba->wbase = *(const int64 *) br_get(p, sizeof(int64));
			ba->wg = br_get(p, (Size) ba->nwg * 4);
			ba->wrows = br_get(p, (Size) ba->nwg * ba->nk * sizeof(uint64));
		}
		if (a->leaf >= 0 && g->lay->leaf[a->leaf].vtype == SCV_CODE)
		{
			const uint32 *vh = (const uint32 *) *p;

			ba->xv = palloc0((vh[0] + 1) * sizeof(uint32));
			merge_values(g, &g->aot[ai], p, ba->xv);
		}
		else if (a->num.n)
		{
			const uint32 *vh = br_get(p, 8);
			const sift_i128 *nv = br_get(p, (Size) vh[0] * sizeof(sift_i128));

			ba->xv = palloc0((vh[0] + 1) * sizeof(uint32));
			for (uint32 i = 0; i < vh[0]; i++)
				ba->xv[i + 1] = idmap_id(&g->amap[ai], nv[i]);
		}
	}
	if (g->spec->agg[ai].kind == SAA_M_DISTINCT)
	{
		ba->npairs = ((const uint32 *) br_get(p, 8))[0];
		ba->pg = br_get(p, (Size) ba->npairs * 4);
		ba->xv = palloc0((((const uint32 *) *p)[0] + 1) * sizeof(uint32));
		merge_values(g, mset_table(g, ai), p, ba->xv);
		ba->pv = br_get(p, (Size) ba->npairs * 4);
	}
	if (g->spec->agg[ai].kind == SAA_O_COUNT_KEY)
	{
		ba->npairs = ((const uint32 *) br_get(p, 8))[0];
		ba->pg = br_get(p, (Size) ba->npairs * 4);
		ba->xv = merge_key_table(g, g->spec->agg[ai].slot, p);
		ba->pv = br_get(p, (Size) ba->npairs * 4);
	}
	if (is_ordered_agg(&g->spec->agg[ai]) && g->spec->agg[ai].share == ai)
	{
		ba->nrec = *(const uint32 *) br_get(p, 8);
		ba->rec = br_get(p, (Size) ba->nrec * sizeof(OsaRec));
	}
	if (g->spec->agg[ai].kind == SAA_GEN)
	{
		const uint32 *hd = br_get(p, 8);

		ba->gcur = *p;
		*p += TYPEALIGN(8, hd[1]);
	}
}

/*
 * The rows of words of a narrow set added to the sets of the leader. A dictionary leaf's ids are numbered by the table that wrote them, and
 * ba->xv has the leader's id of each value the words hold, in order of id: where that is the id itself the words are ORed as they are,
 * else each bit goes through the map.
 */
static void
merge_words(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	Dist	   *d = g->dist[ai];
	uint32	   *lid = NULL;

	if (ba->xv)
	{
		uint32		i = 0;
		bool		same = true;

		lid = palloc0((Size) ba->nk * 64 * sizeof(uint32));
		for (uint32 w = 0; w < ba->nk; w++)
		{
			uint64		seen = 0;

			for (uint32 k = 0; k < ba->nwg; k++)
				seen |= ba->wrows[(Size) k * ba->nk + w];
			for (; seen; seen &= seen - 1)
			{
				uint32		bit = w * 64 + pg_rightmost_one_pos64(seen);

				lid[bit] = ba->xv[++i];
				same &= (int64) lid[bit] == ba->wbase * 64 + bit;
			}
		}
		if (same)
		{
			pfree(lid);
			lid = NULL;
		}
	}
	for (uint32 k = 0; k < ba->nwg; k++)
	{
		uint32		t = gids[ba->wg[k]];
		const uint64 *row = ba->wrows + (Size) k * ba->nk;

		if (lid == NULL)
			g->acc[ai].n[t] += dist_or_words(g, d, t, ba->wbase, (const uint8 *) row, ba->nk);
		else
			for (uint32 w = 0; w < ba->nk; w++)
				for (uint64 m = row[w]; m; m &= m - 1)
					if (dist_add(g, d, t, lid[w * 64 + pg_rightmost_one_pos64(m)]))
						g->acc[ai].n[t]++;
	}
	if (lid)
		pfree(lid);
}

static void
merge_set(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	if (g->spec->agg[ai].kind == SAA_SUM_NUM || g->spec->agg[ai].kind == SAA_AVG_NUM)
	{
		/* a sum over distinct numbers: a value the leader has not summed in the group is added */
		AggAcc	   *acc = &g->acc[ai];

		for (uint32 i = 0; i < ba->npairs; i++)
		{
			uint32		t = gids[ba->pg[i]],
						id = ba->xv[ba->pv[i]];
			sift_i128	v = g->amap[ai].val[id - 1];

			if (i + DSET_AHEAD < ba->npairs)
				dset_prefetch(&g->dset[ai], gids[ba->pg[i + DSET_AHEAD]], ba->xv[ba->pv[i + DSET_AHEAD]]);
			if (!dset_add(g, &g->dset[ai], t, id))
				continue;
			if (v == SIFT_NX_NAN)
				acc->nf[t] = 1;
			else
				agg_sum_add(acc, t, v);
			acc->n[t]++;
		}
		return;
	}
	if (g->spec->agg[ai].kind == SAA_M_DISTINCT)
	{
		/* a value the leader has not counted in the group is counted */
		for (uint32 i = 0; i < ba->npairs; i++)
		{
			uint32		t = gids[ba->pg[i]];

			if (i + DSET_AHEAD < ba->npairs)
				dset_prefetch(&g->dset[ai], gids[ba->pg[i + DSET_AHEAD]], ba->xv[ba->pv[i + DSET_AHEAD]]);
			if (dset_add(g, &g->dset[ai], t, ba->xv[ba->pv[i]]))
				g->acc[ai].n[t]++;
		}
		return;
	}
	if (ba->nwg)
		merge_words(g, ai, ba, gids);
	for (uint32 i = 0; i < ba->nchunks; i++)
	{
		uint32		t = gids[ba->cg[i]];

		g->acc[ai].n[t] += dist_or_chunk(g, g->dist[ai], t, (int32) ba->cc[i], ba->bits + (Size) i * DB_WORDS);
		g->n_dist_chunks++;
	}
	for (uint32 i = 0; i < ba->npairs; i++)
	{
		uint32		t = gids[ba->pg[i]];

		if (dist_add(g, g->dist[ai], t, ba->xv ? ba->xv[ba->pv[i]] : ba->pv[i]))
			g->acc[ai].n[t]++;
	}
}

static void
merge_osa(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	Osa		   *o = g->osa[ai];
	AggAcc	   *own = &g->acc[ai];

	for (uint32 i = 0; i < ba->nrec; i++)
	{
		uint32		t = gids[ba->rec[i].g];

		if (own->i[t] == 0)
			own->i[t] = ++o->nsid;
		osa_add_n(g, o, own->i[t] - 1, ba->rec[i].key, ba->rec[i].cnt);
	}
}

void
blob_merge(AggExec *g, const char *p)
{
	SiftAggSpec *s = g->spec;
	const BlobHdr *h = br_get(&p, sizeof(BlobHdr));
	uint32		ng = h->ngroups,
			   *xid[SIFT_AGG_MAX_KEYS],
			   *gids = palloc(Max(ng, 1) * sizeof(uint32));
	const uint32 *lid[SIFT_AGG_MAX_KEYS];
	const int64 *seen;
	BlobAgg    *ba = palloc0(Max(s->naggs, 1) * sizeof(BlobAgg));
	const double **lvec = palloc0(Max(s->naggs, 1) * SIFT_AGG_LANES * sizeof(double *));
	bool		grow = false;

	SIFT_INVARIANT(h->magic == BLOB_MAGIC, "sift: bad partial aggregate");
	for (int j = 0; j < s->nkeys; j++)
	{
		xid[j] = merge_key_table(g, j, &p);
		lid[j] = br_get(&p, (Size) ng * sizeof(uint32));
	}
	seen = br_get(&p, (Size) ng * sizeof(int64));
	for (int i = 0; i < s->naggs; i++)
		merge_read_agg(g, i, &p, ng, &ba[i]);
	{
		const double *am = br_get(&p, Max(s->naggs, 1) * sizeof(double));

		for (int i = 0; i < s->naggs; i++)
			g->amax[i] = Max(g->amax[i], am[i]);
	}
	for (int k = 0; k < s->narms; k++)
	{
		const double *am = br_get(&p, Max(s->arm[k].ninner, 1) * sizeof(double));

		for (int i = 0; i < s->arm[k].ninner; i++)
			g->arm[k].amax[i] = Max(g->arm[k].amax[i], am[i]);
	}
	for (int i = 0; i < s->naggs; i++)
		if (g->acc[i].lv)
		{
			uint64		mask = *(const uint64 *) br_get(&p, sizeof(uint64));

			for (int l = 0; l < SIFT_AGG_LANES; l++)
				if (mask >> l & 1)
					lvec[i * SIFT_AGG_LANES + l] = br_get(&p, (Size) ng * sizeof(double));
		}
	if (!g->hash)
		for (int j = 0; j < s->nkeys; j++)
			grow |= key_used(&g->ks[j]) > g->ks[j].cap;
	if (grow)
		direct_grow(g);
	for (uint32 r0 = 0; r0 < ng; r0 += VEC)
	{
		uint32		n = Min(VEC, ng - r0);

		for (uint32 r = r0; r < r0 + n; r++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			for (int j = 0; j < s->nkeys; j++)
				ids[j] = xid[j][lid[j][r]];
			if (g->hash)
				gids[r] = hash_group(g, ids);
			else
			{
				gids[r] = 0;
				for (int j = 0; j < s->nkeys; j++)
					gids[r] += ids[j] * g->ks[j].mult;
			}
		}
		if (g->ngroups > g->gcap)
			groups_resize(g, Max(g->gcap * 2, g->ngroups), NULL, g->gcap);
		for (uint32 r = r0; r < r0 + n; r++)
			g->seen[gids[r]] += seen[r];
		for (int i = 0; i < s->naggs; i++)
			merge_agg(g, i, &ba[i], gids, r0, n);
	}
	for (int i = 0; i < s->naggs; i++)
		if (set_agg(&s->agg[i]) || s->agg[i].kind == SAA_O_COUNT_KEY || s->agg[i].kind == SAA_M_DISTINCT)
			merge_set(g, i, &ba[i], gids);
		else if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			merge_osa(g, i, &ba[i], gids);
	/* the lanes a process scanned are its own: their sums are put in place, the leader adds them in lane order (lanes_sum) */
	for (int i = 0; i < s->naggs; i++)
		for (int l = 0; g->acc[i].lv && l < SIFT_AGG_LANES; l++)
		{
			const double *v = lvec[i * SIFT_AGG_LANES + l];

			if (v == NULL)
				continue;
			if (g->acc[i].lv[l] == NULL)
				g->acc[i].lv[l] = MemoryContextAllocZero(g->tcxt, g->gcap * sizeof(double));
			for (uint32 r = 0; r < ng; r++)
				g->acc[i].lv[l][gids[r]] = v[r];
		}
	pfree(gids);
	pfree(ba);
	pfree(lvec);
}
