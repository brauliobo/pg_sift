/*
 * agg/fuse.c - arms that share one pass over the units
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

/*
 * ---------- arms that share one pass ----------
 *
 * The arms of a UNION ALL over one CTE (a time series at four granularities) are nodes that scan the same rows for the same aggregates and
 * group by different keys. The first of them to run finds the others (nodes of the same executor state register when they begin) and
 * scans for all of them as the grouping sets of one spec: the leaves are decoded and the filters run once, and every arm gets its groups
 * from the set that is its own. The sums are those of an arm alone (the same tuples in the same lanes), so the rows are the same.
 */

typedef struct FuseReg
{
	EState	   *estate;
	List	   *nodes;			/* the AggExec of the estate that have not started */
} FuseReg;

static List *fuse_regs;

static void
fuse_reg_free(void *arg)
{
	FuseReg    *r = arg;

	fuse_regs = list_delete_ptr(fuse_regs, r);
	list_free(r->nodes);
	pfree(r);
}

static FuseReg *
fuse_reg(EState *estate, bool create)
{
	FuseReg    *r;
	MemoryContextCallback *cb;
	MemoryContext old;

	foreach_ptr(FuseReg, x, fuse_regs)
		if (x->estate == estate)
			return x;
	if (!create)
		return NULL;
	old = MemoryContextSwitchTo(TopMemoryContext);
	r = palloc0(sizeof(FuseReg));
	r->estate = estate;
	fuse_regs = lappend(fuse_regs, r);
	MemoryContextSwitchTo(old);
	cb = MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback));
	cb->func = fuse_reg_free;
	cb->arg = r;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);
	return r;
}

void
fuse_register(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, true);
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	r->nodes = lappend(r->nodes, ex);
	MemoryContextSwitchTo(old);
}

void
fuse_unregister(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, false);

	if (r)
		r->nodes = list_delete_ptr(r->nodes, ex);
}

/* the nodes (this one first) that one pass can serve, scanned into the groups of a fused executor; false when this node runs alone */
bool
fuse_start(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, false);
	EState	   *estate = ex->css.ss.ps.state;
	List	   *m = list_make1(ex);
	SiftAggSpec **specs;
	int		  **keymap;
	int			n = 1;
	MemoryContext old;
	FuseSt	   *fs;
	AggExec    *F;

	if (r == NULL)
		return false;
	specs = palloc(sizeof(SiftAggSpec *) * (list_length(r->nodes) + 1));
	specs[0] = ex->spec;
	foreach_ptr(AggExec, o, r->nodes)
	{
		specs[n] = o->spec;
		if (o != ex && !o->started && sift_aggspec_fusable(specs, n + 1))
		{
			m = lappend(m, o);
			n++;
		}
	}
	if (n < 2)
		return false;
	keymap = palloc(sizeof(int *) * n);
	old = MemoryContextSwitchTo(estate->es_query_cxt);
	F = palloc0(sizeof(AggExec));
	F->spec = sift_aggspec_fuse(specs, n, keymap);
	F->pack = sift_aggspec_pack(F->spec);
	F->params = ex->params;
	F->snap = ex->snap;
	F->css.ss.ps.state = estate;
	F->css.custom_ps = ex->css.custom_ps;
	fs = palloc0(sizeof(FuseSt));
	fs->F = F;
	fs->keymap = keymap;
	fs->refs = n;
	engine_contexts(F, estate->es_query_cxt);
	engine_rels(F);
	MemoryContextSwitchTo(old);
	run_scan(F);
	empty_set_groups(F->gtab);
	dist_finish_all(F->gtab);
	dist_end_all(F->gtab);
	mapped_finish(F->gtab);
	list_groups(F->gtab);
	for (int i = 0; i < n; i++)
	{
		AggExec    *o = list_nth(m, i);

		o->fuse = fs;
		o->fuse_set = i;
		o->started = true;
		r->nodes = list_delete_ptr(r->nodes, o);
	}
	return true;
}

/* the groups of the fused scan that are this node's set */
static void
fuse_list(AggExec *ex)
{
	AggExec    *t = ex->fuse->F->gtab;
	int			setkey = t->spec->nkeys - 1;

	ex->glist = MemoryContextAlloc(ex->cxt, Max(t->nglist, 1) * sizeof(uint32));
	ex->nglist = 0;
	for (uint32 i = 0; i < t->nglist; i++)
		if (key_id(t, setkey, t->glist[i]) == (uint32) ex->fuse_set + 1)
			ex->glist[ex->nglist++] = t->glist[i];
	ex->n_groups = ex->nglist;
}

TupleTableSlot *
fuse_next(AggExec *ex, TupleTableSlot *slot)
{
	AggExec    *t = ex->fuse->F->gtab;
	uint32		g;

	if (ex->glist == NULL)
		fuse_list(ex);
	if (ex->gpos >= ex->nglist)
		return NULL;
	g = ex->glist[ex->gpos++];
	ExecClearTuple(slot);
	for (int j = 0; j < ex->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(t, ex->fuse->keymap[ex->fuse_set][j], g, &slot->tts_isnull[j]);
	for (int i = 0; i < ex->spec->naggs; i++)
	{
		int			c = ex->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(t, i, g, &slot->tts_isnull[c]);
	}
	return ExecStoreVirtualTuple(slot);
}
