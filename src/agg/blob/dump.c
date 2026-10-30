/*
 * agg/blob/dump.c - partial group tables: the dump of a worker's or lane's table
 * The writers of the sums, lanes and values, the keys of the groups, the hash of a group, and
 * the dump of the whole table or of its partitions.
 */
#include "postgres.h"

#include "utils/datum.h"

#include "agg/blob/int.h"

/* a sum or average of numerics added as 38-digit integers until one does not fit (numeric_sum_add) */
bool
numeric_sum_agg(const SiftAggAgg *a)
{
	return (a->kind == SAA_X_SUM || a->kind == SAA_X_AVG) && a->restype == NUMERICOID;
}

/* the aggregate arrays a state holds per group, besides the count: what acc_alloc made */
static void
bw_acc(StringInfo b, const AggAcc *acc, const uint32 *gl, uint32 ng)
{
	char	   *tmp = palloc(Max(ng, 1) * sizeof(sift_i128));
	struct
	{
		const void *src;
		int			esize;
	}			col[] = {{acc->n, 8}, {acc->i, 8}, {acc->d, 8}, {acc->nf, 1}, {acc->w, sizeof(sift_i128)}};

	for (int c = 0; c < lengthof(col); c++)
	{
		if (col[c].src == NULL)
			continue;
		for (uint32 r = 0; r < ng; r++)
			memcpy(tmp + (Size) r * col[c].esize, (const char *) col[c].src + (Size) gl[r] * col[c].esize, col[c].esize);
		bw_put(b, tmp, (Size) ng * col[c].esize);
	}
	pfree(tmp);
}

/* the numeric sums that left the integer form (nf): their numeric values, as (position, length, bytes) after a count; the sums of the others are in the columns */
static void
bw_numeric_sums(StringInfo b, const AggAcc *acc, const uint32 *gl, uint32 ng)
{
	uint32		hd[2] = {0, 0};

	for (uint32 r = 0; r < ng; r++)
		hd[0] += acc->nf[gl[r]] != 0 && acc->n[gl[r]] > 0;
	bw_put(b, hd, sizeof(hd));
	for (uint32 r = 0; r < ng; r++)
	{
		struct
		{
			uint32		pos;
			uint32		len;
		}			rec;
		Pointer		v;

		if (!acc->nf[gl[r]] || acc->n[gl[r]] == 0)
			continue;
		v = DatumGetPointer(acc->v[gl[r]]);
		rec.pos = r;
		rec.len = VARSIZE_ANY(v);
		bw_put(b, &rec, sizeof(rec));
		bw_put(b, v, rec.len);
	}
}

/* the float sums each lane kept (lane_keep): a bit mask of the lanes, then the sums of the groups of each */
static void
bw_lanes(StringInfo b, const AggAcc *acc, const uint32 *gl, uint32 ng)
{
	uint64		mask = 0;
	double	   *tmp = palloc(Max(ng, 1) * sizeof(double));

	for (int l = 0; l < SIFT_AGG_LANES; l++)
		mask |= (uint64) (acc->lv[l] != NULL) << l;
	bw_put(b, &mask, sizeof(mask));
	for (int l = 0; l < SIFT_AGG_LANES; l++)
	{
		if (acc->lv[l] == NULL)
			continue;
		for (uint32 r = 0; r < ng; r++)
			tmp[r] = acc->lv[l][gl[r]];
		bw_put(b, tmp, (Size) ng * sizeof(double));
	}
	pfree(tmp);
}

/* the values of an output table whose ids are listed in old[0..nused), as datums */
void
bw_values(StringInfo b, const OutTable *t, const uint32 *old, uint32 nused)
{
	StringInfoData vb;
	uint32		hd[2];

	initStringInfo(&vb);
	for (uint32 i = 0; i < nused; i++)
	{
		Datum		d = t->val[old[i] - 1];
		Size		sz = datumEstimateSpace(d, false, t->byval, t->len);
		char	   *at;

		enlargeStringInfo(&vb, sz);
		at = vb.data + vb.len;
		datumSerialize(d, false, t->byval, t->len, &at);
		vb.len += sz;
	}
	hd[0] = nused;
	hd[1] = vb.len;
	bw_put(b, hd, sizeof(hd));
	if (vb.len)
		bw_put(b, vb.data, vb.len);
	pfree(vb.data);
}

/* renumbers the ids ids[0..n) from 1 in order of use (0 stays 0); returns the old ids by new id */
uint32 *
renumber(uint32 *ids, uint32 n, uint32 nold, uint32 *nused)
{
	uint32	   *rm = palloc0((nold + 1) * sizeof(uint32)),
			   *old;

	*nused = 0;
	for (uint32 r = 0; r < n; r++)
		if (ids[r] && !rm[ids[r]])
			rm[ids[r]] = ++*nused;
	old = palloc(Max(*nused, 1) * sizeof(uint32));
	for (uint32 id = 1; id <= nold; id++)
		if (rm[id])
			old[rm[id] - 1] = id;
	for (uint32 r = 0; r < n; r++)
		ids[r] = ids[r] ? rm[ids[r]] : 0;
	pfree(rm);
	return old;
}

static uint32
group_key_id(AggExec *ex, int j, uint32 g)
{
	return ex->hash ? ex->hkey[(Size) g * ex->spec->nkeys + j] : (g / ex->ks[j].mult) % ex->ks[j].cap;
}

/* the values of the ids ids[0..n) of key j, and the ids renumbered from 1 in order of use (0 stays 0): the leader gives them its own */
static void
blob_key_table(AggExec *ex, int j, uint32 *ids, uint32 n, StringInfo b)
{
	KeyState   *k = &ex->ks[j];
	uint32	   *old,
				nused;

	old = renumber(ids, n, k->numk ? k->im.n : k->ident ? k->nident : k->ot.n, &nused);
	if (k->ident || k->numk)
	{
		/* the values themselves: 64 bits for an integer or the bits of a float, 128 for a numeric */
		Size		esz = k->numk ? sizeof(sift_i128) : sizeof(int64);
		uint32		hd[2] = {nused, nused * esz};
		char	   *vals = palloc(Max(nused, 1) * esz);

		for (uint32 i = 0; i < nused; i++)
			if (k->numk)
				memcpy(vals + i * esz, &k->im.val[old[i] - 1], esz);
			else
				memcpy(vals + i * esz, &k->idval[old[i] - 1], esz);
		bw_put(b, hd, sizeof(hd));
		bw_put(b, vals, nused * esz);
	}
	else
		bw_values(b, &k->ot, old, nused);
	pfree(old);
}

static void
blob_key(AggExec *ex, int j, const uint32 *gl, uint32 ng, StringInfo b)
{
	uint32	   *ids = palloc(Max(ng, 1) * sizeof(uint32));

	for (uint32 r = 0; r < ng; r++)
		ids[r] = group_key_id(ex, j, gl[r]);
	blob_key_table(ex, j, ids, ng, b);
	bw_put(b, ids, (Size) ng * sizeof(uint32));
	pfree(ids);
}

/* the pairs (group of the blob, id of key `slot`) of the count(DISTINCT) of a key the arms group by, with the values of the ids */
static void
blob_key_set(AggExec *ex, int ai, const uint32 *cidx, StringInfo b)
{
	uint32		np,
			   *pg,
			   *pv,
				hd[2];

	dist_pairs(ex, ai, cidx, &pg, &pv, &np);
	hd[0] = np;
	hd[1] = 0;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, pg, (Size) np * sizeof(uint32));
	blob_key_table(ex, ex->spec->agg[ai].slot, pv, np, b);
	bw_put(b, pv, (Size) np * sizeof(uint32));
	pfree(pg);
	pfree(pv);
}

/* the table of the values a combo slot gives ids to for aggregate ai (combo/out.c slot_ot) */
OutTable *
mset_table(AggExec *ex, int ai)
{
	const SiftAggAgg *a = &ex->spec->agg[ai];

	return &ex->aot[ex->spec->combo[a->combo].slot[a->slot].agg];
}

/* the pairs (group of the blob, id) of a count(DISTINCT) of a column of a joined table, with the values of the ids: the leader gives them its own */
static void
blob_mset(AggExec *ex, int ai, const uint32 *cidx, StringInfo b)
{
	OutTable   *t = mset_table(ex, ai);
	uint32		np,
			   *pg,
			   *pv,
			   *old,
				nused,
				hd[2];

	dset_pairs(ex, ai, cidx, &pg, &pv, &np);
	old = renumber(pv, np, t->n, &nused);
	hd[0] = np;
	hd[1] = 0;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, pg, (Size) np * sizeof(uint32));
	bw_values(b, t, old, nused);
	bw_put(b, pv, (Size) np * sizeof(uint32));
	pfree(pg);
	pfree(pv);
	pfree(old);
}

/* The hash of the key values of group g, the same in every table that holds the group (the ids are not): the partition of a spilled group */
uint32
blob_group_hash(AggExec *ex, uint32 g)
{
	uint64		h = 0;

	for (int j = 0; j < ex->spec->nkeys; j++)
	{
		KeyState   *k = &ex->ks[j];
		uint32		id = group_key_id(ex, j, g),
					kh = 0;

		if (id == 0)
			kh = 0;
		else if (k->numk)
			kh = DatumGetUInt32(hash_any((const unsigned char *) &k->im.val[id - 1], sizeof(sift_i128)));
		else if (k->ident)
			kh = DatumGetUInt32(hash_any((const unsigned char *) &k->idval[id - 1], sizeof(int64)));
		else
			kh = k->ot.hv[id - 1];
		h = h * 0x9E3779B97F4A7C15ULL + kh + 1;
	}
	return (uint32) (h >> 32) ^ (uint32) h;
}

void
blob_dump(AggExec *ex, int lane, StringInfo b)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				ng = 0,
			   *gl = palloc(Max(n, 1) * sizeof(uint32));

	dist_finish_all(ex);
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
			gl[ng++] = g;
	blob_dump_list(ex, lane, gl, ng, b);
	dist_end_all(ex);
	pfree(gl);
}

/*
 * The groups gl[0..ng) of the table (their rows have seen > 0) as a blob. cidx[g] is the number of group g in the blob. pairs, from
 * blob_pairs_split, holds the pairs of the sets of these groups (cidx is then not read for them): a table spilled to many partitions
 * walks each set once, not once for each partition.
 */
static void
blob_dump_with(AggExec *ex, int lane, const uint32 *gl, uint32 ng, const uint32 *cidx, const PartPairs *pairs, StringInfo b)
{
	SiftAggSpec *s = ex->spec;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;
	BlobHdr		h = {BLOB_MAGIC, lane, 0, 0};

	h.ngroups = ng;
	bw_put(b, &h, sizeof(h));
	for (int j = 0; j < s->nkeys; j++)
		blob_key(ex, j, gl, ng, b);
	{
		AggAcc		seen = {.n = ex->seen};

		bw_acc(b, &seen, gl, ng);
	}
	for (int i = 0; i < s->naggs; i++)
	{
		bw_acc(b, &ex->acc[i], gl, ng);
		if (numeric_sum_agg(&s->agg[i]))
			bw_numeric_sums(b, &ex->acc[i], gl, ng);
		if (set_agg(&s->agg[i]))
			blob_set(ex, i, gl, ng, cidx, pairs ? &pairs[i] : NULL, b);
		if (s->agg[i].kind == SAA_O_COUNT_KEY)
			blob_key_set(ex, i, cidx, b);
		if (s->agg[i].kind == SAA_M_DISTINCT)
			blob_mset(ex, i, cidx, b);
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			blob_osa(ex, i, cidx, n, b);
		if (s->agg[i].kind == SAA_GEN)
			blob_gen(ex, i, gl, ng, b);
	}
	bw_put(b, ex->amax, Max(s->naggs, 1) * sizeof(double));
	for (int k = 0; k < s->narms; k++)
		bw_put(b, ex->arm[k].amax, Max(s->arm[k].ninner, 1) * sizeof(double));
	for (int i = 0; i < s->naggs; i++)
		if (ex->acc[i].lv)
			bw_lanes(b, &ex->acc[i], gl, ng);
}

void
blob_dump_list(AggExec *ex, int lane, const uint32 *gl, uint32 ng, StringInfo b)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
			   *cidx = palloc(Max(n, 1) * sizeof(uint32));

	memset(cidx, 0xff, Max(n, 1) * sizeof(uint32));
	for (uint32 r = 0; r < ng; r++)
		cidx[gl[r]] = r;
	blob_dump_with(ex, lane, gl, ng, cidx, NULL, b);
	pfree(cidx);
}

/* can the sets of the table be split by partition (blob_pairs_split)? The other states that read cidx are one structure for all the groups */
bool
blob_splittable(const AggExec *ex)
{
	for (int i = 0; i < ex->spec->naggs; i++)
		if (ex->spec->agg[i].kind == SAA_O_COUNT_KEY || is_ordered_agg(&ex->spec->agg[i]))
			return false;
	return true;
}

/*
 * The table written to the nparts partitions, one blob each, with the groups of partition p as order[start[p] .. start[p] + fill[p]): the sets are
 * walked once for all the partitions. blob_write is called with each blob in the context sp->tmp, which it resets after.
 */
void
blob_dump_parts(AggExec *ex, int nparts, const uint32 *order, const uint32 *start, const uint32 *fill, MemoryContext tmp,
				void (*put) (void *arg, int p, StringInfo b), void *arg)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
			   *gpart = palloc(Max(n, 1) * sizeof(uint32)),
			   *cidx = palloc(Max(n, 1) * sizeof(uint32));
	int			naggs = ex->spec->naggs;
	PartPairs  *pp = palloc0((Size) nparts * Max(naggs, 1) * sizeof(PartPairs));

	memset(gpart, 0xff, Max(n, 1) * sizeof(uint32));
	memset(cidx, 0xff, Max(n, 1) * sizeof(uint32));
	for (int p = 0; p < nparts; p++)
		for (uint32 k = 0; k < fill[p]; k++)
		{
			gpart[order[start[p] + k]] = p;
			cidx[order[start[p] + k]] = k;
		}
	for (int i = 0; i < naggs; i++)
		if (set_agg(&ex->spec->agg[i]))
		{
			if (set_words(&ex->spec->agg[i], ex->dist[i]))
				continue;
			if (ex->dist[i])
				dist_pairs_split(ex, i, gpart, cidx, pp + i, naggs);
			else
				dset_pairs_split(ex, i, gpart, cidx, pp + i, naggs);
		}
	for (int p = 0; p < nparts; p++)
	{
		StringInfoData b;
		MemoryContext old;

		if (fill[p] == 0)
			continue;
		old = MemoryContextSwitchTo(tmp);
		initStringInfo(&b);
		blob_dump_with(ex, -1, order + start[p], fill[p], cidx, pp + (Size) p * naggs, &b);
		put(arg, p, &b);
		MemoryContextSwitchTo(old);
		MemoryContextReset(tmp);
		for (int i = 0; i < naggs; i++)
		{
			PartPairs  *q = pp + (Size) p * naggs + i;

			if (q->pg)
			{
				pfree(q->pg);
				pfree(q->pv);
			}
		}
	}
	pfree(pp);
	pfree(gpart);
	pfree(cidx);
}

