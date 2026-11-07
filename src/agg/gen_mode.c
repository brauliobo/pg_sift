/*
 * agg/gen_mode.c - mode() WITHIN GROUP: a count of each (group, value) pair instead of a sort
 * The generic path sorts every value of every group (a tuplesort by group), then hands the values of a group to ordered_set_transition, which sorts them
 * again, and mode_final walks the second sort. Here the values are numbered as DISTINCT numbers them (an OutTable) and the pairs counted in a hash; when the
 * scan has ended each group takes the value with the highest count, the first in the sort order of the call among equal counts, which is the value
 * mode_final returns (it keeps the first value of the longest run of the sorted input). The counts outgrowing work_mem move to the tuplesort, each value
 * put as many times as it was counted, and the generic path goes on from there.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"
#include "utils/typcache.h"

#include "agg/agg_int.h"
#include "agg/gen_int.h"
#include "agg/leaf_int.h"
#include "core/hash.h"

typedef struct GenMode
{
	OutTable	ot;				/* the distinct values */
	uint64	   *key;			/* (group number << 32) | value id, 0: free */
	uint32	   *cnt;
	uint64		cap;
	uint64		n;
	SortSupportData ssup;		/* the order of the call: sort operator, collation */
} GenMode;

static inline uint64
mode_hash(uint64 k)
{
	k = sift_fib64(k);
	return k ^ (k >> 32);
}

/* the counts and the values it holds, in bytes */
static uint64
mode_bytes(const GenMode *m)
{
	return m->cap * (sizeof(uint64) + sizeof(uint32)) + m->ot.cap * (sizeof(Datum) + sizeof(uint32)) + (uint64) m->ot.nslot * sizeof(uint32) + m->ot.bytes;
}

static void
mode_grow(GenMode *m)
{
	uint64		ncap = m->cap * 2;
	MemoryContext cx = GetMemoryChunkContext(m->key);	/* the scan's: a row's memory is reset after it */
	uint64	   *nk = MemoryContextAllocZero(cx, ncap * sizeof(uint64));
	uint32	   *nc = MemoryContextAlloc(cx, ncap * sizeof(uint32));

	for (uint64 i = 0; i < m->cap; i++)
		if (m->key[i])
		{
			uint64		h = mode_hash(m->key[i]) & (ncap - 1);

			while (nk[h])
				h = (h + 1) & (ncap - 1);
			nk[h] = m->key[i];
			nc[h] = m->cnt[i];
		}
	pfree(m->key);
	pfree(m->cnt);
	m->key = nk;
	m->cnt = nc;
	m->cap = ncap;
}

static void
mode_add(GenMode *m, uint32 sid, uint32 id)
{
	uint64		k = ((uint64) sid << 32) | id,
				h = mode_hash(k) & (m->cap - 1);

	while (m->key[h])
	{
		if (m->key[h] == k)
		{
			m->cnt[h]++;
			return;
		}
		h = (h + 1) & (m->cap - 1);
	}
	m->key[h] = k;
	m->cnt[h] = 1;
	if (++m->n * 2 > m->cap)
		mode_grow(m);
}

/* mode() with a hashable argument type (not the hypothetical-set aggregates): its values are counted, not sorted */
bool
gen_mode_pick(AggExec *ex, GenSt *st)
{
	const SiftAggGen *g = st->g;
	Aggref	   *ref = st->aggref;
	SortGroupClause *sc;
	TargetEntry *te;
	TypeCacheEntry *tc;
	RegProcedure lhs,
				rhs;
	GenMode    *m;

	if (!g->ordered || g->hyp || !st->has_final || st->finalfn.fn_oid != F_MODE_FINAL || g->nargs != 1)
		return false;
	tc = lookup_type_cache(g->arg[0].typid, TYPECACHE_EQ_OPR);
	if (!OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs))
		return false;
	sc = linitial_node(SortGroupClause, ref->aggorder);
	te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);
	m = MemoryContextAllocZero(ex->cxt, sizeof(GenMode));
	ot_init(&m->ot, tc->eq_opr, g->arg[0].typid, exprCollation((Node *) te->expr));
	m->cap = 1024;
	m->key = MemoryContextAllocZero(ex->cxt, m->cap * sizeof(uint64));
	m->cnt = MemoryContextAlloc(ex->cxt, m->cap * sizeof(uint32));
	m->ssup.ssup_cxt = ex->cxt;
	m->ssup.ssup_collation = exprCollation((Node *) te->expr);
	m->ssup.ssup_nulls_first = sc->nulls_first;
	PrepareSortSupportFromOrderingOp(sc->sortop, &m->ssup);
	st->mode = m;
	return true;
}

/* one value of tuple's group: the group is numbered as its first value arrives, as the sort path numbers it */
void
gen_mode_put(AggExec *ex, GenSt *st, AggAcc *acc, uint32 gid, Datum v)
{
	GenMode    *m = st->mode;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint32		id = ot_id(&m->ot, v);

	MemoryContextSwitchTo(old);
	if (acc->i[gid] == 0)
		acc->i[gid] = ++st->nsid;
	mode_add(m, acc->i[gid], id);
}

/* the counts outgrew work_mem: the generic path takes over, with each value put as many times as it was counted */
bool
gen_mode_spill_due(const GenSt *st)
{
	return st->mode && mode_bytes(st->mode) > (uint64) work_mem * 1024;
}

void
gen_mode_spill(AggExec *ex, GenSt *st)
{
	GenMode    *m = st->mode;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	Datum		args[SIFT_AGG_MAX_GARGS] = {0};
	bool		nulls[SIFT_AGG_MAX_GARGS] = {false};

	elog(DEBUG1, "sift: mode() moved to a tuplesort");
	gen_sort_begin(st);
	MemoryContextSwitchTo(old);
	st->spilled = true;
	for (uint64 i = 0; i < m->cap; i++)
		if (m->key[i])
		{
			args[0] = m->ot.val[(uint32) m->key[i] - 1];
			for (uint32 c = 0; c < m->cnt[i]; c++)
				gen_sort_put_sid(st, (uint32) (m->key[i] >> 32) - 1, args, nulls);
		}
	if (!m->ot.byval)
		for (uint32 i = 0; i < m->ot.n; i++)
			pfree(DatumGetPointer(m->ot.val[i]));
	pfree(m->key);
	pfree(m->cnt);
	pfree(m->ot.val);
	pfree(m->ot.hv);
	pfree(m->ot.slot);
	pfree(m);
	st->mode = NULL;
}

/* the scan has ended: each group's result is its most frequent value (the first of them in the order of the call) */
void
gen_mode_flush(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	GenMode    *m = st->mode;
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;
	uint32	   *best = palloc0(Max(st->nsid, 1) * sizeof(uint32)),
			   *bcnt = palloc0(Max(st->nsid, 1) * sizeof(uint32));
	MemoryContext old;

	for (uint64 i = 0; i < m->cap; i++)
		if (m->key[i])
		{
			uint32		sid = (uint32) (m->key[i] >> 32) - 1,
						id = (uint32) m->key[i],
						c = m->cnt[i];

			if (c > bcnt[sid] ||
				(c == bcnt[sid] && ApplySortComparator(m->ot.val[id - 1], false, m->ot.val[best[sid] - 1], false, &m->ssup) < 0))
			{
				best[sid] = id;
				bcnt[sid] = c;
			}
		}
	old = MemoryContextSwitchTo(ex->gres);
	for (uint32 g = 0; g < ngr; g++)
		if (acc->i[g])
		{
			Datum		v = m->ot.val[best[acc->i[g] - 1] - 1];

			acc->r[g] = st->rbyval ? v : datumCopy(v, st->rbyval, st->rlen);
			acc->nf[g] = GEN_INIT | GEN_DONE;
		}
	MemoryContextSwitchTo(old);
	pfree(best);
	pfree(bcnt);
}
