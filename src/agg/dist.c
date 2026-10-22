/*
 * agg/dist.c - count(DISTINCT): the sets of values and the bitmap, hash and sort engine
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

#define DSET_PAIR(g, v)		(UINT64CONST(1) << 63 | (uint64) (g) << 32 | (uint32) (v))

static uint64
dset_insert_raw(DSet *s, uint64 pair)
{
	uint64		h = dset_hash(pair) & (s->cap - 1);

	while (s->slot[h])
		h = (h + 1) & (s->cap - 1);
	s->slot[h] = pair;
	s->n++;
	return h;
}

/* twice the slots, the groups renumbered through remap (NULL: as they are) */
void
dset_grow(AggExec *ex, DSet *s, const uint32 *remap)
{
	uint64	   *old = s->slot;
	int64	   *ocnt = s->cnt;
	uint64		ocap = s->cap;

	s->cap = Max(ocap * 2, 1024);
	s->slot = MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(uint64));
	s->cnt = s->counted ? MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(int64)) : NULL;
	s->n = 0;
	for (uint64 i = 0; i < ocap; i++)
		if (old[i])
		{
			uint64		h = dset_insert_raw(s, remap ? DSET_PAIR(remap[(old[i] >> 32) & 0x7fffffff], old[i]) : old[i]);

			if (s->counted)
				s->cnt[h] = ocnt[i];
		}
	if (old)
		pfree(old);
	if (ocnt)
		pfree(ocnt);
}

/* true when the pair is new */
bool
dset_add(AggExec *ex, DSet *s, uint32 g, uint32 v)
{
	uint64		pair = DSET_PAIR(g, v),
				h;

	if (s->n * 2 >= s->cap)
		dset_grow(ex, s, NULL);
	h = dset_hash(pair) & (s->cap - 1);
	while (s->slot[h] && s->slot[h] != pair)
		h = (h + 1) & (s->cap - 1);
	if (s->slot[h])
	{
		if (s->counted)
			s->cnt[h]++;
		return false;
	}
	s->slot[h] = pair;
	s->n++;
	if (s->counted)
		s->cnt[h] = 1;
	return true;
}

Dist *
dist_new(AggExec *ex, bool sgn)
{
	Dist	   *d = MemoryContextAllocZero(ex->tcxt, sizeof(Dist));

	d->mode = DD_BITS;
	d->sgn = sgn;
	d->budget = get_hash_memory_limit();
	return d;
}

static inline int64
dist_x(const Dist *d, uint32 v)
{
	return d->sgn ? (int64) (int32) v : (int64) v;
}

typedef void (*DistFn) (void *arg, uint32 g, uint32 v);

/* every pair of a bitmap or hash structure (not a sort), through fn */
static void
dist_walk(const Dist *d, DistFn fn, void *arg)
{
	if (d->mode == DD_BITS)
	{
		for (uint32 g = 0; g < d->ngd; g++)
		{
			DistChunk  *dir = d->gd[g];

			for (uint32 c = 0; dir && c < d->ncd; c++)
			{
				if (!dir[c])
					continue;
				for (int w = 0; w < DB_WORDS; w++)
					for (uint64 m = dir[c][w]; m; m &= m - 1)
					{
						int64		x = (d->cbase + c) * (1 << DB_SHIFT) + w * 64 + pg_rightmost_one_pos64(m);

						fn(arg, g, (uint32) x);
					}
			}
		}
	}
	else if (d->mode == DD_HASH)
	{
		for (uint64 i = 0; i < d->ds.cap; i++)
			if (d->ds.slot[i])
				fn(arg, (d->ds.slot[i] >> 32) & 0x7fffffff, (uint32) d->ds.slot[i]);
	}
}

typedef struct DistMove
{
	AggExec    *ex;
	Dist	   *to;
} DistMove;

static void
dist_free_bits(Dist *d)
{
	for (uint32 g = 0; g < d->ngd; g++)
		if (d->gd[g])
			pfree(d->gd[g]);
	if (d->gd)
		pfree(d->gd);
	while (d->arena)
	{
		uint64	   *prev = (uint64 *) d->arena[0];

		pfree(d->arena);
		d->arena = prev;
	}
}

static void
dist_move(void *arg, uint32 g, uint32 v)
{
	DistMove   *mv = arg;

	(void) dist_add(mv->ex, mv->to, g, v);
}

/* a tuplesort of (int4, int8) rows ordered by both, and the slots that put rows in and read them back; a third int4 column rides along unsorted. In the current memory context */
void
agg_pair_sort(int natts, int flags, Tuplesortstate **sort, TupleTableSlot **put, TupleTableSlot **get)
{
	TupleDesc	desc = CreateTemplateTupleDesc(natts);
	AttrNumber	att[2] = {1, 2};
	Oid			ops[2] = {Int4LessOperator, Int8LessOperator},
				coll[2] = {InvalidOid, InvalidOid};
	bool		nulls[2] = {false, false};

	TupleDescInitEntry(desc, 1, "a", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "b", INT8OID, -1, 0);
	if (natts > 2)
		TupleDescInitEntry(desc, 3, "c", INT4OID, -1, 0);
	sift_tupdesc_finalize(desc);
	*put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	*get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	*sort = tuplesort_begin_heap(desc, 2, att, ops, coll, nulls, work_mem, NULL, flags);
}

/* the structure of d moves to mode `to`, its pairs added to the new one */
static void
dist_convert(AggExec *ex, Dist *d, DistMode to)
{
	Dist		old = *d;
	DistMove	mv = {ex, d};

	if (to == DD_SORT)
	{
		MemoryContext mc = MemoryContextSwitchTo(ex->cxt);

		agg_pair_sort(2, TUPLESORT_RANDOMACCESS, &d->sort, &d->put, &d->get);
		MemoryContextSwitchTo(mc);
		d->nsid = d->nsidcap = d->sidgroups = 0;
		d->sid = NULL;
		ex->n_dist_spilled++;
	}
	memset(&d->ds, 0, sizeof(DSet));
	d->mode = to;
	d->gd = NULL;
	d->ngd = d->ncd = 0;
	d->arena = NULL;
	d->arena_left = 0;
	d->bytes = 0;
	d->maxn = Max(d->budget / (4 * sizeof(uint64)), 256);	/* the table is at most half full, and a power of 2 */
	dist_walk(&old, dist_move, &mv);
	if (old.mode == DD_BITS)
		dist_free_bits(&old);
	else if (old.ds.slot)
		pfree(old.ds.slot);
}

static void
dist_put_sort(Dist *d, uint32 g, uint32 v)
{
	TupleTableSlot *t = d->put;

	if (g >= d->sidgroups)
	{
		uint32		n = Max(Max(g + 1, d->sidgroups * 2), 64);
		uint32	   *ns = MemoryContextAllocZero(GetMemoryChunkContext(d), n * sizeof(uint32));

		if (d->sid)
		{
			memcpy(ns, d->sid, d->sidgroups * sizeof(uint32));
			pfree(d->sid);
		}
		d->sid = ns;
		d->sidgroups = n;
	}
	if (d->sid[g] == 0)
		d->sid[g] = ++d->nsid;
	ExecClearTuple(t);
	t->tts_values[0] = Int32GetDatum((int32) (d->sid[g] - 1));
	t->tts_values[1] = Int64GetDatum((int64) v);
	t->tts_isnull[0] = t->tts_isnull[1] = false;
	ExecStoreVirtualTuple(t);
	tuplesort_puttupleslot(d->sort, t);
}

/* a bitmap chunk of 4096 zero bits */
static uint64 *
dist_chunk(AggExec *ex, Dist *d)
{
	if (d->arena_left == 0)
	{
		uint64	   *blk = MemoryContextAllocZero(ex->tcxt, (DB_ARENA + 1) * DB_WORDS * sizeof(uint64));

		blk[0] = (uint64) d->arena;
		d->arena = blk;
		d->arena_left = DB_ARENA;
	}
	return d->arena + (size_t) (DB_ARENA - --d->arena_left) * DB_WORDS;
}

/* the directories cover the chunk xc; false if they cannot (the set is no longer a bitmap) */
static bool
dist_cover(AggExec *ex, Dist *d, int64 xc)
{
	int64		lo,
				hi;
	uint32		nn;
	int64		shift;
	uint64		touched = 0;

	if (d->ncd == 0)
	{
		d->cbase = xc & ~(int64) (DB_NCD_MIN / 2 - 1);
		d->ncd = DB_NCD_MIN;
		return true;
	}
	if (xc >= d->cbase && xc < d->cbase + d->ncd)
		return true;
	lo = Min(d->cbase, xc) & ~(int64) (DB_NCD_MIN / 2 - 1);
	hi = Max(d->cbase + d->ncd - 1, xc);
	if (hi - lo + 1 > DB_NCD_MAX)
		return false;
	nn = pg_nextpower2_32((uint32) (hi - lo + 1));
	for (uint32 g = 0; g < d->ngd; g++)
		touched += d->gd[g] != NULL;
	if (d->bytes + touched * (nn - d->ncd) * sizeof(DistChunk) > d->budget)
		return false;
	shift = d->cbase - lo;
	for (uint32 g = 0; g < d->ngd; g++)
		if (d->gd[g])
		{
			DistChunk  *nd = MemoryContextAllocZero(ex->tcxt, nn * sizeof(DistChunk));

			memcpy(nd + shift, d->gd[g], d->ncd * sizeof(DistChunk));
			pfree(d->gd[g]);
			d->gd[g] = nd;
		}
	d->bytes += touched * (nn - d->ncd) * sizeof(DistChunk);
	d->cbase = lo;
	d->ncd = nn;
	return true;
}

/*
 * The chunk of 4096 bits that holds chunk number xc of group g, allocated if it is not there yet; NULL when the set stopped being a
 * bitmap (it has moved to a hash set and nothing was added).
 */
static uint64 *
dist_chunk_for(AggExec *ex, Dist *d, uint32 g, int64 xc)
{
	DistChunk  *dir;

	if (!dist_cover(ex, d, xc))
		goto convert;
	if (g >= d->ngd)
	{
		uint32		n = Max(Max(g + 1, d->ngd * 2), 64);
		DistChunk **ng = MemoryContextAllocZero(ex->tcxt, n * sizeof(DistChunk *));

		if (d->gd)
		{
			memcpy(ng, d->gd, d->ngd * sizeof(DistChunk *));
			pfree(d->gd);
		}
		d->gd = ng;
		d->ngd = n;
	}
	if (d->gd[g] == NULL)
	{
		if (d->bytes + d->ncd * sizeof(DistChunk) > d->budget)
			goto convert;
		d->gd[g] = MemoryContextAllocZero(ex->tcxt, d->ncd * sizeof(DistChunk));
		d->bytes += d->ncd * sizeof(DistChunk);
	}
	dir = d->gd[g];
	if (dir[xc - d->cbase] == NULL)
	{
		if (d->bytes + DB_WORDS * sizeof(uint64) > d->budget)
			goto convert;
		dir[xc - d->cbase] = dist_chunk(ex, d);
		d->bytes += DB_WORDS * sizeof(uint64);
	}
	return dir[xc - d->cbase];
convert:
	dist_convert(ex, d, DD_HASH);
	return NULL;
}

/*
 * The bit of x in group g, when the fast path of the caller did not find its chunk: 1 if it was not set, 0 if it was, -1 when the
 * set stopped being a bitmap (and the pair is not added: the caller goes on with dist_add).
 */
static int
dist_bits_slow(AggExec *ex, Dist *d, uint32 g, int64 x)
{
	uint64	   *ch = dist_chunk_for(ex, d, g, x >> DB_SHIFT);
	uint64	   *w;
	uint64		m = UINT64CONST(1) << (x & 63);

	if (ch == NULL)
		return -1;
	w = &ch[(x >> 6) & (DB_WORDS - 1)];
	if (*w & m)
		return 0;
	*w |= m;
	return 1;
}

static bool
dist_hash_add(AggExec *ex, Dist *d, uint32 g, uint32 v)
{
	DSet	   *s = &d->ds;
	uint64		pair = DSET_PAIR(g, v),
				h;

	if (s->cap == 0)
		dset_grow(ex, s, NULL);
	h = dset_hash(pair) & (s->cap - 1);
	while (s->slot[h] && s->slot[h] != pair)
		h = (h + 1) & (s->cap - 1);
	if (s->slot[h])
		return false;
	if (s->n >= d->maxn)
	{
		dist_convert(ex, d, DD_SORT);
		dist_put_sort(d, g, v);
		return false;
	}
	s->slot[h] = pair;
	if (++s->n * 2 >= s->cap)
		dset_grow(ex, s, NULL);
	return true;
}

bool
dist_add(AggExec *ex, Dist *d, uint32 g, uint32 v)
{
	if (d->mode == DD_BITS)
	{
		int			r = dist_bits_slow(ex, d, g, dist_x(d, v));

		if (r >= 0)
			return r == 1;
	}
	if (d->mode == DD_HASH)
		return dist_hash_add(ex, d, g, v);
	dist_put_sort(d, g, v);
	return false;
}

/*
 * The bits of another bitmap added to the set of group g, how many were new: nwords words, little endian and not aligned, the first of which holds the
 * values from 64 * wbase (a word of a chunk is DB_WORDS words long)
 */
uint64
dist_or_words(AggExec *ex, Dist *d, uint32 g, int64 wbase, const uint8 *words, uint64 nwords)
{
	uint64		n = 0;

	for (uint64 i = 0; i < nwords;)
	{
		int64		xc = (wbase + (int64) i) >> (DB_SHIFT - 6);
		uint32		w0 = (uint32) ((wbase + (int64) i) & (DB_WORDS - 1));
		uint64		cnt = Min(nwords - i, DB_WORDS - w0);
		uint64	   *ch = d->mode == DD_BITS ? dist_chunk_for(ex, d, g, xc) : NULL;

		for (uint64 k = 0; k < cnt; k++)
		{
			uint64		m;

			memcpy(&m, words + (i + k) * sizeof(m), sizeof(m));
			if (ch)
			{
				n += pg_popcount64(m & ~ch[w0 + k]);
				ch[w0 + k] |= m;
				continue;
			}
			for (; m; m &= m - 1)
				n += dist_add(ex, d, g, (uint32) (xc * (1 << DB_SHIFT) + (w0 + k) * 64 + pg_rightmost_one_pos64(m)));
		}
		i += cnt;
	}
	return n;
}

/* the bits of a chunk of another bitmap (chunk number xc of group g) added to the set; how many were new */
uint64
dist_or_chunk(AggExec *ex, Dist *d, uint32 g, int64 xc, const uint64 *bits)
{
	return dist_or_words(ex, d, g, xc * DB_WORDS, (const uint8 *) bits, DB_WORDS);
}

/* the groups renumbered through remap (UINT32_MAX: dropped) while the scan runs; newcap is their new capacity */
void
dist_regroup(AggExec *ex, Dist *d, const uint32 *remap, uint32 oldcap, uint32 newcap)
{
	if (d->mode == DD_HASH)
	{
		if (d->ds.slot)
			dset_grow(ex, &d->ds, remap);
		return;
	}
	if (d->mode == DD_BITS && d->gd)
	{
		DistChunk **ng = MemoryContextAllocZero(ex->tcxt, Max(newcap, 1) * sizeof(DistChunk *));

		for (uint32 g = 0; g < Min(oldcap, d->ngd); g++)
			if (d->gd[g] && remap[g] != UINT32_MAX)
				ng[remap[g]] = d->gd[g];
			else if (d->gd[g])
				pfree(d->gd[g]);
		pfree(d->gd);
		d->gd = ng;
		d->ngd = Max(newcap, 1);
	}
	if (d->mode == DD_SORT && d->sid)
	{
		uint32	   *ns = MemoryContextAllocZero(GetMemoryChunkContext(d), Max(newcap, 1) * sizeof(uint32));

		for (uint32 g = 0; g < Min(oldcap, d->sidgroups); g++)
			if (d->sid[g] && remap[g] != UINT32_MAX)
				ns[remap[g]] = d->sid[g];
		pfree(d->sid);
		d->sid = ns;
		d->sidgroups = Max(newcap, 1);
	}
}

/* the fields of a bitmap set the loops of dist_update read, in registers: the stores to the bits would make the compiler reload them */
typedef struct DistView
{
	int64		cbase;
	uint32		ncd;
	uint32		ngd;
	DistChunk **gd;
	bool		sgn;
} DistView;

static inline DistView
dist_view(const Dist *d)
{
	DistView	vw = {d->cbase, d->ncd, d->ngd, d->gd, d->sgn};

	return vw;
}

static inline int64
dist_vx(const DistView *vw, uint32 v)
{
	return vw->sgn ? (int64) (int32) v : (int64) v;
}

/* the word of the bit of x in group g and its mask, when the chunk is there; NULL when it is not */
static inline uint64 *
dist_word(const DistView *vw, uint32 g, int64 x, uint64 *m)
{
	int64		c = (x >> DB_SHIFT) - vw->cbase;
	DistChunk  *dir;

	if (unlikely((uint64) c >= vw->ncd || g >= vw->ngd || (dir = vw->gd[g]) == NULL || dir[c] == NULL))
		return NULL;
	*m = UINT64CONST(1) << (x & 63);
	return &dir[c][(x >> 6) & (DB_WORDS - 1)];
}

/* adds the selected values (v[j] of tuple j, in the group ex->gid[j]); the count of a group goes up for each new one */
void
dist_update(AggExec *ex, int ai, const uint64 *ok, int nsel, const uint32 *v)
{
	Dist	   *d = ex->dist[ai];
	int64	   *cnt = ex->acc[ai].n;
	const uint32 *gid = ex->gid;
	DistView	vw = dist_view(d);
	int			j = 0;

	/* bits that fit the cache are set one by one; past that, a batch at a time: the words are found and prefetched first, since each is a miss */
	for (; j < nsel && d->mode == DD_BITS && d->bytes < DB_PREFETCH; j++)
	{
		uint32		g = gid[j];
		uint64		m;
		uint64	   *w;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if ((w = dist_word(&vw, g, dist_vx(&vw, v[j]), &m)) != NULL)
		{
			cnt[g] += !(*w & m);
			*w |= m;
		}
		else
		{
			int			r = dist_bits_slow(ex, d, g, dist_x(d, v[j]));

			if (r < 0)
				break;
			cnt[g] += r;
			vw = dist_view(d);
		}
	}
	while (j < nsel && d->mode == DD_BITS)
	{
		int			nb = Min(DB_BATCH, nsel - j);
		uint64	   *wp[DB_BATCH];
		uint64		mk[DB_BATCH];
		bool		take[DB_BATCH];
		int			k;

		for (k = 0; k < nb; k++)
		{
			take[k] = !ok || sift_colsrc_bit(ok, j + k);
			wp[k] = take[k] ? dist_word(&vw, gid[j + k], dist_vx(&vw, v[j + k]), &mk[k]) : NULL;
			if (wp[k])
				__builtin_prefetch(wp[k], 1);
		}
		for (k = 0; k < nb; k++)
		{
			uint32		g = gid[j + k];

			if (wp[k])
			{
				cnt[g] += !(*wp[k] & mk[k]);
				*wp[k] |= mk[k];
			}
			else if (take[k])
			{
				int			r = dist_bits_slow(ex, d, g, dist_x(d, v[j + k]));

				if (r < 0)
					break;
				cnt[g] += r;
			}
		}
		j += k;
		if (k < nb)
			break;
		vw = dist_view(d);
	}
	if (j < nsel && d->mode == DD_HASH && d->ds.n + (uint64) (nsel - j) < d->maxn)
	{
		DSet	   *s = &d->ds;
		uint64		pair[32],
					h[32];

		while ((s->n + (nsel - j)) * 2 >= s->cap)
			dset_grow(ex, s, NULL);
		while (j < nsel)
		{
			int			m = 0,
						jj[32];

			for (; j < nsel && m < 32; j++)
				if (!ok || sift_colsrc_bit(ok, j))
				{
					pair[m] = DSET_PAIR(ex->gid[j], v[j]);
					h[m] = dset_hash(pair[m]) & (s->cap - 1);
					__builtin_prefetch(&s->slot[h[m]]);
					jj[m++] = j;
				}
			for (int k = 0; k < m; k++)
			{
				uint64		p = h[k];

				while (s->slot[p] && s->slot[p] != pair[k])
					p = (p + 1) & (s->cap - 1);
				if (!s->slot[p])
				{
					s->slot[p] = pair[k];
					s->n++;
					cnt[ex->gid[jj[k]]]++;
				}
			}
		}
	}
	for (; j < nsel; j++)
		if ((!ok || sift_colsrc_bit(ok, j)) && dist_add(ex, d, ex->gid[j], v[j]))
			cnt[ex->gid[j]]++;
}

/* DD_SORT: sorts the pairs and sets the count of each group */
static void
dist_finish(AggExec *ex, int ai)
{
	Dist	   *d = ex->dist[ai];
	AggAcc	   *acc = &ex->acc[ai];
	TupleTableSlot *t;
	bool		isnull;
	int32		prev_sid = -1;
	int64		prev_val = 0;
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;

	if (d == NULL || d->mode != DD_SORT || d->done)
		return;
	tuplesort_performsort(d->sort);
	d->cnt = MemoryContextAllocZero(ex->tcxt, Max(d->nsid, 1) * sizeof(uint64));
	t = d->get;
	while (tuplesort_gettupleslot(d->sort, true, false, t, NULL))
	{
		int32		sid = DatumGetInt32(slot_getattr(t, 1, &isnull));
		int64		val = DatumGetInt64(slot_getattr(t, 2, &isnull));

		if (sid != prev_sid || val != prev_val)
			d->cnt[sid]++;
		prev_sid = sid;
		prev_val = val;
	}
	for (uint32 g = 0; g < Min(ngr, d->sidgroups); g++)
		if (d->sid[g])
			acc->n[g] = d->cnt[d->sid[g] - 1];
	d->done = true;
}

void
dist_finish_all(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		dist_finish(ex, ai);
}

/* the sorts of the tables about to be forgotten */
void
dist_end_all(AggExec *ex)
{
	if (ex->dist == NULL)
		return;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		if (ex->dist[ai] && ex->dist[ai]->sort)
		{
			tuplesort_end(ex->dist[ai]->sort);
			ex->dist[ai]->sort = NULL;
		}
}

typedef struct DistPairs
{
	const uint32 *cidx;
	uint32	   *pg;
	uint32	   *pv;
	uint32		np;
} DistPairs;

static void
dist_collect(void *arg, uint32 g, uint32 v)
{
	DistPairs  *dp = arg;

	if (dp->cidx[g] == UINT32_MAX)
		return;
	dp->pg[dp->np] = dp->cidx[g];
	dp->pv[dp->np++] = v;
}

/* the pairs of aggregate ai in the groups of cidx (blob numbering): the arrays pg and pv, np of them */
void
dist_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out)
{
	Dist	   *d = ex->dist[ai];
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				np = 0;
	uint64		total = 0;
	uint32	   *pg,
			   *pv;

	dist_finish(ex, ai);
	for (uint32 g = 0; g < n; g++)
		if (cidx[g] != UINT32_MAX)
			total += ex->acc[ai].n[g];
	pg = palloc(Max(total, 1) * sizeof(uint32));
	pv = palloc(Max(total, 1) * sizeof(uint32));
	if (d->mode == DD_SORT)
	{
		uint32	   *sg = palloc0(Max(d->nsid, 1) * sizeof(uint32));
		bool		isnull;
		int32		prev_sid = -1;
		int64		prev_val = 0;

		for (uint32 g = 0; g < Min(n, d->sidgroups); g++)
			if (d->sid[g])
				sg[d->sid[g] - 1] = g;
		tuplesort_rescan(d->sort);
		while (tuplesort_gettupleslot(d->sort, true, false, d->get, NULL))
		{
			int32		sid = DatumGetInt32(slot_getattr(d->get, 1, &isnull));
			int64		val = DatumGetInt64(slot_getattr(d->get, 2, &isnull));

			if ((sid != prev_sid || val != prev_val) && cidx[sg[sid]] != UINT32_MAX)
			{
				pg[np] = cidx[sg[sid]];
				pv[np++] = (uint32) val;
			}
			prev_sid = sid;
			prev_val = val;
		}
		pfree(sg);
	}
	else
	{
		DistPairs	dp = {cidx, pg, pv, 0};

		dist_walk(d, dist_collect, &dp);
		np = dp.np;
	}
	*pg_out = pg;
	*pv_out = pv;
	*np_out = np;
}

/* The (group of cidx, value id) pairs of the set of a sum or average over distinct values (ex->dset[ai]), as dist_pairs gives those of a count */
void
dset_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out)
{
	const DSet *s = &ex->dset[ai];
	uint32		np = 0;

	*pg_out = palloc(Max(s->n, 1) * sizeof(uint32));
	*pv_out = palloc(Max(s->n, 1) * sizeof(uint32));
	for (uint64 i = 0; i < s->cap; i++)
		if (s->slot[i] && cidx[(s->slot[i] >> 32) & 0x7fffffff] != UINT32_MAX)
		{
			(*pg_out)[np] = cidx[(s->slot[i] >> 32) & 0x7fffffff];
			(*pv_out)[np++] = (uint32) s->slot[i];
		}
	*np_out = np;
}
