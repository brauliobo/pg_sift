/*
 * agg/blob.c - partial group tables: dump and merge of the table of a worker or lane
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

static void
bw_put(StringInfo b, const void *p, Size n)
{
	appendBinaryStringInfo(b, p, n);
	while (b->len % 8)
		appendStringInfoChar(b, 0);
}

static const void *
br_get(const char **p, Size n)
{
	const void *r = *p;

	*p += TYPEALIGN(8, n);
	return r;
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

/* the values of an output table whose ids are listed in old[0..nused), as datums */
static void
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
static uint32 *
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

/* the table of the values a combo slot gives ids to for aggregate ai (combo.c slot_ot) */
static OutTable *
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

/* count(DISTINCT x), and the sum and average over distinct numbers: their partial state is a set of (group, value) pairs, not an accumulator */
static bool
set_agg(const SiftAggAgg *a)
{
	return a->kind == SAA_COUNT_DISTINCT || ((a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM) && a->distinct);
}

/* a narrow set (DD_WORDS) goes as its words; the ids of a numeric expression are the worker's own and go as pairs */
static bool
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
static void
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

typedef struct OsaRec
{
	uint32		g;				/* group of the blob */
	uint32		pad;
	uint64		key;
	uint64		cnt;
} OsaRec;

/* the values of an ordered-set aggregate that owns them, as runs (group of the blob, value, count): the histogram or the sorted tuples */
static void
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
static void
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

typedef struct BlobAgg
{
	AggAcc		a;				/* the arrays of the blob, read only */
	uint32		npairs;
	const uint32 *pg;
	const uint32 *pv;
	uint32		nchunks;		/* count(DISTINCT) bitmap chunks: group and number of each, then their bits */
	const uint32 *cg;
	const uint32 *cc;
	const uint64 *bits;
	uint32		nwg;			/* a narrow set: the rows of words, of nk words, the first holding the values from 64 * wbase; the group of each */
	uint32		nk;
	int64		wbase;
	const uint32 *wg;
	const uint64 *wrows;
	uint32	   *xv;				/* dictionary values: the ids in the table */
	uint32		nrec;			/* ordered sets: the runs of the values */
	const OsaRec *rec;
	const char *gcur;			/* generic aggregates: the next partial state */
} BlobAgg;

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
			case SAA_SUM_F64:
			case SAA_AVG_F64:
			case SAA_X_SUM:
			case SAA_X_AVG:
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
	pfree(gids);
	pfree(ba);
}
