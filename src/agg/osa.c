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

/* ---------- ordered-set aggregates ---------- */

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
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	AttrNumber	att[2] = {1, 2};
	Oid			ops[2] = {Int4LessOperator, Int8LessOperator},
				coll[2] = {InvalidOid, InvalidOid};
	bool		nulls[2] = {false, false};

	TupleDescInitEntry(desc, 1, "sid", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "key", INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	o->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	o->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	o->sort = tuplesort_begin_heap(desc, 2, att, ops, coll, nulls, work_mem, NULL, TUPLESORT_NONE);
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

/* adds the selected values to the values of the aggregate, which owns them (the other aggregates on the argument share them) */
void
osa_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *own = &ex->acc[ai];
	Osa		   *o = ex->osa[ai];
	const uint32 *idx = NULL;
	const double *x;

	if (a->leaf >= 0)
		gather_idx(ex, a->leaf, nsel, &idx);
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
			key[m] = f8_key(x[b + k]);
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
		return a->sid1 < b->sid1 ? -1 : 1;
	return a->key < b->key ? -1 : a->key > b->key;
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

static double
osa_result(const SiftAggAgg *a, const OsaPick *p)
{
	double		first = key_f8(p->key[0]),
				second = key_f8(p->key[1]);

	if (a->kind == SAA_PCT_DISC || p->pos[0] == p->pos[1])
		return first;
	return first + (p->prop * (second - first));
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

	for (uint32 g = 0; g < ngr; g++)
		if (own->i[g])
			nrows[own->i[g] - 1] = own->n[g];
	for (int ai = 0; ai < s->naggs; ai++)
		if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
			ex->oval[ai] = MemoryContextAlloc(ex->tcxt, Max(o->nsid, 1) * sizeof(double));
	osa_sort(o);
	for (bool more = osa_next(o, &r);; more = osa_next(o, &r))
	{
		if (!more || r.sid != cur)
		{
			for (int ai = 0; cur != UINT32_MAX && ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
					ex->oval[ai][cur] = osa_result(&s->agg[ai], &pick[ai]);
			if (cur != UINT32_MAX && pos != nrows[cur])
				elog(ERROR, "sift: ordered-set values of a group are %lld, expected %lld", (long long) pos, (long long) nrows[cur]);
			if (!more)
				break;
			cur = r.sid;
			pos = 0;
			for (int ai = 0; ai < s->naggs; ai++)
				if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
					osa_pick(&s->agg[ai], nrows[cur], &pick[ai]);
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if (is_ordered_agg(&s->agg[ai]) && s->agg[ai].share == owner)
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
