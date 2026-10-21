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

/* ---------- DISTINCT ON: the best row of each group ---------- */

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
	MemoryContext old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
	GenBest    *nb = palloc0(GEN_BEST_SIZE(n)),
			   *ob = (GenBest *) DatumGetPointer(acc->v[g]);

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
		gen_args(ex, st, &in, j, args, nulls);
		if (g->sorted || st->spilled)
			gen_sort_put(st, acc, gid, args, nulls);
		else if (g->ordered)
		{
			/* percentile_*, mode(): the transition function leaves NULLs out; a hypothetical-set aggregate counts them */
			if (g->hyp || !nulls[0])
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
		return a->g < b->g ? -1 : 1;
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
