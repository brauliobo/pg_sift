/*
 * agg/group.c - the group table and the group ids of the tuples
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
#include "agg/acc_layout.h"
#include "agg/agg_int.h"

/* ---- combos and lookup joins: the executor state of SiftAggCombo ---- */

/* The group of a float: its bits, with the one zero and the one NaN of float8_eq / hashfloat8 (-0 = 0, NaN = NaN) */
static int64
float_key(double d)
{
	int64		bits;

	d = d == 0 ? 0.0 : isnan(d) ? get_float8_nan() : d;
	memcpy(&bits, &d, sizeof(bits));
	return bits;
}

/* tuples ahead whose slot of a group or value table is prefetched: the tables outgrow the cache at millions of groups */
#define GROUP_AHEAD 16

AggAcc *
acc_alloc(AggExec *ex, uint32 cap)
{
	AggAcc	   *a = MemoryContextAllocZero(ex->tcxt, Max(ex->spec->naggs, 1) * sizeof(AggAcc));

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		unsigned	layout = sift_agg_acc_layout(&ex->spec->agg[i], ex->lane_serial);

		a[i].n = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
		if (layout & ACCL_I)
			a[i].i = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));	/* SAA_GEN: the group's number among the sorted values */
		if (layout & ACCL_W)
			a[i].w = MemoryContextAllocZero(ex->tcxt, cap * sizeof(sift_i128));
		if (layout & ACCL_D)
			a[i].d = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
		if (layout & ACCL_DT)
			a[i].dt = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
		if (layout & ACCL_NF)
			a[i].nf = MemoryContextAllocZero(ex->tcxt, cap);
		if (layout & ACCL_V)
			a[i].v = MemoryContextAllocZero(ex->tcxt, cap * sizeof(Datum));
		if (layout & ACCL_R)
			a[i].r = MemoryContextAllocZero(ex->tcxt, cap * sizeof(Datum));
		if (layout & ACCL_LAST)
		{
			a[i].last = MemoryContextAlloc(ex->tcxt, cap * sizeof(uint64));
			memset(a[i].last, 0xff, cap * sizeof(uint64));
		}
		if (ex->lanes && sift_aggkind_float_sum(&ex->spec->agg[i]))
			a[i].lv = MemoryContextAllocZero(ex->tcxt, SIFT_AGG_LANES * sizeof(double *));
	}
	return a;
}

static void
acc_free(AggAcc *a, int naggs)
{
	for (int i = 0; i < naggs; i++)
	{
		void	   *col[] = {a[i].n, a[i].i, a[i].w, a[i].d, a[i].v, a[i].r, a[i].last, a[i].nf, a[i].dt, a[i].lv};

		for (int l = 0; a[i].lv && l < SIFT_AGG_LANES; l++)
			if (a[i].lv[l])
				pfree(a[i].lv[l]);
		for (int c = 0; c < lengthof(col); c++)
			if (col[c])
				pfree(col[c]);
	}
	pfree(a);
}

/* every per-group array to newcap groups; remap[old] = new index (UINT32_MAX: drop), NULL: identity */
void
groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	AggAcc	   *na = acc_alloc(ex, newcap);
	int64	   *nseen = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));

	/* the groups keep their numbers: copy the arrays whole, an unseen slot holds what a new one does */
	if (remap == NULL)
	{
		memcpy(nseen, ex->seen, oldcap * sizeof(int64));
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			AggAcc	   *o = &ex->acc[i],
					   *n = &na[i];

			memcpy(n->n, o->n, oldcap * sizeof(int64));
			if (o->i)
				memcpy(n->i, o->i, oldcap * sizeof(int64));
			if (o->w)
				memcpy(n->w, o->w, oldcap * sizeof(sift_i128));
			if (o->d)
				memcpy(n->d, o->d, oldcap * sizeof(double));
			if (o->dt)
				memcpy(n->dt, o->dt, oldcap * sizeof(double));
			if (o->last)
				memcpy(n->last, o->last, oldcap * sizeof(uint64));
			if (o->nf)
				memcpy(n->nf, o->nf, oldcap * sizeof(uint8));
			if (o->v)
				memcpy(n->v, o->v, oldcap * sizeof(Datum));
			if (o->r)
				memcpy(n->r, o->r, oldcap * sizeof(Datum));
			for (int l = 0; o->lv && l < SIFT_AGG_LANES; l++)
				if (o->lv[l])
				{
					n->lv[l] = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
					memcpy(n->lv[l], o->lv[l], oldcap * sizeof(double));
				}
		}
	}
	for (uint32 g = 0; remap && g < oldcap; g++)
	{
		uint32		t = remap ? remap[g] : g;

		if (t == UINT32_MAX || ex->seen[g] == 0)
			continue;
		nseen[t] = ex->seen[g];
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			AggAcc	   *o = &ex->acc[i],
					   *n = &na[i];

			n->n[t] = o->n[g];
			if (o->i)
				n->i[t] = o->i[g];
			if (o->w)
				n->w[t] = o->w[g];
			if (o->d)
				n->d[t] = o->d[g];
			if (ex->spec->agg[i].kind == SAA_GEN)
			{
				n->v[t] = o->v[g];
				n->r[t] = o->r[g];
			}
			if (o->dt)
				n->dt[t] = o->dt[g];
			for (int l = 0; o->lv && l < SIFT_AGG_LANES; l++)
				if (o->lv[l])
				{
					if (n->lv[l] == NULL)
						n->lv[l] = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
					n->lv[l][t] = o->lv[l][g];
				}
			if (o->last)
				n->last[t] = o->last[g];
			if (o->nf)
				n->nf[t] = o->nf[g];
			if (o->v)
				n->v[t] = o->v[g];
		}
	}
	if (remap)
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			if (ex->dset[i].slot)
				dset_grow(ex, &ex->dset[i], remap);
			if (ex->dist && ex->dist[i])
				dist_regroup(ex, ex->dist[i], remap, oldcap, newcap);
		}
	arms_resize(ex, newcap, remap, oldcap);
	acc_free(ex->acc, ex->spec->naggs);
	pfree(ex->seen);
	ex->gen++;
	ex->seen = nseen;
	ex->acc = na;
	ex->gcap = newcap;
}

void
direct_layout(AggExec *ex)
{
	uint64		m = 1;

	for (int j = 0; j < ex->spec->nkeys; j++)
	{
		ex->ks[j].mult = (uint32) m;
		m *= ex->ks[j].cap;
	}
}

/* a new group of an id tuple in hash mode, in slot `pos` of its probe */
static pg_noinline uint32
hash_add(AggExec *ex, const uint32 *ids, uint32 pos)
{
	int			nk = ex->spec->nkeys;
	uint32		g = ex->ngroups++;

	if (g >= ex->hkcap)
	{
		ex->hkcap = Max(ex->hkcap * 2, 1024);
		ex->hkey = ex->hkey ? repalloc(ex->hkey, (Size) ex->hkcap * nk * sizeof(uint32)) :
			MemoryContextAlloc(ex->tcxt, (Size) ex->hkcap * nk * sizeof(uint32));
	}
	memcpy(&ex->hkey[(Size) g * nk], ids, nk * sizeof(uint32));
	ex->hslot[pos] = g + 1;
	if ((uint64) ex->ngroups * 2 > ex->nhslot)
	{
		uint32		nn = ex->nhslot * 2;
		uint32	   *ns = MemoryContextAllocZero(ex->tcxt, nn * sizeof(uint32));

		for (uint32 x = 0; x < ex->ngroups; x++)
		{
			uint32		p = ids_hash(&ex->hkey[(Size) x * nk], nk, nn);

			while (ns[p])
				p = (p + 1) & (nn - 1);
			ns[p] = x + 1;
		}
		pfree(ex->hslot);
		ex->hslot = ns;
		ex->nhslot = nn;
	}
	return g;
}

/* the probe for `nk` ids, a constant where it is inlined: the hash and the comparison unroll */
static pg_attribute_always_inline uint32
hash_probe(AggExec *ex, const uint32 *ids, const int nk, uint32 h)
{
	uint32		pos = h & (ex->nhslot - 1);

	for (; ex->hslot[pos]; pos = (pos + 1) & (ex->nhslot - 1))
		if (memcmp(&ex->hkey[(Size) (ex->hslot[pos] - 1) * nk], ids, nk * sizeof(uint32)) == 0)
			return ex->hslot[pos] - 1;
	return hash_add(ex, ids, pos);
}

/* one probe function per number of keys (`ex->hashg`): the hash and the comparison of the ids are unrolled, as the hot loop wants */
#define HASH_GROUP_FN(n) \
	static uint32 hash_group_h##n(AggExec *ex, const uint32 *ids, uint32 h) { return hash_probe(ex, ids, n, h); } \
	static uint32 hash_group_##n(AggExec *ex, const uint32 *ids) { return hash_group_h##n(ex, ids, ids_hash32(ids, n)); }
HASH_GROUP_FN(1)
HASH_GROUP_FN(2)
HASH_GROUP_FN(3)
HASH_GROUP_FN(4)
HASH_GROUP_FN(5)
HASH_GROUP_FN(6)
HASH_GROUP_FN(7)
HASH_GROUP_FN(8)
HASH_GROUP_FN(9)
HASH_GROUP_FN(10)
HASH_GROUP_FN(11)
HASH_GROUP_FN(12)
HASH_GROUP_FN(13)
HASH_GROUP_FN(14)
HASH_GROUP_FN(15)
HASH_GROUP_FN(16)

static uint32 (*const hash_group_fns[]) (AggExec *, const uint32 *) =
{hash_group_1, hash_group_2, hash_group_3, hash_group_4, hash_group_5, hash_group_6, hash_group_7, hash_group_8,
 hash_group_9, hash_group_10, hash_group_11, hash_group_12, hash_group_13, hash_group_14, hash_group_15, hash_group_16};
static uint32 (*const hash_group_hfns[]) (AggExec *, const uint32 *, uint32) =
{hash_group_h1, hash_group_h2, hash_group_h3, hash_group_h4, hash_group_h5, hash_group_h6, hash_group_h7, hash_group_h8,
 hash_group_h9, hash_group_h10, hash_group_h11, hash_group_h12, hash_group_h13, hash_group_h14, hash_group_h15, hash_group_h16};

/* Wider tuples use the same hash and equality with the plan's actual width. */
static uint32
hash_group_generic_h(AggExec *ex, const uint32 *ids, uint32 h)
{
	return hash_probe(ex, ids, ex->spec->nkeys, h);
}

static uint32
hash_group_generic(AggExec *ex, const uint32 *ids)
{
	return hash_group_generic_h(ex, ids, ids_hash32(ids, ex->spec->nkeys));
}

void
hash_init(AggExec *ex, uint32 groups)
{
	int         nk = ex->spec->nkeys;

	ex->hash = true;
	ex->hashg = nk <= lengthof(hash_group_fns) ? hash_group_fns[nk - 1] : hash_group_generic;
	ex->hashgh = nk <= lengthof(hash_group_hfns) ? hash_group_hfns[nk - 1] : hash_group_generic_h;
	ex->nhslot = groups > 1024 ? pg_nextpower2_32(groups * 2) : 1024;
	ex->hslot = MemoryContextAllocZero(ex->tcxt, ex->nhslot * sizeof(uint32));
	ex->ngroups = 0;
	if (groups > 1024)
	{
		ex->hkcap = groups;
		ex->hkey = MemoryContextAlloc(ex->tcxt, (Size) groups * nk * sizeof(uint32));
	}
}

/* the arrays of `total` slots are far more than the groups the planner expects: the capacity of the keys multiplies, the groups do not */
bool
direct_oversized(AggExec *ex, uint64 total)
{
	return total > PRESIZE_MIN && total > ex->spec->est_groups * 4;
}

/* a key outgrew its capacity: more room per key, or the hash table when the arrays would be too big */
void
direct_grow(AggExec *ex)
{
	int			nk = ex->spec->nkeys;
	uint32		oldcap = ex->gcap,
				oldcaps[SIFT_AGG_MAX_KEYS],
				oldmult[SIFT_AGG_MAX_KEYS];
	uint64		total = 1;
	uint32	   *remap;

	for (int j = 0; j < nk; j++)
	{
		KeyState   *k = &ex->ks[j];

		oldcaps[j] = k->cap;
		oldmult[j] = k->mult;
		while (k->cap < key_used(k))
			k->cap *= 2;
		total = Min((uint64) ex->direct_max + 1, total * k->cap);
	}
	remap = palloc(oldcap * sizeof(uint32));
	if (total <= ex->direct_max && direct_oversized(ex, total))
	{
		uint32		used = 0;

		for (uint32 g = 0; g < oldcap; g++)
			used += ex->seen[g] != 0;
		if ((uint64) used * DIRECT_SPARSE < total)
			total = (uint64) ex->direct_max + 1;
	}
	if (total > ex->direct_max)
	{
		/* the existing groups keep their ids in the hash table */
		hash_init(ex, 1024);
		for (uint32 g = 0; g < oldcap; g++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			if (ex->seen[g] == 0)
			{
				remap[g] = UINT32_MAX;
				continue;
			}
			for (int j = 0; j < nk; j++)
				ids[j] = (g / oldmult[j]) % oldcaps[j];
			remap[g] = hash_group(ex, ids);
		}
		groups_resize(ex, Max(ex->ngroups * 2, 1024), remap, oldcap);
	}
	else
	{
		bool		same = true;

		direct_layout(ex);
		/* only the last key grew: every group keeps its number */
		for (int j = 0; j < nk - 1; j++)
			same &= ex->ks[j].cap == oldcaps[j];
		for (uint32 g = 0; !same && g < oldcap; g++)
		{
			uint64		t = 0;

			for (int j = 0; j < nk; j++)
				t += (uint64) ((g / oldmult[j]) % oldcaps[j]) * ex->ks[j].mult;
			remap[g] = (uint32) t;
		}
		groups_resize(ex, (uint32) total, same ? NULL : remap, oldcap);
	}
	pfree(remap);
}

/* the next id of an ident key, for the value v */
uint32
ident_add(AggExec *ex, KeyState *k, int64 v)
{
	if (k->nident == k->capident)
	{
		k->capident = Max(k->capident * 2, 64);
		k->idval = k->idval ? repalloc(k->idval, k->capident * sizeof(int64)) :
			MemoryContextAlloc(ex->tcxt, k->capident * sizeof(int64));
	}
	k->idval[k->nident++] = v;
	return k->nident;
}


/* the top bits of the product: a float with a short mantissa (1.5, 100.0) has nothing in its low 40 bits, and the middle bits of the product have nothing either */
static inline uint32
wide_hash(int64 v, uint32 cap)
{
	return (uint32) (((uint64) v * UINT64CONST(0x9E3779B97F4A7C15)) >> (64 - __builtin_ctz(cap)));
}

static uint32 bucket_id(AggExec *ex, KeyState *k, int64 bucket);
static uint32 cal_id(AggExec *ex, KeyState *k, int64 day);

/*
 * The id of an ident key's value when the range table is not used: ids are given in order of appearance, as the table does. A
 * bucket key (a calendar expression of a timestamp) keeps the id of the value of its expression for each bucket number it sees, a calendar key
 * of a date for each day.
 */
uint32
ident_wide_id(AggExec *ex, KeyState *k, int64 v)
{
	uint32		h = wide_hash(v, k->wcap);
	uint32		id;

	while (k->wid[h])
	{
		if (k->wval[h] == v)
			return k->wid[h];
		h = (h + 1) & (k->wcap - 1);
	}
	k->wval[h] = v;
	id = k->bucket ? bucket_id(ex, k, v) : k->cal ? cal_id(ex, k, v) : k->ident ? ident_add(ex, k, v) : key_value_id(ex, (int) (k - ex->ks), leaf_datum(k->param->paramtype, v), false);
	k->wid[h] = id;
	if ((uint64) ++k->nwide * 2 > k->wcap)
	{
		uint32		ocap = k->wcap;
		int64	   *ov = k->wval;
		uint32	   *oi = k->wid;

		k->wcap *= 2;
		k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
		k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
		for (uint32 s = 0; s < ocap; s++)
			if (oi[s])
			{
				uint32		p = wide_hash(ov[s], k->wcap);

				while (k->wid[p])
					p = (p + 1) & (k->wcap - 1);
				k->wval[p] = ov[s];
				k->wid[p] = oi[s];
			}
		pfree(ov);
		pfree(oi);
	}
	return id;
}

/* from now on the ident key's ids come from the hash table; the values the range table gave ids to are in it */
void
ident_go_wide(AggExec *ex, KeyState *k)
{
	uint32		n = k->nident;

	/* a table after a spill starts with room for the keys of the one written, as its group arrays do (spill_keep) */
	k->wcap = 1024;
	while (k->wcap < 4 * Max(n, spill_keep(ex)))
		k->wcap *= 2;
	k->wval = MemoryContextAlloc(ex->tcxt, k->wcap * sizeof(int64));
	k->wid = MemoryContextAllocZero(ex->tcxt, k->wcap * sizeof(uint32));
	k->nident = 0;				/* ident_wide_id gives the same ids back, in the same order */
	k->nwide = 0;
	for (uint32 i = 0; i < n; i++)
		(void) ident_wide_id(ex, k, k->idval[i]);
}

void
key_range_fill(void *arg, int32 v, void *entry)
{
	KeyFill    *f = arg;
	KeyState   *k = &f->ex->ks[f->ki];

	if (k->ident)
		*(uint32 *) entry = ident_add(f->ex, k, v);
	else if (k->bucket)
		*(uint32 *) entry = bucket_id(f->ex, k, v);
	else
		*(uint32 *) entry = key_value_id(f->ex, f->ki, leaf_datum(k->param->paramtype, v), false);
}

/* the id of a bucket of a timestamp key: the key's expression of the bucket's first microsecond, or an instant of a step of the zone's clock (an infinity is a bucket of its own) */
static uint32
bucket_id(AggExec *ex, KeyState *k, int64 bucket)
{
	int64		us = bucket == PG_INT64_MAX || bucket == PG_INT64_MIN ? bucket : k->zcut ? k->witness : k->zone ? zone_instant(k->zone, bucket, k->bucket) : bucket * k->bucket;

	return key_value_id(ex, (int) (k - ex->ks), Int64GetDatum(us), false);
}

/* the id of the cell of a date key that starts on a day: the key's expression of that date */
static uint32
cal_id(AggExec *ex, KeyState *k, int64 day)
{
	return key_value_id(ex, (int) (k - ex->ks), Int32GetDatum((int32) day), false);
}

/* the number of the bucket of a timestamp, floor(v / us) */
static int64
bucket_floor(int64 v, const int64 us)
{
	return v / us - (v % us < 0);
}

/*
 * The bucket numbers of the selected rows as int32 (a NULL row gets 0) and their range; false: an infinity or a range a table
 * cannot hold is among them. us is a constant at each call, so the division is an unsigned multiplication: adding a multiple of us
 * above 2^63 makes every timestamp positive, and the same multiple of buckets comes off the quotient. shift: the offset of the zone's clock (a valid timestamp
 * is days short of the limits of int64, so the sum cannot overflow).
 */
static pg_attribute_always_inline bool
bucket_numbers(const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, const uint64 us, const int64 shift, int32 *mn, int32 *mx)
{
	const int64 *data = v->data;
	const uint8 *isnull = v->isnull;
	const uint64 off = us * (((uint64) 1 << 63) / us + 1),
				off_buckets = off / us;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if (isnull && isnull[idx[i]])
		{
			out[i] = 0;
			continue;
		}
		lo = Min(lo, d);
		hi = Max(hi, d);
		out[i] = (uint32) (((uint64) (d + shift) + off) / us - off_buckets);
	}
	if (lo > hi)
	{
		*mn = *mx = 0;
		return true;
	}
	if (lo == PG_INT64_MIN || hi == PG_INT64_MAX)
		return false;
	lo = bucket_floor(lo + shift, us);
	hi = bucket_floor(hi + shift, us);
	*mn = (int32) lo;
	*mx = (int32) hi;
	return lo >= PG_INT32_MIN && hi <= PG_INT32_MAX && hi - lo < MAX_RANGE;
}

/* bucket_numbers of a step of the UTC clock: a second, a minute, an hour or a day */
static bool
bucket_numbers_utc(int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx)
{
	switch (us)
	{
		case USECS_PER_SEC:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC, 0, mn, mx);
		case USECS_PER_SEC * 60:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC * 60, 0, mn, mx);
		case USECS_PER_HOUR:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_HOUR, 0, mn, mx);
		case USECS_PER_DAY:
			return bucket_numbers(v, idx, nsel, out, USECS_PER_DAY, 0, mn, mx);
	}
	SIFT_INVARIANT(false, "sift: unexpected bucket of %lld microseconds", (long long) us);
	return false;
}

/* the zone's pieces cover the selected rows that are not an infinity or NULL (false: there are none) */
static bool
zone_rows(ZoneOff *z, const SiftSrcVec *v, const uint32 *idx, int nsel)
{
	const int64 *data = v->data;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if ((v->isnull && v->isnull[idx[i]]) || d == PG_INT64_MAX || d == PG_INT64_MIN)
			continue;
		lo = Min(lo, d);
		hi = Max(hi, d);
	}
	if (lo > hi)
		return false;
	zone_cover(z, lo, hi);
	return true;
}

/*
 * bucket_numbers for a key on the zone's clock: one piece of constant offset over the rows is a shift of the vector kernel, several are a lookup of the piece of each row.
 * False as bucket_numbers: an infinity or a range a table cannot hold is among the rows.
 */
static bool
bucket_numbers_zoned(ZoneOff *z, int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx)
{
	const int64 *data = v->data;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN,
				shift;
	int64		blo = PG_INT64_MAX,
				bhi = PG_INT64_MIN;
	bool		inf = false;

	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]];

		if (v->isnull && v->isnull[idx[i]])
			continue;
		inf |= d == PG_INT64_MAX || d == PG_INT64_MIN;
		if (!inf)
		{
			lo = Min(lo, d);
			hi = Max(hi, d);
		}
	}
	if (inf)
		return false;
	if (lo > hi)
	{
		for (int i = 0; i < nsel; i++)
			out[i] = 0;
		*mn = *mx = 0;
		return true;
	}
	zone_cover(z, lo, hi);
	if (zone_piece(z, lo) == zone_piece(z, hi))
	{
		shift = z->off[zone_piece(z, lo)];
		switch (us)
		{
			case USECS_PER_SEC:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC, shift, mn, mx);
			case USECS_PER_SEC * 60:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_SEC * 60, shift, mn, mx);
			case USECS_PER_HOUR:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_HOUR, shift, mn, mx);
			case USECS_PER_DAY:
				return bucket_numbers(v, idx, nsel, out, USECS_PER_DAY, shift, mn, mx);
			default:
				SIFT_INVARIANT(false, "sift: unexpected bucket of %lld microseconds", (long long) us);
		}
	}
	for (int i = 0; i < nsel; i++)
	{
		int64		b;

		if (v->isnull && v->isnull[idx[i]])
		{
			out[i] = 0;
			continue;
		}
		b = zone_bucket(z, data[idx[i]], us);
		blo = Min(blo, b);
		bhi = Max(bhi, b);
		out[i] = (uint32) b;
	}
	*mn = (int32) blo;
	*mx = (int32) bhi;
	return blo >= PG_INT32_MIN && bhi <= PG_INT32_MAX && bhi - blo < MAX_RANGE;
}

/* the bucket numbers of the n rows of v from `start` (iota: 0 .. n - 1), on the zone's clock or on UTC's, for the condition of a step (agg/pred_in.c); false as bucket_numbers */
bool
step_numbers(const SiftSrcVec *v, int start, int n, const uint32 *iota, ZoneOff *z, int64 us, uint32 *out, int32 *mn, int32 *mx)
{
	SiftSrcVec	w = *v;

	w.data = (const int64 *) v->data + start;
	if (v->isnull)
		w.isnull = v->isnull + start;
	return z ? bucket_numbers_zoned(z, us, &w, iota, n, out, mn, mx) : bucket_numbers_utc(us, &w, iota, n, out, mn, mx);
}

/* the key ids of the selected rows by the hash table of bucket numbers: infinities and wide ranges */
static void
bucket_hash_ids(AggExec *ex, KeyState *k, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *ids)
{
	const int64 *data = v->data;
	int64		last = 0;
	uint32		lastid = 0;

	if (k->zone)
		zone_rows(k->zone, v, idx, nsel);
	for (int i = 0; i < nsel; i++)
	{
		int64		d = data[idx[i]],
					b = d == PG_INT64_MAX || d == PG_INT64_MIN ? d : k->zcut ? zone_cut(k->zone, d, k->bucket) : k->zone ? zone_bucket(k->zone, d, k->bucket) : bucket_floor(d, k->bucket);

		if (v->isnull && v->isnull[idx[i]])
			ids[i] = 0;			/* every function of a bucket key is strict: the key of a NULL is NULL */
		else
		{
			if (b != last || lastid == 0)
			{
				k->witness = d;
				lastid = ident_wide_id(ex, k, b);
				last = b;
			}
			ids[i] = lastid;
		}
	}
}

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
static void
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
			for (int i = 0; i < nsel; i++)
			{
				if (i + GROUP_AHEAD < nsel)
				{
					uint32		h = wide_hash((int32) vals_out[i + GROUP_AHEAD], k->wcap);

					__builtin_prefetch(&k->wid[h]);
					__builtin_prefetch(&k->wval[h]);
				}
				vals_out[i] = (v->isnull && v->isnull[idx[i]]) ? 0 : ident_wide_id(ex, k, (int32) vals_out[i]);
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
static void
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

/* ids_hash32 of every tuple of the chunk's key ids (kid, as compute_kids lays them out), key by key */
static void
hash_cols(const uint32 *kid, int nk, int nsel, uint32 *hv)
{
	memset(hv, 0, nsel * sizeof(uint32));
	for (int j = 0; j < nk; j++)
	{
		const uint32 *col = kid + (Size) j * (VEC + PADW);

		for (int i = 0; i < nsel; i++)
		{
			uint32		h = (hv[i] ^ col[i]) * IDS_HASH_MUL;

			hv[i] = h ^ (h >> 16);
		}
	}
}

void
compute_gids(AggExec *ex, int nsel)
{
	int			nk = ex->spec->nkeys;
	uint32		hv[VEC + PADW];

	if (nk == 0)
	{
		memset(ex->gid, 0, nsel * sizeof(uint32));
		return;
	}
	if (!ex->hash)
	{
		uint32	   *maps[SIFT_AGG_MAX_KEYS];
		int32		bases[SIFT_AGG_MAX_KEYS];
		uint32	   *vals[SIFT_AGG_MAX_KEYS];
		bool		grow = false;

		/* make every id first: capacity checks need the final counts */
		for (int j = 0; j < nk; j++)
		{
			const uint32 *idx = NULL;

			if (key_lf(ex, j))
				gather_idx(ex, ex->spec->key[j].leaf, nsel, &idx);
			vals[j] = ex->kids + (Size) j * (VEC + PADW);
			key_prepare(ex, j, nsel, idx, &maps[j], &bases[j], vals[j]);
			if (key_used(&ex->ks[j]) > ex->ks[j].cap)
				grow = true;
		}
		if (grow)
			direct_grow(ex);
		if (!ex->hash)
		{
			memset(ex->gid, 0, nsel * sizeof(uint32));
			for (int j = 0; j < nk; j++)
			{
				const SiftSrcLeaf *lf = key_lf(ex, j);

				if (maps[j] == NULL)
					for (int i = 0; i < nsel; i++)
						ex->gid[i] += vals[j][i] * ex->ks[j].mult;
				else if (lf->vtype == SCV_CODE)
					sift_aggk->gid_code(ex->gid, vals[j], maps[j], nsel, ex->ks[j].mult);
				else
				{
					sift_aggk->gid_i32(ex->gid, (const int32 *) vals[j], bases[j], maps[j], nsel, ex->ks[j].mult);
					key_null_fix(ex, j, nsel, vals[j], ex->gid, ex->ks[j].mult, NULL);
				}
			}
			return;
		}
		/* the table just replaced the arrays: recompute below, in hash mode */
	}
	compute_kids(ex, nsel, ex->tmpu32);
	/* the slot of a tuple is prefetched GROUP_AHEAD tuples early */
	hash_cols(ex->tmpu32, nk, nsel, hv);
	for (int i = 0; i < Min(GROUP_AHEAD, nsel); i++)
		__builtin_prefetch(&ex->hslot[hv[i] & (ex->nhslot - 1)]);
	for (int i = 0; i < nsel; i++)
	{
		uint32		ids[SIFT_AGG_MAX_KEYS];

		if (i + GROUP_AHEAD < nsel)
			__builtin_prefetch(&ex->hslot[hv[i + GROUP_AHEAD] & (ex->nhslot - 1)]);
		for (int j = 0; j < nk; j++)
			ids[j] = ex->tmpu32[(Size) j * (VEC + PADW) + i];
		ex->gid[i] = ex->hashgh(ex, ids, hv[i]);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}

/*
 * The group ids of the current grouping set in hash mode from the key ids of the chunk (kid: compute_kids with every key): a key outside the set is
 * NULL (id 0) and the key of the sets is the set's number, as key_prepare gives them, so the sets that read the chunk's own tuples prepare the keys once.
 */
void
compute_set_gids(AggExec *ex, int nsel, const uint32 *kid)
{
	int			nk = ex->spec->nkeys;
	const uint32 *col[SIFT_AGG_MAX_KEYS];
	uint32		fixed[SIFT_AGG_MAX_KEYS];

	for (int j = 0; j < nk; j++)
	{
		bool		in = (ex->arm_mask >> j & 1) != 0;

		col[j] = in && ex->spec->key[j].kind != SAK_SET ? kid + (Size) j * (VEC + PADW) : NULL;
		fixed[j] = in ? ex->curset + 1 : 0;
	}
	for (int i = 0; i < nsel; i++)
	{
		uint32		ids[SIFT_AGG_MAX_KEYS];

		for (int j = 0; j < nk; j++)
			ids[j] = col[j] ? col[j][i] : fixed[j];
		ex->gid[i] = hash_group(ex, ids);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}
