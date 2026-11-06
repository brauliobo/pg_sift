/*
 * agg/gen_run.c - feeding rows to the generic aggregates and flushing them: DISTINCT ON, DISTINCT, ORDER BY
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
#include "core/cmp.h"

/* the aggregated arguments of tuple j */
static void
gen_args(AggExec *ex, GenSt *st, const GenIn *in, int j, Datum *args, bool *nulls)
{
	for (int k = 0; k < st->g->nargs; k++)
		gen_arg(ex, st, in, j, k, &args[k], &nulls[k]);
}

/* what the distinct values of a DISTINCT aggregate hold: the pair set, the value table and the values themselves */
static uint64
gen_hash_bytes(const DSet *d, const OutTable *ot)
{
	return d->cap * sizeof(uint64) + ot->cap * (sizeof(Datum) + sizeof(uint32)) + (uint64) ot->nslot * sizeof(uint32) + ot->bytes;
}

/*
 * The distinct pairs outgrew work_mem: they move to a tuplesort, where the inputs that follow go too and PostgreSQL's own
 * sort-and-skip-equal of a DISTINCT aggregate removes the duplicates (spilling to disk when it must).
 */
static void
gen_spill(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	DSet	   *d = &ex->dset[ai];
	OutTable   *ot = &ex->aot[ai];
	AggAcc	   *acc = &ex->acc[ai];
	Datum		args[SIFT_AGG_MAX_GARGS] = {0};
	bool		nulls[SIFT_AGG_MAX_GARGS] = {false};
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	elog(DEBUG1, "sift: DISTINCT of %s moved to a tuplesort", get_func_name(st->aggref->aggfnoid));
	gen_sort_begin(st);
	MemoryContextSwitchTo(old);
	st->spilled = true;
	for (uint64 i = 0; i < d->cap; i++)
		if (d->slot[i])
		{
			uint32		id = (uint32) d->slot[i];

			args[st->g->darg] = id ? ot->val[id - 1] : (Datum) 0;
			nulls[st->g->darg] = id == 0;
			gen_sort_put(st, acc, (d->slot[i] >> 32) & 0x7fffffff, args, nulls);
		}
	if (!ot->byval)
		for (uint32 i = 0; i < ot->n; i++)
			pfree(DatumGetPointer(ot->val[i]));
	pfree(ot->val);
	pfree(ot->hv);
	pfree(ot->slot);
	pfree(d->slot);
	ot->n = ot->cap = ot->nslot = 0;
	ot->val = NULL;
	ot->hv = NULL;
	ot->slot = NULL;
	ot->bytes = 0;
	d->slot = NULL;
	d->n = d->cap = 0;
}

/* does an argument go into the best row (a constant is the same in every row) */
static bool
gen_stored(const SiftAggGen *g, int k)
{
	return g->arg[k].nleaves > 0 || g->arg[k].mapped;
}

static void
gen_best_free(GenSt *st, GenBest *b)
{
	int			n = st->g->nargs;

	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k) && !GEN_BEST_NULL(b, n, k) && !st->abyval[k])
			pfree(DatumGetPointer(b[k]));
	pfree(b);
}

/* group g keeps the row whose arguments these are, in memory that lives as long as the scan; the row it had is freed */
static void
gen_best_set(GenSt *st, AggAcc *acc, uint32 g, const Datum *args, const bool *nulls)
{
	int			n = st->g->nargs;
	MemoryContext old;
	GenBest    *nb,
			   *ob = (GenBest *) DatumGetPointer(acc->v[g]);

	/* a row of by-value arguments takes the place of the one the group has: nothing to allocate or free */
	if (ob != NULL && st->inplace)
	{
		for (int k = 0; k < n; k++)
			if (gen_stored(st->g, k))
			{
				GEN_BEST_NULL(ob, n, k) = nulls[k];
				ob[k] = nulls[k] ? (Datum) 0 : args[k];
			}
		return;
	}
	old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
	nb = palloc0(GEN_BEST_SIZE(n));
	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k))
		{
			GEN_BEST_NULL(nb, n, k) = nulls[k];
			nb[k] = nulls[k] ? (Datum) 0 : datumCopy(args[k], st->abyval[k], st->alen[k]);
		}
	if (ob != NULL)
		gen_best_free(st, ob);
	acc->v[g] = PointerGetDatum(nb);
	MemoryContextSwitchTo(old);
}

/* the sort keys of a row against those of the best row: < 0 when the row comes first */
static int
gen_first_cmp(GenSt *st, const Datum *args, const bool *nulls, const GenBest *b)
{
	int			n = st->g->nargs;

	for (int i = 0; i < st->nfk; i++)
	{
		int			k = st->fkey[i],
					c;
		Datum		bv = gen_stored(st->g, k) ? b[k] : st->argk[k];
		bool		bn = gen_stored(st->g, k) ? GEN_BEST_NULL(b, n, k) : st->argknull[k];

		if ((c = ApplySortComparator(args[k], nulls[k], bv, bn, &st->fsup[i])) != 0)
			return c;
	}
	return 0;
}

/* tuple j of group gid: its row is the group's best when its sort keys come first (the values of the other arguments are read only then) */
static bool
gen_first(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, int j, uint32 gid)
{
	GenBest    *b = (GenBest *) DatumGetPointer(acc->v[gid]);
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];

	for (int i = 0; i < st->nfk; i++)
		gen_arg(ex, st, in, j, st->fkey[i], &args[st->fkey[i]], &nulls[st->fkey[i]]);
	if (b != NULL && gen_first_cmp(st, args, nulls, b) >= 0)
		return false;
	for (int k = 0; k < st->g->nargs; k++)
		if (!st->iskey[k])
			gen_arg(ex, st, in, j, k, &args[k], &nulls[k]);
	gen_best_set(st, acc, gid, args, nulls);
	return true;
}

/* the scan has ended: the first argument of the best row of each group is the group's value (the carrier aggregate has no transition or final function to run) */
static void
gen_flush_first(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;
	int			n = st->g->nargs;

	for (uint32 g = 0; g < ngr; g++)
	{
		GenBest    *b = (GenBest *) DatumGetPointer(acc->v[g]);

		if (b == NULL)
			continue;
		acc->v[g] = b[0];
		acc->nf[g] = GEN_INIT | (GEN_BEST_NULL(b, n, 0) ? GEN_NULL : 0);
		for (int k = 1; k < n; k++)
			if (gen_stored(st->g, k) && !GEN_BEST_NULL(b, n, k) && !st->abyval[k])
				pfree(DatumGetPointer(b[k]));
		pfree(b);
	}
}

/* a worker's best row of group g, appended to vb as a length (0: none) and the serialized values */
void
gen_first_put(GenSt *st, AggAcc *acc, uint32 g, StringInfo vb)
{
	const GenBest *b = (const GenBest *) DatumGetPointer(acc->v[g]);
	int			n = st->g->nargs;
	uint32		len = 0;
	char	   *at;

	if (b != NULL)
		for (int k = 0; k < n; k++)
			if (gen_stored(st->g, k))
				len += datumEstimateSpace(b[k], GEN_BEST_NULL(b, n, k), st->abyval[k], st->alen[k]);
	appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
	if (len == 0)
		return;
	enlargeStringInfo(vb, len);
	at = vb->data + vb->len;
	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k))
			datumSerialize(b[k], GEN_BEST_NULL(b, n, k), st->abyval[k], st->alen[k], &at);
	vb->len += len;
}

/* the best row a worker serialized joins group g's: it replaces the row of the leader when its sort keys come first */
void
gen_first_merge(GenSt *st, AggAcc *acc, uint32 g, const char *at)
{
	GenBest    *b = (GenBest *) DatumGetPointer(acc->v[g]);
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];

	for (int k = 0; k < st->g->nargs; k++)
		if (gen_stored(st->g, k))
			args[k] = datumRestore((char **) &at, &nulls[k]);
		else
		{
			args[k] = st->argk[k];
			nulls[k] = st->argknull[k];
		}
	if (b == NULL || gen_first_cmp(st, args, nulls, b) < 0)
		gen_best_set(st, acc, g, args, nulls);
}

/* is the first sort key of the call a plain column of a by-value type: its value is read from the chunk with no allocation and kept by a group across the tuples of the chunk */
static bool
gen_key_plain(const GenSt *st)
{
	const SiftAggGenArg *ar = &st->g->arg[st->fkey[0]];

	return !ar->mapped && ar->nleaves == 1 && ar->expr == NULL && st->abyval[st->fkey[0]];
}

static bool
gen_first_plain(const GenSt *st)
{
	return st->nfk == 1 && gen_key_plain(st);
}

/* the per-group arrays of a chunk's pass (the best or the last kept tuple of a group) cover the group */
static void
gen_chunk_room(AggExec *ex, GenSt *st, uint32 gid)
{
	if (gid >= st->ccap)
	{
		uint32		cap = Max(gid + 1, st->ccap * 2);

		if (st->ccap == 0)
		{
			st->cwin = MemoryContextAllocZero(ex->cxt, cap * sizeof(uint32));
			st->ckey = MemoryContextAlloc(ex->cxt, cap * sizeof(Datum));
			st->cnull = MemoryContextAlloc(ex->cxt, cap * sizeof(bool));
		}
		else
		{
			st->cwin = repalloc(st->cwin, cap * sizeof(uint32));
			memset(st->cwin + st->ccap, 0, (cap - st->ccap) * sizeof(uint32));
			st->ckey = repalloc(st->ckey, cap * sizeof(Datum));
			st->cnull = repalloc(st->cnull, cap * sizeof(bool));
		}
		st->ccap = cap;
	}
}

/*
 * The tuples of the chunk, when each group's best row can be told from the key alone: one pass keeps the best tuple of each group (the first of equal keys, as the tuple-by-tuple pass
 * does), then only those go through gen_first, which reads the other arguments and copies them. A group whose best row changes at every tuple (the chunk in the order of the
 * key) copies once for a chunk, not once for a tuple.
 */
static void
gen_first_chunk(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, const uint64 *ok, int nsel)
{
	uint32		nt = 0;

	if (st->ntcap < (uint32) nsel)
	{
		st->ntcap = nsel;
		st->ctouch = st->ctouch ? repalloc(st->ctouch, nsel * sizeof(uint32)) : MemoryContextAlloc(ex->cxt, nsel * sizeof(uint32));
	}
	memset(ex->fwin, 0, nsel);
	for (int j = 0; j < nsel; j++)
	{
		uint32		gid = ex->gid[j];
		Datum		d;
		bool		isnull;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		gen_chunk_room(ex, st, gid);
		gen_arg(ex, st, in, j, st->fkey[0], &d, &isnull);
		if (st->cwin[gid] == 0)
			st->ctouch[nt++] = gid;
		else if (ApplySortComparator(d, isnull, st->ckey[gid], st->cnull[gid], &st->fsup[0]) >= 0)
			continue;
		st->cwin[gid] = j + 1;
		st->ckey[gid] = d;
		st->cnull[gid] = isnull;
	}
	for (uint32 t = 0; t < nt; t++)
	{
		uint32		gid = st->ctouch[t],
					j = st->cwin[gid] - 1;

		st->cwin[gid] = 0;
		ex->fwin[j] = gen_first(ex, st, acc, in, j, gid);
		ResetExprContext(ex->lutcx);
	}
}

/*
 * topn: the first rows of a group by the ORDER BY of the call, in the order of the call, kept as the rows pass: a row that comes after the group's last of
 * topn rows is dropped after the compare of its sort keys (the values of the other arguments are read only for a row that is kept), and a row that stays
 * goes in at the place a binary search of the kept rows gives it, after the rows it ties with, which are the ones the group met first. When the scan has
 * ended, the rows of each group go through the transition function, in order, and then the final function (gen_flush_topn).
 * SIFT_TOPN_RANK keeps the rows that tie with the topn-th as well (the rows of rank <= topn), and SIFT_TOPN_DENSE the rows of the topn best values of the sort
 * keys (dense_rank <= topn): a row is dropped when it comes after the last kept row of a group that has the rows it needs, and the rows past the ties of the
 * topn-th, or of the value after the topn best, go when a better row has taken their place.
 */
typedef struct GenTop
{
	uint32		n;
	uint32		cap;
	uint32		distinct;			/* SIFT_TOPN_DENSE: the values of the sort keys of the rows kept */
	GenBest   **row;
} GenTop;

/* the sort keys of two kept rows: < 0 when a comes first */
static int
gen_best_cmp(GenSt *st, const GenBest *a, const GenBest *b)
{
	int			n = st->g->nargs;

	for (int i = 0; i < st->nfk; i++)
	{
		int			k = st->fkey[i],
					c;
		Datum		av = gen_stored(st->g, k) ? a[k] : st->argk[k],
					bv = gen_stored(st->g, k) ? b[k] : st->argk[k];
		bool		an = gen_stored(st->g, k) ? GEN_BEST_NULL(a, n, k) : st->argknull[k],
					bn = gen_stored(st->g, k) ? GEN_BEST_NULL(b, n, k) : st->argknull[k];

		if ((c = ApplySortComparator(av, an, bv, bn, &st->fsup[i])) != 0)
			return c;
	}
	return 0;
}

/* the row a new one is measured against to stay: the topn-th row, or the last row of a group that holds the values of the topn best */
static const GenBest *
gen_top_edge(const GenSt *st, const GenTop *t)
{
	if (st->g->ties == SIFT_TOPN_ROWS || st->g->ties == SIFT_TOPN_RANK)
		return t->n >= (uint32) st->g->topn ? t->row[st->g->topn - 1] : NULL;
	return t->distinct >= (uint32) st->g->topn ? t->row[t->n - 1] : NULL;
}

static void
gen_top(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, int j, uint32 gid)
{
	GenTop	   *t = (GenTop *) DatumGetPointer(acc->v[gid]);
	int			n = st->g->nargs,
				ties = st->g->ties;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];
	uint32		lo = 0,
				hi;
	const GenBest *edge;
	bool		fresh;
	GenBest    *nb;
	MemoryContext old;

	for (int i = 0; i < st->nfk; i++)
		gen_arg(ex, st, in, j, st->fkey[i], &args[st->fkey[i]], &nulls[st->fkey[i]]);
	if (t != NULL && (edge = gen_top_edge(st, t)) != NULL)
	{
		int			c = gen_first_cmp(st, args, nulls, edge);

		if (ties == SIFT_TOPN_ROWS ? c >= 0 : c > 0)
			return;
	}
	for (int k = 0; k < n; k++)
		if (!st->iskey[k])
			gen_arg(ex, st, in, j, k, &args[k], &nulls[k]);
	old = MemoryContextSwitchTo(st->tcx);
	if (t == NULL)
	{
		t = palloc0(sizeof(GenTop));
		acc->v[gid] = PointerGetDatum(t);
	}
	hi = t->n;
	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (gen_first_cmp(st, args, nulls, t->row[mid]) < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	/* the rows of a value are kept together: the row is a new value when the one before it comes first */
	fresh = lo == 0 || gen_first_cmp(st, args, nulls, t->row[lo - 1]) != 0;
	if (ties == SIFT_TOPN_ROWS && t->n == (uint32) st->g->topn)
		gen_best_free(st, t->row[--t->n]);
	if (t->n == t->cap)
	{
		t->cap = t->cap ? t->cap * 2 : Min(4, (uint32) st->g->topn);
		if (ties == SIFT_TOPN_ROWS)
			t->cap = Min(t->cap, (uint32) st->g->topn);
		t->row = t->row ? repalloc(t->row, t->cap * sizeof(GenBest *)) : palloc(t->cap * sizeof(GenBest *));
	}
	nb = palloc0(GEN_BEST_SIZE(n));
	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k))
		{
			GEN_BEST_NULL(nb, n, k) = nulls[k];
			nb[k] = nulls[k] ? (Datum) 0 : datumCopy(args[k], st->abyval[k], st->alen[k]);
		}
	memmove(&t->row[lo + 1], &t->row[lo], (t->n - lo) * sizeof(GenBest *));
	t->row[lo] = nb;
	t->n++;
	t->distinct += fresh;
	if (ties == SIFT_TOPN_RANK)
	{
		/* the rows past the topn-th that do not tie with it */
		while (t->n > (uint32) st->g->topn && gen_best_cmp(st, t->row[t->n - 1], t->row[st->g->topn - 1]) > 0)
			gen_best_free(st, t->row[--t->n]);
	}
	else if (ties == SIFT_TOPN_DENSE && t->distinct > (uint32) st->g->topn)
	{
		/* the rows of the worst value, which a better one has pushed out of the topn best */
		const GenBest *last = t->row[t->n - 1];
		uint32		first = t->n - 1;

		while (first > 0 && gen_best_cmp(st, t->row[first - 1], last) == 0)
			first--;
		while (t->n > first)
			gen_best_free(st, t->row[--t->n]);
		t->distinct--;
	}
	MemoryContextSwitchTo(old);
}

/*
 * The tuples of the chunk of a topn whose first sort key is a plain by-value column: a tuple whose first key comes after that of the last kept tuple of its group (the edge
 * gen_top compares it with) is dropped on that key alone, read from the chunk, with no allocation and no reset of the expression context; one that ties with the edge on it
 * goes on to the later keys (with no later key a tie is dropped too, for the rows of a topn, as in gen_top). The edge of a group is kept in ckey, as the best tuple of
 * a chunk is for a first(): the most of the tuples of a group that has its rows are dropped here.
 */
static void
gen_top_chunk(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, const uint64 *ok, int nsel)
{
	int			k = st->fkey[0],
				n = st->g->nargs;
	uint32		stamp = ++st->cstamp;	/* an edge holds for the chunk it was read in: the group ids may be renumbered between chunks */

	for (int j = 0; j < nsel; j++)
	{
		uint32		gid = ex->gid[j];
		const GenBest *edge;
		Datum		d;
		bool		isnull;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		gen_chunk_room(ex, st, gid);
		if (st->cwin[gid] == stamp)
		{
			int			c;

			gen_arg(ex, st, in, j, k, &d, &isnull);
			c = ApplySortComparator(d, isnull, st->ckey[gid], st->cnull[gid], &st->fsup[0]);
			if (st->g->ties == SIFT_TOPN_ROWS && st->nfk == 1 ? c >= 0 : c > 0)
				continue;
		}
		gen_top(ex, st, acc, in, j, gid);
		ResetExprContext(ex->lutcx);
		edge = gen_top_edge(st, (GenTop *) DatumGetPointer(acc->v[gid]));
		st->cwin[gid] = edge != NULL ? stamp : 0;
		if (edge != NULL)
		{
			st->ckey[gid] = edge[k];
			st->cnull[gid] = GEN_BEST_NULL(edge, n, k);
		}
	}
}

/* the scan has ended: the rows each group kept go through the transition function in order, then its final function */
static void
gen_flush_topn(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;
	int			n = st->g->nargs;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];

	for (uint32 g = 0; g < ngr; g++)
	{
		GenTop	   *t = (GenTop *) DatumGetPointer(acc->v[g]);

		if (t == NULL)
			continue;
		acc->v[g] = (Datum) 0;
		for (uint32 r = 0; r < t->n; r++)
		{
			const GenBest *b = t->row[r];

			for (int k = 0; k < st->g->ntrans; k++)
			{
				bool		stored = gen_stored(st->g, k);

				args[k] = stored ? b[k] : st->argk[k];
				nulls[k] = stored ? GEN_BEST_NULL(b, n, k) : st->argknull[k];
			}
			gen_advance(ex, st, acc, g, args, nulls);
			ResetExprContext(ex->lutcx);
		}
		gen_final(ex, st, acc, g);
		acc->v[g] = (Datum) 0;
		ReScanExprContext(st->cx);
	}
	MemoryContextReset(st->tcx);
}

/* the tuples of the chunk: DISTINCT collects the pairs, WITHIN GROUP sorts the values by group, the others advance the states */
void
gen_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	GenSt	   *st = ex->gst[ai];
	const SiftAggGen *g = st->g;
	AggAcc	   *acc = &ex->acc[ai];
	GenIn		in;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	for (int k = 0; k < g->nargs; k++)
		if (g->arg[k].mapped)
		{
			in.cout[k] = combo_outs(ex, g->arg[k].combo);
			in.cval[k] = ex->cs[g->arg[k].combo].val;
		}
	for (int k = 0; k < g->nargs; k++)
		for (int p = 0; p < g->arg[k].nleaves; p++)
		{
			in.vec[k][p] = get_vec(ex, g->arg[k].leaf[p]);
			gather_idx(ex, g->arg[k].leaf[p], nsel, &in.idx[k][p]);
		}
	if (g->first && !st->follower && gen_first_plain(st))
	{
		gen_first_chunk(ex, st, acc, &in, ok, nsel);
		MemoryContextSwitchTo(old);
		return;
	}
	if (g->topn && gen_key_plain(st))
	{
		gen_top_chunk(ex, st, acc, &in, ok, nsel);
		MemoryContextSwitchTo(old);
		return;
	}
	if (st->stat)
	{
		gen_stat_chunk(ex, st, acc, &in, ok, nsel);
		MemoryContextSwitchTo(old);
		return;
	}
	for (int j = 0; j < nsel; j++)
	{
		uint32		gid = ex->gid[j];

		if (ok && !sift_colsrc_bit(ok, j))
		{
			ex->fwin[j] = 0;
			continue;
		}
		if (g->first)
		{
			if (!st->follower)
				ex->fwin[j] = gen_first(ex, st, acc, &in, j, gid);
			else if (ex->fwin[j])
			{
				gen_args(ex, st, &in, j, args, nulls);
				gen_best_set(st, acc, gid, args, nulls);
			}
			ResetExprContext(ex->lutcx);
			continue;
		}
		if (g->topn)
		{
			gen_top(ex, st, acc, &in, j, gid);
			ResetExprContext(ex->lutcx);
			continue;
		}
		gen_args(ex, st, &in, j, args, nulls);
		if (g->sorted || st->spilled)
			gen_sort_put(st, acc, gid, args, nulls);
		else if (g->ordered)
		{
			/* percentile_*, mode(): the transition function leaves NULLs out; a hypothetical-set aggregate counts them */
			if (st->mode && !nulls[0])
				gen_mode_put(ex, st, acc, gid, args[0]);
			else if (g->hyp || !nulls[0])
				gen_sort_put(st, acc, gid, args, nulls);
		}
		else if (g->distinct)
		{
			bool		dn = nulls[g->darg];

			if (!(st->strict && memchr(nulls, true, g->nargs)))
			{
				uint32		id = 0;

				if (!dn)
				{
					MemoryContextSwitchTo(ex->tcxt);
					id = ot_id(&ex->aot[ai], args[g->darg]);
					MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
				}
				dset_add(ex, &ex->dset[ai], gid, id);
			}
		}
		else
			gen_advance(ex, st, acc, gid, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	MemoryContextSwitchTo(old);
	if (gen_mode_spill_due(st))
		gen_mode_spill(ex, st);
	if (g->distinct && !g->sorted && !st->spilled && gen_hash_bytes(&ex->dset[ai], &ex->aot[ai]) > (uint64) work_mem * 1024)
		gen_spill(ex, ai);
}

typedef struct GenPair
{
	uint32		g;
	uint32		id;
} GenPair;

typedef struct GenSortArg
{
	GenSt	   *st;
	const OutTable *ot;
	Oid			collid;
} GenSortArg;

/* by group, then the type's btree order (NULL last), then the id: PostgreSQL sorts a DISTINCT aggregate's input this way */
static int
gen_pair_cmp(const void *pa, const void *pb, void *arg)
{
	const GenPair *a = pa,
			   *b = pb;
	const GenSortArg *sa = arg;
	int32		c;

	if (a->g != b->g)
		return SIFT_CMP3(a->g, b->g);
	if (a->id == 0 || b->id == 0)
		return a->id == b->id ? 0 : a->id == 0 ? 1 : -1;
	c = DatumGetInt32(FunctionCall2Coll(sa->st->cmp, sa->collid, sa->ot->val[a->id - 1], sa->ot->val[b->id - 1]));
	if (c != 0)
		return c;
	return a->id < b->id ? -1 : 1;
}

/* DISTINCT: the distinct values of each group go through the transition function, in order, the first of a run of equal ones only */
static void
gen_flush_distinct(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	const DSet *d = &ex->dset[ai];
	AggAcc	   *acc = &ex->acc[ai];
	GenSortArg	sa = {st, &ex->aot[ai], ((Aggref *) st->g->aggref)->inputcollid};
	GenPair    *pr = palloc(Max(d->n, 1) * sizeof(GenPair));
	uint64		n = 0;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];
	int			darg = st->g->darg;

	for (int k = 0; k < st->g->nargs; k++)
	{
		args[k] = st->argk[k];
		nulls[k] = st->argknull[k];
	}
	for (uint64 i = 0; i < d->cap; i++)
		if (d->slot[i])
			pr[n++] = (GenPair) {(d->slot[i] >> 32) & 0x7fffffff, (uint32) d->slot[i]};
	qsort_arg(pr, n, sizeof(GenPair), gen_pair_cmp, &sa);
	for (uint64 i = 0; i < n; i++)
	{
		Datum		v = pr[i].id ? ex->aot[ai].val[pr[i].id - 1] : (Datum) 0;

		if (i > 0 && pr[i - 1].g == pr[i].g)
		{
			if (pr[i].id == 0 && pr[i - 1].id == 0)
				continue;
			if (pr[i].id != 0 && pr[i - 1].id != 0 &&
				DatumGetBool(FunctionCall2Coll(st->eq, sa.collid, ex->aot[ai].val[pr[i - 1].id - 1], v)))
				continue;
		}
		args[darg] = v;
		nulls[darg] = pr[i].id == 0;
		gen_advance(ex, st, acc, pr[i].g, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	pfree(pr);
}

/*
 * DISTINCT in the sort: is the tuple just read equal, in every column the DISTINCT list names (NULLs equal), to the one before it of the
 * same group? The tuple becomes the one before when it is not.
 */
static bool
gen_same_input(GenSt *st, bool samegroup)
{
	bool		same = samegroup;
	MemoryContext old;

	for (int i = 0; same && i < st->ndcol; i++)
	{
		bool		n;
		Datum		d = slot_getattr(st->get, 2 + st->dcol[i], &n);

		same = n == st->prevnull[i] && (n || DatumGetBool(FunctionCall2Coll(&st->deq[i], ((Aggref *) st->g->aggref)->inputcollid,
																		  st->prev[i], d)));
	}
	if (same)
		return true;
	MemoryContextReset(st->pcx);
	old = MemoryContextSwitchTo(st->pcx);
	for (int i = 0; i < st->ndcol; i++)
	{
		Datum		d = slot_getattr(st->get, 2 + st->dcol[i], &st->prevnull[i]);

		st->prev[i] = st->prevnull[i] ? (Datum) 0 : datumCopy(d, st->dbyval[i], st->dlen[i]);
	}
	MemoryContextSwitchTo(old);
	return false;
}

/* WITHIN GROUP, ORDER BY: the values of a group go through the transition function, then the final function; the state is freed */
static void
gen_flush_ordered(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		cur = UINT32_MAX,
				ngr = ex->hash ? ex->ngroups : ex->gcap,
			   *sid2g = palloc(Max(st->nsid, 1) * sizeof(uint32));
	bool		isnull,
				nulls[SIFT_AGG_MAX_GARGS] = {false};
	Datum		args[SIFT_AGG_MAX_GARGS];

	for (uint32 g = 0; g < ngr; g++)
		if (acc->i[g])
			sid2g[acc->i[g] - 1] = g;
	tuplesort_performsort(st->sort);
	while (tuplesort_gettupleslot(st->sort, true, false, st->get, NULL))
	{
		uint32		g = sid2g[DatumGetInt32(slot_getattr(st->get, 1, &isnull))];

		if (st->tmode)
			for (int k = 0; k < st->g->ntrans; k++)
			{
				bool		cn = st->vcol[k] < 0;

				args[k] = cn ? st->argk[k] : slot_getattr(st->get, 2 + st->vcol[k], &isnull);
				nulls[k] = cn ? st->argknull[k] : isnull;
			}
		else
			args[0] = slot_getattr(st->get, 2, &isnull);
		if (g != cur && cur != UINT32_MAX)
		{
			gen_final(ex, st, acc, cur);
			acc->v[cur] = (Datum) 0;
			ReScanExprContext(st->cx);
		}
		if (st->g->distinct && gen_same_input(st, g == cur))
			continue;
		cur = g;
		gen_advance(ex, st, acc, g, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	if (cur != UINT32_MAX)
	{
		gen_final(ex, st, acc, cur);
		acc->v[cur] = (Datum) 0;
		ReScanExprContext(st->cx);
	}
	tuplesort_end(st->sort);
	st->sort = NULL;
	pfree(sid2g);
}

/* the scan has ended: the aggregates that kept their values until now give them to the transition functions */
void
gen_flush(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai] && ex->gst[ai]->g->first)
			gen_flush_first(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->mode)
			gen_mode_flush(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->g->topn)
			gen_flush_topn(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->tmode)
			gen_flush_ordered(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->g->distinct)
			gen_flush_distinct(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->g->ordered)
			gen_flush_ordered(ex, ai);
}

/* the final function of every group that is listed and was not finished */
void
gen_finish(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai])
			for (uint32 i = 0; i < ex->nglist; i++)
				if (!(ex->acc[ai].nf[ex->glist[i]] & GEN_DONE))
					gen_final(ex, ex->gst[ai], &ex->acc[ai], ex->glist[i]);
}
