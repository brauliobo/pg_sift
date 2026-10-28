/*
 * agg/osa.c - ordered-set aggregates: histograms, spill, sort
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
#include "core/cmp.h"

#define OSA_SIGN	(UINT64CONST(1) << 63)

/* the key of a float8 that sorts as float8_cmp does: unsigned order, -0 and 0 one key, every NaN the last */
static inline uint64
f8_key(double x)
{
	uint64		b;

	if (isnan(x))
		return UINT64_MAX;
	x += 0.0;					/* -0 -> 0 */
	memcpy(&b, &x, sizeof(b));
	return (b & OSA_SIGN) ? ~b : (b | OSA_SIGN);
}

static inline double
key_f8(uint64 k)
{
	uint64		b = (k & OSA_SIGN) ? (k & ~OSA_SIGN) : ~k;
	double		x;

	if (k == UINT64_MAX)
		return get_float8_nan();
	memcpy(&x, &b, sizeof(x));
	return x;
}

static inline uint64
osa_hash(uint32 sid, uint64 key)
{
	return dset_hash(key ^ ((uint64) sid * UINT64CONST(0xC2B2AE3D27D4EB4F)));
}

Osa *
osa_new(AggExec *ex)
{
	Osa		   *o = MemoryContextAllocZero(ex->tcxt, sizeof(Osa));

	o->cap = 1024;
	o->slot = MemoryContextAllocZero(ex->tcxt, o->cap * sizeof(OsaSlot));
	o->maxn = Max((uint64) work_mem * 1024 / (4 * sizeof(OsaSlot)), 256);	/* the table has at most 4 slots per pair */
	return o;
}

static void
osa_put(Osa *o, uint32 sid, uint64 key, uint64 cnt)
{
	TupleTableSlot *t = o->put;

	for (; cnt > 0; cnt--)
	{
		ExecClearTuple(t);
		t->tts_values[0] = Int32GetDatum((int32) sid);
		t->tts_values[1] = Int64GetDatum((int64) (key ^ OSA_SIGN));
		t->tts_isnull[0] = t->tts_isnull[1] = false;
		ExecStoreVirtualTuple(t);
		tuplesort_puttupleslot(o->sort, t);
	}
}

/* the histogram does not fit work_mem: its pairs, and from now on every value, go through a tuplesort */
static void
osa_spill(AggExec *ex, Osa *o)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	agg_pair_sort(2, TUPLESORT_NONE, &o->sort, &o->put, &o->get);
	MemoryContextSwitchTo(old);
	for (uint64 i = 0; i < o->cap; i++)
		if (o->slot[i].sid1)
			osa_put(o, o->slot[i].sid1 - 1, o->slot[i].key, o->slot[i].cnt);
	pfree(o->slot);
	o->slot = NULL;
	ex->n_osa_spilled++;
}

static void
osa_grow(AggExec *ex, Osa *o)
{
	OsaSlot    *old = o->slot;
	uint64		ocap = o->cap;

	o->cap *= 2;
	o->slot = MemoryContextAllocZero(ex->tcxt, o->cap * sizeof(OsaSlot));
	for (uint64 i = 0; i < ocap; i++)
		if (old[i].sid1)
		{
			uint64		h = osa_hash(old[i].sid1 - 1, old[i].key) & (o->cap - 1);

			while (o->slot[h].sid1)
				h = (h + 1) & (o->cap - 1);
			o->slot[h] = old[i];
		}
	pfree(old);
}

/* one more of the value (sid, key), whose slot chain starts at h (osa_hash & (cap - 1) of the table as it is now) */
static void
osa_add(AggExec *ex, Osa *o, uint32 sid, uint64 key, uint64 h)
{
	if (o->sort)
	{
		osa_put(o, sid, key, 1);
		return;
	}
	for (; o->slot[h].sid1; h = (h + 1) & (o->cap - 1))
		if (o->slot[h].sid1 == sid + 1 && o->slot[h].key == key && o->slot[h].cnt != UINT32_MAX)
		{
			o->slot[h].cnt++;
			return;
		}
	if (o->n >= o->maxn)
	{
		osa_spill(ex, o);
		osa_put(o, sid, key, 1);
		return;
	}
	o->slot[h] = (OsaSlot) {key, sid + 1, 1};
	if (++o->n * 2 > o->cap)
		osa_grow(ex, o);
}

/* cnt more of the value (sid, key): the counts of a partial table, a slot holds at most UINT32_MAX */
void
osa_add_n(AggExec *ex, Osa *o, uint32 sid, uint64 key, uint64 cnt)
{
	while (cnt > 0)
	{
		uint64		h;
		uint32		take;

		if (o->sort)
		{
			osa_put(o, sid, key, cnt);
			return;
		}
		h = osa_hash(sid, key) & (o->cap - 1);
		for (; o->slot[h].sid1; h = (h + 1) & (o->cap - 1))
			if (o->slot[h].sid1 == sid + 1 && o->slot[h].key == key && o->slot[h].cnt != UINT32_MAX)
				break;
		take = (uint32) Min(cnt, (uint64) UINT32_MAX - (o->slot[h].sid1 ? o->slot[h].cnt : 0));
		if (o->slot[h].sid1)
			o->slot[h].cnt += take;
		else if (o->n >= o->maxn)
		{
			osa_spill(ex, o);
			continue;
		}
		else
		{
			o->slot[h] = (OsaSlot) {key, sid + 1, take};
			if (++o->n * 2 > o->cap)
				osa_grow(ex, o);
		}
		cnt -= take;
	}
}

/* the argument of ordered-set aggregate ai for the nsel tuples, in double */
static const double *
osa_values(AggExec *ex, SiftAggAgg *a, const uint32 *idx, int nsel)
{
	const SiftSrcVec *v;

	if (a->nvx)
		return eval_vx(ex, a, nsel);
	v = get_vec(ex, a->leaf);
	if (ex->lay->leaf[a->leaf].vtype == SCV_F64)
	{
		sift_colk->for_i64_sel[SIFT_COLW_64](v->data, idx, nsel, 0, (int64 *) ex->c64);
		return (const double *) ex->c64;
	}
	sift_colk->gather_u32((const uint32 *) v->data, idx, nsel, ex->cu32);
	for (int j = 0; j < nsel; j++)
		ex->vstk[j] = ((const int32 *) ex->cu32)[j];
	return ex->vstk;
}

/* the argument of an osa_int_key aggregate for the nsel tuples, as order keys */
static const uint64 *
osa_int_keys(AggExec *ex, SiftAggAgg *a, const uint32 *idx, int nsel, const uint64 *ok)
{
	uint64	   *key = (uint64 *) ex->c64;

	if (a->num.n)
	{
		const sift_i128 *nx = eval_nx(ex, &a->num, nsel, ex->tidx, ex->ridx, NULL, ok);

		for (int j = 0; j < nsel; j++)
			key[j] = nx[j] == SIFT_NX_NAN ? UINT64_MAX : (uint64) (int64) nx[j] ^ OSA_SIGN;	/* a NaN sorts last; 18 digits never reach the key of INT64_MAX */
		return key;
	}
	sift_colk->for_i64_sel[SIFT_COLW_64](get_vec(ex, a->leaf)->data, idx, nsel, 0, (int64 *) key);
	for (int j = 0; j < nsel; j++)
		key[j] ^= OSA_SIGN;
	return key;
}

/* adds the selected values to the values of the aggregate, which owns them (the other aggregates on the argument share them) */
void
osa_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *own = &ex->acc[ai];
	Osa		   *o = ex->osa[ai];
	const uint32 *idx = NULL;
	const double *x = NULL;
	const uint64 *ik = NULL;

	if (a->leaf >= 0)
		gather_idx(ex, a->leaf, nsel, &idx);
	if (osa_int_key(a))
		ik = osa_int_keys(ex, a, idx, nsel, ok);
	else
		x = osa_values(ex, a, idx, nsel);
	/* a block of values at a time: the slots of the whole block are requested before the first is probed (a histogram of 10^5 pairs is out of the cache) */
	for (int b = 0; b < nsel; b += OSA_BLOCK)
	{
		int			n = Min(OSA_BLOCK, nsel - b),
					m = 0;
		uint32		sid[OSA_BLOCK];
		uint64		key[OSA_BLOCK],
					h[OSA_BLOCK],
					cap = o->cap;

		for (int k = 0; k < n; k++)
		{
			uint32		g = ex->gid[b + k];

			if (ok && !sift_colsrc_bit(ok, b + k))
				continue;
			if (own->i[g] == 0)
				own->i[g] = ++o->nsid;
			sid[m] = own->i[g] - 1;
			key[m] = ik ? ik[b + k] : f8_key(x[b + k]);
			h[m] = osa_hash(sid[m], key[m]) & (cap - 1);
			if (o->slot)
				__builtin_prefetch(&o->slot[h[m]]);
			m++;
		}
		for (int k = 0; k < m; k++)
			osa_add(ex, o, sid[k], key[k], o->cap == cap && o->slot ? h[k] : osa_hash(sid[k], key[k]) & (o->cap - 1));
	}
}

static int
osa_cmp(const void *pa, const void *pb)
{
	const OsaSlot *a = pa,
			   *b = pb;

	if (a->sid1 != b->sid1)
		return SIFT_CMP3(a->sid1, b->sid1);
	return SIFT_CMP3(a->key, b->key);
}

/* the runs of equal values, by group id then value */
void
osa_sort(Osa *o)
{
	uint64		w = 0;

	if (o->sort)
	{
		tuplesort_performsort(o->sort);
		return;
	}
	for (uint64 i = 0; i < o->cap; i++)
		if (o->slot[i].sid1)
			o->slot[w++] = o->slot[i];
	qsort(o->slot, w, sizeof(OsaSlot), osa_cmp);
	o->n = w;
	o->next = 0;
}

bool
osa_next(Osa *o, OsaRun *r)
{
	bool		isnull;

	if (o->sort)
	{
		if (!tuplesort_gettupleslot(o->sort, true, false, o->get, NULL))
			return false;
		r->sid = DatumGetInt32(slot_getattr(o->get, 1, &isnull));
		r->key = (uint64) DatumGetInt64(slot_getattr(o->get, 2, &isnull)) ^ OSA_SIGN;
		r->cnt = 1;
		return true;
	}
	if (o->next >= o->n)
		return false;
	r->sid = o->slot[o->next].sid1 - 1;
	r->key = o->slot[o->next].key;
	r->cnt = o->slot[o->next].cnt;
	o->next++;
	return true;
}

/* where an aggregate's one or two values sit in the ascending values of a group of n, as the final functions of PostgreSQL pick them */
typedef struct OsaPick
{
	int64		pos[2];
	uint64		key[2];
	double		prop;
} OsaPick;

static void
osa_pick(const SiftAggAgg *a, int64 n, OsaPick *p)
{
	int64		lo,
				hi;

	if (osa_is_mode(a))
	{
		/* no rank: the walk of the runs (osa_finish) or the pass over the slots (osa_finish_select) finds the value */
		p->pos[0] = p->pos[1] = -1;
		p->key[0] = p->key[1] = 0;
		p->prop = 0;
		return;
	}
	if (a->kind == SAA_PCT_DISC)
	{
		lo = hi = Max((int64) ceil(a->frac * n), 1) - 1;
		p->prop = 0;
		if (a->desc_order)
			lo = hi = n - 1 - lo;
	}
	else
	{
		double		at = a->frac * (n - 1);

		lo = (int64) floor(at);
		hi = (int64) ceil(at);
		p->prop = at - lo;
		if (a->desc_order)
		{
			lo = n - 1 - lo;
			hi = n - 1 - hi;
		}
	}
	p->pos[0] = lo;
	p->pos[1] = hi;
}

static OsaVal
osa_result(const SiftAggAgg *a, const OsaPick *p)
{
	OsaVal		r;
	double		first,
				second;

	if (osa_int_key(a))
	{
		r.i = (int64) (p->key[0] ^ OSA_SIGN);
		return r;
	}
	first = key_f8(p->key[0]);
	second = key_f8(p->key[1]);
	r.d = a->kind == SAA_PCT_DISC || p->pos[0] == p->pos[1] ? first : first + (p->prop * (second - first));
	return r;
}

/*
 * The keys at the positions p[0..nt) (0-based, ascending) of the values of a group, from its histogram slots s[lo..hi) in any order, whose first value is at
 * position base: a three-way quickselect that moves the counts with the slots and sends each position to the side it lies in, so that the two positions a
 * percentile picks (and the positions of several percentiles) share the passes until they part. A sort of the slots takes a log factor more.
 */
static void
osa_select(OsaSlot *s, int64 lo, int64 hi, uint64 base, const uint64 *p, uint64 *out, int nt)
{
	while (nt > 0)
	{
		int64		n = hi - lo,
					lt = lo,
					i = lo,
					gt = hi;
		uint64		a = s[lo].key,
					b = s[lo + n / 2].key,
					c = s[hi - 1].key,
					pivot = a < b ? (b < c ? b : (a < c ? c : a)) : (a < c ? a : (b < c ? c : b)),
					less = 0,
					equal = 0;
		int			nl = 0,
					ne;

		if (n <= 16)
		{
			uint64		upto = base;

			for (int64 x = lo + 1; x < hi; x++)
				for (int64 y = x; y > lo && s[y - 1].key > s[y].key; y--)
				{
					OsaSlot		t = s[y];

					s[y] = s[y - 1];
					s[y - 1] = t;
				}
			for (int64 x = lo, k = 0; k < nt; x++)
			{
				SIFT_INVARIANT(x < hi, "sift: ordered-set position %llu is past the values of its group", (unsigned long long) p[k]);
				upto += s[x].cnt;
				for (; k < nt && p[k] < upto; k++)
					out[k] = s[x].key;
			}
			return;
		}
		while (i < gt)
		{
			if (s[i].key < pivot)
			{
				less += s[i].cnt;
				if (i != lt)
				{
					OsaSlot		t = s[lt];

					s[lt] = s[i];
					s[i] = t;
				}
				lt++;
				i++;
			}
			else if (s[i].key > pivot)
			{
				OsaSlot		t = s[--gt];

				s[gt] = s[i];
				s[i] = t;
			}
			else
			{
				equal += s[i].cnt;
				i++;
			}
		}
		while (nl < nt && p[nl] < base + less)
			nl++;
		ne = nl;
		while (ne < nt && p[ne] < base + less + equal)
			out[ne++] = pivot;
		osa_select(s, lo, lt, base, p, out, nl);
		/* the positions right of the pivot's slots go on in the same loop */
		p += ne;
		out += ne;
		nt -= ne;
		base += less + equal;
		lo = gt;
	}
}

/*
 * The values of the aggregates that share the histogram, without sorting it: the slots are split by group (a counting pass), and the positions the
 * aggregates pick in a group are found together by osa_select. oval is filled as the walk of the sorted runs fills it.
 */
static void
osa_finish_select(AggExec *ex, int owner, const int64 *nrows)
{
	SiftAggSpec *s = ex->spec;
	Osa		   *o = ex->osa[owner];
	uint64		w = 0;
	uint32	   *start = palloc0((o->nsid + 1) * sizeof(uint32));
	uint64	   *pos = palloc(2 * s->naggs * sizeof(uint64)),
			   *key = palloc(2 * s->naggs * sizeof(uint64));
	OsaPick    *pick = palloc(s->naggs * sizeof(OsaPick));
	OsaSlot    *by;

	for (uint64 i = 0; i < o->cap; i++)
		if (o->slot[i].sid1)
			start[o->slot[i].sid1 - 1 + 1]++, w++;
	for (uint32 g = 0; g < o->nsid; g++)
		start[g + 1] += start[g];
	by = palloc(Max(w, 1) * sizeof(OsaSlot));
	{
		uint32	   *at = palloc((o->nsid + 1) * sizeof(uint32));

		memcpy(at, start, (o->nsid + 1) * sizeof(uint32));
		for (uint64 i = 0; i < o->cap; i++)
			if (o->slot[i].sid1)
				by[at[o->slot[i].sid1 - 1]++] = o->slot[i];
		pfree(at);
	}
	for (uint32 sid = 0; sid < o->nsid; sid++)
	{
		OsaSlot    *sl = by + start[sid];
		int64		m = start[sid + 1] - start[sid],
					total = 0;
		int			nt = 0;

		for (int64 i = 0; i < m; i++)
			total += sl[i].cnt;
		SIFT_INVARIANT(total == nrows[sid], "sift: ordered-set values of a group are %lld, expected %lld", (long long) total, (long long) nrows[sid]);
		for (int ai = 0; ai < s->naggs; ai++)
			if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
			{
				osa_pick(&s->agg[ai], nrows[sid], &pick[ai]);
				if (osa_is_mode(&s->agg[ai]))
				{
					/* the most frequent value; on a tie the first in the sort order: the smallest key, the largest for DESC */
					int64		best = 0;

					for (int64 i = 1; i < m; i++)
						if (sl[i].cnt > sl[best].cnt || (sl[i].cnt == sl[best].cnt && (s->agg[ai].desc_order ? sl[i].key > sl[best].key : sl[i].key < sl[best].key)))
							best = i;
					pick[ai].key[0] = pick[ai].key[1] = sl[best].key;
					continue;
				}
				pos[nt++] = (uint64) pick[ai].pos[0];
				pos[nt++] = (uint64) pick[ai].pos[1];
			}
		sift_sort_u64(pos, nt);
		osa_select(sl, 0, m, 0, pos, key, nt);
		for (int ai = 0; ai < s->naggs; ai++)
			if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner && !osa_is_mode(&s->agg[ai]))
				for (int k = 0; k < 2; k++)
				{
					int			at = 0;

					while (pos[at] != (uint64) pick[ai].pos[k])
						at++;
					pick[ai].key[k] = key[at];
				}
		for (int ai = 0; ai < s->naggs; ai++)
			if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
				ex->oval[ai][sid] = osa_result(&s->agg[ai], &pick[ai]);
	}
	pfree(by);
	pfree(start);
	pfree(pos);
	pfree(key);
	pfree(pick);
}

/* the walk of the sorted values of a mode(): the run of one value just ended, and the best so far (the longest, a run as long wins for DESC as the sort puts the largest first) */
typedef struct OsaModeRun
{
	uint64		key;
	uint64		cnt;
	uint64		best;
} OsaModeRun;

static void
mode_end_run(const SiftAggAgg *a, OsaModeRun *m, OsaPick *p)
{
	if (m->cnt > m->best || (m->cnt > 0 && m->cnt == m->best && a->desc_order))
	{
		m->best = m->cnt;
		p->key[0] = p->key[1] = m->key;
	}
	m->cnt = 0;
}

/* walks the sorted values of the aggregate that owns them once, taking the values of every aggregate on them */
static void
osa_finish(AggExec *ex, int owner)
{
	SiftAggSpec *s = ex->spec;
	Osa		   *o = ex->osa[owner];
	AggAcc	   *own = &ex->acc[owner];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap,
				cur = UINT32_MAX;
	int64	   *nrows = palloc0(Max(o->nsid, 1) * sizeof(int64));
	OsaPick    *pick = palloc0(s->naggs * sizeof(OsaPick));
	OsaRun		r;
	int64		pos = 0;
	OsaModeRun *mr = palloc0(s->naggs * sizeof(OsaModeRun));

	for (uint32 g = 0; g < ngr; g++)
		if (own->i[g])
			nrows[own->i[g] - 1] = own->n[g];
	for (int ai = 0; ai < s->naggs; ai++)
		if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
			ex->oval[ai] = MemoryContextAlloc(ex->tcxt, Max(o->nsid, 1) * sizeof(OsaVal));
	if (o->slot)
	{
		osa_finish_select(ex, owner, nrows);
		return;
	}
	osa_sort(o);
	for (bool more = osa_next(o, &r);; more = osa_next(o, &r))
	{
		if (!more || r.sid != cur)
		{
			for (int ai = 0; cur != UINT32_MAX && ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
				{
					if (osa_is_mode(&s->agg[ai]))
						mode_end_run(&s->agg[ai], &mr[ai], &pick[ai]);
					ex->oval[ai][cur] = osa_result(&s->agg[ai], &pick[ai]);
				}
			SIFT_INVARIANT(cur == UINT32_MAX || pos == nrows[cur], "sift: ordered-set values of a group are %lld, expected %lld", (long long) pos,
						   (long long) (cur == UINT32_MAX ? 0 : nrows[cur]));
			if (!more)
				break;
			cur = r.sid;
			pos = 0;
			for (int ai = 0; ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
				{
					osa_pick(&s->agg[ai], nrows[cur], &pick[ai]);
					memset(&mr[ai], 0, sizeof(OsaModeRun));
				}
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if (osa_is_mode(&s->agg[ai]) && s->agg[ai].share == owner)
			{
				if (mr[ai].cnt > 0 && mr[ai].key == r.key)
					mr[ai].cnt += r.cnt;
				else
				{
					mode_end_run(&s->agg[ai], &mr[ai], &pick[ai]);
					mr[ai].key = r.key;
					mr[ai].cnt = r.cnt;
				}
			}
			else if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
				for (int k = 0; k < 2; k++)
					if (pick[ai].pos[k] >= pos && pick[ai].pos[k] < pos + (int64) r.cnt)
						pick[ai].key[k] = r.key;
		pos += r.cnt;
	}
	if (o->sort)
	{
		tuplesort_end(o->sort);
		o->sort = NULL;
	}
}

void
osa_finish_all(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		if (is_ordered_agg(&ex->spec->agg[ai]) && ex->spec->agg[ai].share == ai)
			osa_finish(ex, ai);
}
