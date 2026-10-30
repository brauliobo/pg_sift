/*
 * agg/blob/sets.c - partial group tables: the sets and states of an aggregate
 * The sets of count(DISTINCT), the ordered-set values and the generic states written for the
 * groups of a dump.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "utils/tuplesort.h"

#include "agg/blob/int.h"

/* count(DISTINCT x), and the sum and average over distinct numbers: their partial state is a set of (group, value) pairs, not an accumulator */
bool
set_agg(const SiftAggAgg *a)
{
	return a->kind == SAA_COUNT_DISTINCT || ((a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM) && a->distinct);
}

/* a narrow set (DD_WORDS) goes as its words; the ids of a numeric expression are the worker's own and go as pairs */
bool
set_words(const SiftAggAgg *a, const Dist *d)
{
	return d && d->mode == DD_WORDS && a->num.n == 0;
}

/*
 * The set of a count(DISTINCT) of aggregate ai in the groups of cidx (blob numbering), header {pairs, chunks, rows of words, words of a row}:
 * the (group, value) pairs, then the (group, chunk number) of each bitmap chunk and its bits. A set that is still a bitmap of raw values
 * sends the chunks with DB_DENSE bits or more as they are, so that the leader ORs them and counts by popcount, and the bits of the others
 * as pairs; what it sends is bounded by the memory of the bitmap, hash_mem. A narrow set sends no pairs: the first word of its window,
 * the group (its place in gl) of each row that has a bit, and the rows, which the leader ORs in the same way. The values of a dictionary
 * leaf go by value, and so do the numbers of a numeric expression (the ids of the worker's table of them, which the leader gives its own);
 * a narrow set sends the values of the ids that have a bit, in order of id.
 */
void
blob_set(AggExec *ex, int ai, const uint32 *gl, uint32 ng, const uint32 *cidx, const PartPairs *pre, StringInfo b)
{
	const SiftAggAgg *a = &ex->spec->agg[ai];
	Dist	   *d = ex->dist[ai];
	bool		dict = a->leaf >= 0 && ex->lay->leaf[a->leaf].vtype == SCV_CODE,
				num = a->num.n != 0,
				words = set_words(a, d);
	uint32		np,
			   *pg,
			   *pv,
				hd[4],
			   *old = NULL,
				nused = 0,
				nc = 0,
				nwg = 0,
			   *wg = NULL,
				none[1] = {0},
			   *cg = none,
			   *cc = none;
	uint64	   *wrows = NULL;
	const uint64 **bits = NULL;

	if (words)
	{
		uint32		nk = d->nk;
		uint64	   *seen = palloc0(Max(nk, 1) * sizeof(uint64));

		np = 0;
		pg = pv = none;
		wg = palloc(Max(ng, 1) * sizeof(uint32));
		wrows = palloc(Max((Size) ng * nk, 1) * sizeof(uint64));
		for (uint32 r = 0; r < ng; r++)
		{
			const uint64 *row;
			uint32		w = 0;

			if (gl[r] >= d->nng)
				continue;
			row = d->nw + (size_t) gl[r] * nk;
			while (w < nk && row[w] == 0)
				w++;
			if (w == nk)
				continue;
			memcpy(wrows + (size_t) nwg * nk, row, nk * sizeof(uint64));
			for (; w < nk; w++)
				seen[w] |= row[w];
			wg[nwg++] = r;
		}
		if (dict)
		{
			old = palloc(Max(nk * 64, 1) * sizeof(uint32));
			for (uint32 w = 0; w < nk; w++)
				for (uint64 m = seen[w]; m; m &= m - 1)
					old[nused++] = (uint32) (d->nlo + w * 64 + pg_rightmost_one_pos64(m));
		}
		pfree(seen);
	}
	else if (pre)
	{
		pg = pre->pg ? pre->pg : none;
		pv = pre->pv ? pre->pv : none;
		np = pre->np;
	}
	else if (d && d->mode == DD_BITS && d->sgn)
	{
		uint32		n = Min(ex->hash ? ex->ngroups : ex->gcap, d->ngd);
		uint64		total = 0;

		for (uint32 g = 0; g < n; g++)
			if (cidx[g] != UINT32_MAX)
				total += ex->acc[ai].n[g];
		pg = palloc(Max(total, 1) * sizeof(uint32));
		pv = palloc(Max(total, 1) * sizeof(uint32));
		cg = palloc(Max(total / DB_DENSE, 1) * sizeof(uint32));
		cc = palloc(Max(total / DB_DENSE, 1) * sizeof(uint32));
		bits = palloc(Max(total / DB_DENSE, 1) * sizeof(uint64 *));
		np = 0;
		for (uint32 g = 0; g < n; g++)
			for (uint32 c = 0; d->gd[g] && cidx[g] != UINT32_MAX && c < d->ncd; c++)
			{
				const uint64 *ch = d->gd[g][c];
				int			pop = 0;

				for (int w = 0; ch && w < DB_WORDS; w++)
					pop += pg_popcount64(ch[w]);
				if (pop >= DB_DENSE)
				{
					cg[nc] = cidx[g];
					cc[nc] = (uint32) (d->cbase + c);
					bits[nc++] = ch;
				}
				else if (pop > 0)
					for (int w = 0; w < DB_WORDS; w++)
						for (uint64 m = ch[w]; m; m &= m - 1)
						{
							pg[np] = cidx[g];
							pv[np++] = (uint32) ((d->cbase + c) * (1 << DB_SHIFT) + w * 64 + pg_rightmost_one_pos64(m));
						}
			}
	}
	else if (d)
		dist_pairs(ex, ai, cidx, &pg, &pv, &np);
	else
		dset_pairs(ex, ai, cidx, &pg, &pv, &np);
	if (!words && dict)
		old = renumber(pv, np, ex->aot[ai].n, &nused);
	else if (!words && num)
		old = renumber(pv, np, ex->amap[ai].n, &nused);
	hd[0] = np;
	hd[1] = nc;
	hd[2] = nwg;
	hd[3] = words ? d->nk : 0;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, pg, (Size) np * sizeof(uint32));
	bw_put(b, pv, (Size) np * sizeof(uint32));
	bw_put(b, cg, (Size) nc * sizeof(uint32));
	bw_put(b, cc, (Size) nc * sizeof(uint32));
	for (uint32 i = 0; i < nc; i++)
		appendBinaryStringInfo(b, (const char *) bits[i], DB_WORDS * sizeof(uint64));
	if (nwg)
	{
		int64		wbase = d->nlo / 64;

		bw_put(b, &wbase, sizeof(wbase));
		bw_put(b, wg, (Size) nwg * sizeof(uint32));
		bw_put(b, wrows, (Size) nwg * d->nk * sizeof(uint64));
	}
	if (dict)
		bw_values(b, &ex->aot[ai], old, nused);
	else if (num)
	{
		uint32		vh[2] = {nused, nused * sizeof(sift_i128)};

		bw_put(b, vh, sizeof(vh));
		for (uint32 i = 0; i < nused; i++)
			bw_put(b, &ex->amap[ai].val[old[i] - 1], sizeof(sift_i128));
	}
}

/* the values of an ordered-set aggregate that owns them, as runs (group of the blob, value, count): the histogram or the sorted tuples */
void
blob_osa(AggExec *ex, int ai, const uint32 *cidx, uint32 n, StringInfo b)
{
	Osa		   *o = ex->osa[ai];
	AggAcc	   *own = &ex->acc[ai];
	uint32	   *sid2g = palloc(Max(o->nsid, 1) * sizeof(uint32)),
				hd[2] = {0, 0};
	StringInfoData rb;
	OsaRun		r;

	for (uint32 g = 0; g < n; g++)
		if (own->i[g])
			sid2g[own->i[g] - 1] = cidx[g];
	initStringInfo(&rb);
	if (o->sort)
	{
		osa_sort(o);
		while (osa_next(o, &r))
		{
			OsaRec		rec = {sid2g[r.sid], 0, r.key, r.cnt};

			appendBinaryStringInfo(&rb, (const char *) &rec, sizeof(rec));
			hd[0]++;
		}
		tuplesort_end(o->sort);
		o->sort = NULL;
	}
	else
		for (uint64 i = 0; i < o->cap; i++)
			if (o->slot[i].sid1)
			{
				OsaRec		rec = {sid2g[o->slot[i].sid1 - 1], 0, o->slot[i].key, o->slot[i].cnt};

				appendBinaryStringInfo(&rb, (const char *) &rec, sizeof(rec));
				hd[0]++;
			}
	bw_put(b, hd, sizeof(hd));
	bw_put(b, rb.data, rb.len);
	pfree(rb.data);
	pfree(sid2g);
}

/* the partial states of the generic aggregate ai, one record per group of gl */
void
blob_gen(AggExec *ex, int ai, const uint32 *gl, uint32 ng, StringInfo b)
{
	StringInfoData vb;
	uint32		hd[2];
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	initStringInfo(&vb);
	for (uint32 r = 0; r < ng; r++)
		gen_state_put(ex, ex->gst[ai], &ex->acc[ai], gl[r], &vb);
	MemoryContextSwitchTo(old);
	hd[0] = ng;
	hd[1] = vb.len;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, vb.data, vb.len);
	pfree(vb.data);
	ResetExprContext(ex->lutcx);
}
