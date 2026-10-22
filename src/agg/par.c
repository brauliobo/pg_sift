/*
 * agg/par.c - items of work, lanes and parallel workers
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
 * The superblocks are split in items that the participants take from a counter. Float sums are order dependent, so that a result
 * does not depend on the number of workers: such a scan has SIFT_AGG_LANES items, lane l being the superblocks l, l + SIFT_AGG_LANES, ..., and the
 * tables of the lanes are combined in lane order, whoever scanned them. The other aggregates are exact in any order: items are
 * MORSEL_SBS superblocks, and a participant keeps one table for all its items.
 */
#define MORSEL_SBS	2

Size
shared_size(const SiftAggSpec *s, uint32 norder)
{
	return offsetof(ScanShared, partoff) + (s->nparts + 1 + norder) * sizeof(uint32);
}

uint32 *
shared_order(ScanShared *sh, const SiftAggSpec *s)
{
	return sh->partoff + s->nparts + 1;
}

/*
 * A top-N takes windows of superblocks in the visit order the leader's probes chose: one superblock each in the walk's order (norder
 * superblocks, copied), else the superblocks of a match set's chunk (the set is made for a chunk), else one, from the last when the sort
 * column grows toward it.
 */
static void
topn_shared_init(ScanShared *sh, AggExec *ex)
{
	sh->topn_reverse = sift_topn_reverse(ex->topn);
	sh->topn_walk = ex->nvorder > 0;
	sh->topn_item = ex->search ? sift_search_chunk_sbs : 1;
	sh->nitems = sh->topn_walk ? sh->nsb : (sh->nsb + sh->topn_item - 1) / sh->topn_item;
	sift_topn_bound_init(&sh->topn_bound);
	if (sh->topn_walk)
		memcpy(shared_order(sh, ex->spec), ex->vorder, ex->nvorder * sizeof(uint32));
}

void
shared_init(ScanShared *sh, AggExec *ex)
{
	sh->nsb = sift_colsrc_nsb(ex->src);
	if (ex->spec->nparts > 0)
		sift_aggpart_offsets(ex->src, sh->partoff);
	sh->ordered = sift_aggspec_ordered(ex->spec);
	sh->morsel = sift_aggspec_parallel_ok(ex->spec) ? MORSEL_SBS : Max(sh->nsb, 1);
	sh->nitems = sh->ordered ? SIFT_AGG_LANES : (sh->nsb + sh->morsel - 1) / sh->morsel;
	pg_atomic_init_u32(&sh->next, 0);
	if (ex->topn)
		topn_shared_init(sh, ex);
}

void
scan_units(AggExec *ex, uint32 lo, uint32 hi)
{
	lo = Max(lo, ex->tid_sb_lo);
	hi = Min(hi, ex->tid_sb_hi);
	if (lo >= hi)
		return;
	sift_colsrc_begin(ex->src, lo, hi, ex->prune, ex->nzone);
	while (sift_colsrc_next(ex->src, &ex->unit))
	{
		process_unit(ex);
		if (sift_aggbe_stopped(ex->be, ex->be_state))
			break;				/* the scan is run again without the backend (backend_finish) */
	}
}

/* With a match set, the source keeps the matching rows of the units of the range only (agg/search.h) */
static void
scan_range(AggExec *ex, uint32 lo, uint32 hi)
{
	if (ex->search == NULL)
	{
		scan_units(ex, lo, hi);
		return;
	}
	for (uint32 a = lo; a < hi; a += sift_search_chunk_sbs)
	{
		sift_colsrc_select(ex->src, sift_search_range(ex->search, a, Min(a + sift_search_chunk_sbs, hi)));
		scan_units(ex, a, Min(a + sift_search_chunk_sbs, hi));
	}
}

/* a grouping set without columns has its group even when no row reached it, as the plain aggregate has its row */
void
empty_set_groups(AggExec *ex)
{
	for (int set = 0; set < ex->spec->nsets; set++)
		if (ex->spec->setmask[set] == (uint32) 1 << (ex->spec->nkeys - 1))
		{
			ex->curset = set;
			ex->arm_mask = ex->spec->setmask[set];
			compute_gids(ex, 1);
			ex->seen[ex->gid[0]] = Max(ex->seen[ex->gid[0]], 1);
		}
	ex->arm_mask = ~(uint32) 0;
}

static void
scan_item(AggExec *ex, uint32 item)
{
	ScanShared *sh = ex->shared;

	if (sh->ordered)
		for (uint32 sb = item; sb < sh->nsb; sb += SIFT_AGG_LANES)
			scan_range(ex, sb, sb + 1);
	else
		scan_range(ex, item * sh->morsel, Min((item + 1) * sh->morsel, sh->nsb));
}

bool
take_item(AggExec *ex, uint32 *item)
{
	*item = pg_atomic_fetch_add_u32(&ex->shared->next, 1);
	return *item < ex->shared->nitems;
}

/* the lanes that are next in order go into the result */
static void
lanes_combine(AggExec *ex)
{
	while (ex->next_lane < SIFT_AGG_LANES && ex->pending[ex->next_lane])
	{
		blob_merge(ex->gtab, ex->pending[ex->next_lane]);
		pfree(ex->pending[ex->next_lane]);
		ex->pending[ex->next_lane++] = NULL;
	}
}

static void
lanes_put(AggExec *ex, int lane, const char *data, Size len)
{
	ex->pending[lane] = MemoryContextAlloc(ex->cxt, len);
	memcpy(ex->pending[lane], data, len);
	lanes_combine(ex);
}

AggExec *
result_view(AggExec *ex)
{
	AggExec    *g = MemoryContextAlloc(ex->cxt, sizeof(AggExec));

	*g = *ex;
	g->lanes = false;
	g->tcxt = AllocSetContextCreate(ex->cxt, "sift agg result", ALLOCSET_DEFAULT_SIZES);
	g->ks = MemoryContextAlloc(ex->cxt, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	memcpy(g->ks, ex->ks, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	table_init(g);
	return g;
}

/* the check that avg(float8) owes (float_guard): the inputs of the whole scan could have overflowed the sum of squares */
void
final_checks(AggExec *g, int64 ntuples)
{
	for (int i = 0; i < g->spec->naggs; i++)
		if (g->spec->agg[i].kind == SAA_AVG_F64 && 8.0 * ntuples * g->amax[i] * g->amax[i] >= DBL_MAX)
			float_overflow_error();
}

/*
 * A scan whose plan asks for workers runs them under a parallel context of its own (not under a Gather: the partial tables are
 * not tuples). The leader and the workers take items from the counter in the shared memory; a worker sends its tables to the
 * leader through a shm_mq, as blobs, and the leader merges them. Lanes wait for the lanes before them.
 */
#define KEY_SHARED	UINT64CONST(0x53494654A001)
#define KEY_SPEC	UINT64CONST(0x53494654A002)
#define KEY_PARAMS	UINT64CONST(0x53494654A003)
#define KEY_SNAP	UINT64CONST(0x53494654A004)
#define KEY_MQ		UINT64CONST(0x53494654A005)
#define KEY_MAP		UINT64CONST(0x53494654B000)	/* + the combo: the table of its join (agg/lookshare.c) */
#define MQ_SIZE		(1 << 20)
/* the lookup tables the workers share, together: a larger set is read by the leader alone */
#define SHARE_MAX	((Size) 1 << 30)
#define DONE_MAGIC	0x53414732

typedef struct WorkerDone
{
	uint32		magic;
	uint32		pad;
	int64		n_units;
	int64		n_pre_units;
	int64		n_rollup_miss;
	int64		n_rollup_queued;
	int64		n_rollup_cost;
	int64		n_rows;
	int64		n_tuples;
	int64		n_dense;
	int64		n_matches;
	int64		n_skipped;
	SiftSrcCounters cc;
} WorkerDone;

static void
send_blob(shm_mq_handle *mqh, const void *data, Size len)
{
	if (shm_mq_send(mqh, len, data, false, true) != SHM_MQ_SUCCESS)
		ereport(ERROR, (errmsg("sift: the leader of a parallel aggregate went away")));
}

/* the table of the lane just scanned, as a blob: kept in order here, sent by a worker */
static void
lane_done(AggExec *ex, uint32 lane)
{
	StringInfoData b;

	initStringInfo(&b);
	blob_dump(ex, lane, &b);
	if (ex->mqh)
		send_blob(ex->mqh, b.data, b.len);
	else
		lanes_put(ex, lane, b.data, b.len);
	pfree(b.data);
	table_reset(ex);
}

/* the float sums of the lane just scanned are added to those of the lanes before it, as the leader adds the table of each lane */
void
lane_fold(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->dt)
			for (uint32 g = 0; g < n; g++)
			{
				a->dt[g] += a->d[g];
				a->d[g] = 0;
			}
	}
}

static void par_poll(AggExec *ex);

void
scan_items(AggExec *ex)
{
	uint32		item;

	while (!sift_aggbe_stopped(ex->be, ex->be_state) && take_item(ex, &item))
	{
		scan_item(ex, item);
		if (ex->lane_serial)
			lane_fold(ex);
		else if (ex->shared->ordered)
			lane_done(ex, item);
		if (ex->par)
			par_poll(ex);
	}
}

void
sift_agg_worker_main(dsm_segment *seg, shm_toc *toc)
{
	ScanShared *sh = shm_toc_lookup(toc, KEY_SHARED, false);
	char	   *params = shm_toc_lookup(toc, KEY_PARAMS, false);
	shm_mq	   *mq = (shm_mq *) ((char *) shm_toc_lookup(toc, KEY_MQ, false) + (Size) ParallelWorkerNumber * MQ_SIZE);
	AggExec    *ex = palloc0(sizeof(AggExec));
	Snapshot	snap = RegisterSnapshot(RestoreSnapshot(shm_toc_lookup(toc, KEY_SNAP, false)));
	WorkerDone	done = {DONE_MAGIC};

	shm_mq_set_sender(mq, MyProc);
	ex->mqh = shm_mq_attach(mq, seg, NULL);
	ex->spec = sift_aggspec_unpack((List *) stringToNode((char *) shm_toc_lookup(toc, KEY_SPEC, false)));
	ex->pmaps = palloc0(Max(ex->spec->ncombo, 1) * sizeof(char *));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
		ex->pmaps[ci] = shm_toc_lookup(toc, KEY_MAP + ci, true);
	engine_contexts(ex, CurrentMemoryContext);
	engine_rels(ex);
	ex->snap = snap;
	ex->partoff = ex->spec->nparts > 0 ? sh->partoff : NULL;
	engine_open(ex, snap, RestoreParamList(&params));
	ex->shared = sh;
	ex->lanes = sh->ordered;
	if (ex->topn)
	{
		sift_topn_share(ex->topn, &sh->topn_bound);
		sift_topn_visit(ex->topn, sh->nsb, sh->topn_reverse);
	}
	scan_buffers(ex);
	table_init(ex);
	if (ex->topn)
	{
		StringInfoData b;

		topn_items(ex);
		initStringInfo(&b);
		sift_topn_export(ex->topn, &b);
		send_blob(ex->mqh, b.data, b.len);
		done.n_skipped = sift_topn_skipped(ex->topn);
	}
	else
		scan_items(ex);
	if (!sh->ordered && !ex->topn)
	{
		StringInfoData b;

		initStringInfo(&b);
		blob_dump(ex, -1, &b);
		send_blob(ex->mqh, b.data, b.len);
	}
	done.n_units = ex->n_units;
	done.n_pre_units = ex->n_pre_units;
	done.n_rollup_miss = ex->n_rollup_miss;
	done.n_rollup_queued = ex->n_rollup_queued;
	done.n_rollup_cost = ex->n_rollup_cost;
	done.n_rows = ex->n_rows;
	done.n_tuples = ex->n_tuples;
	done.n_dense = ex->n_dense;
	done.n_matches = ex->search ? sift_search_matches(ex->search) : 0;
	sift_colsrc_counters(ex->src, &done.cc);
	send_blob(ex->mqh, &done, sizeof(done));
	shm_mq_detach(ex->mqh);
	engine_close(ex);
	UnregisterSnapshot(snap);
}

/* Starts the workers of the plan; the leader scans alone when none could be launched */
void
par_launch(AggExec *ex)
{
	ParallelContext *pcxt;
	char	   *spec_txt = nodeToString(ex->pack),
			   *p;
	Size		plen = EstimateParamListSpace(ex->params),
				slen = EstimateSnapshotSpace(ex->snap);
	int			nw = ex->spec->nworkers;
	ScanShared  *ps;
	AggPar	   *par;
	MapShared  *ml;
	Size		msize = 0;
	int			nmaps = 0;

	if (nw <= 0 || IsParallelWorker())
		return;
	/* the tables of the joins are the leader's: it has them (it ran the child plans), and a worker reads them from the shared memory */
	ml = palloc0(Max(ex->spec->ncombo, 1) * sizeof(MapShared));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		const SiftAggCombo *cb = &ex->spec->combo[ci];
		MapTab	   *m = ex->cs[ci].map;

		if (cb->join < 0)
			continue;
		if (m == NULL || m->keep || ex->spec->join[cb->join].params != NIL)
			return;
		msize += map_share_size(m, &ex->spec->join[cb->join], &ml[ci]);
		nmaps++;
	}
	if (msize > SHARE_MAX)
		return;
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_agg_worker_main", nw);
	shm_toc_estimate_chunk(&pcxt->estimator, shared_size(ex->spec, ex->nvorder));
	shm_toc_estimate_chunk(&pcxt->estimator, strlen(spec_txt) + 1);
	shm_toc_estimate_chunk(&pcxt->estimator, plen);
	shm_toc_estimate_chunk(&pcxt->estimator, slen);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(MQ_SIZE, nw));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
		if (ex->spec->combo[ci].join >= 0)
			shm_toc_estimate_chunk(&pcxt->estimator, ml[ci].size);
	shm_toc_estimate_keys(&pcxt->estimator, 5 + nmaps);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	ps = shm_toc_allocate(pcxt->toc, shared_size(ex->spec, ex->nvorder));
	shared_init(ps, ex);
	shm_toc_insert(pcxt->toc, KEY_SHARED, ps);
	p = shm_toc_allocate(pcxt->toc, strlen(spec_txt) + 1);
	strcpy(p, spec_txt);
	shm_toc_insert(pcxt->toc, KEY_SPEC, p);
	p = shm_toc_allocate(pcxt->toc, plen);
	shm_toc_insert(pcxt->toc, KEY_PARAMS, p);
	SerializeParamList(ex->params, &p);
	p = shm_toc_allocate(pcxt->toc, slen);
	SerializeSnapshot(ex->snap, p);
	shm_toc_insert(pcxt->toc, KEY_SNAP, p);
	p = shm_toc_allocate(pcxt->toc, mul_size(MQ_SIZE, nw));
	for (int i = 0; i < nw; i++)
		shm_mq_set_receiver(shm_mq_create(p + (Size) i * MQ_SIZE, MQ_SIZE), MyProc);
	shm_toc_insert(pcxt->toc, KEY_MQ, p);
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		const SiftAggCombo *cb = &ex->spec->combo[ci];
		char	   *mp;

		if (cb->join < 0)
			continue;
		mp = shm_toc_allocate(pcxt->toc, ml[ci].size);
		map_share_write(ex->cs[ci].map, &ex->spec->join[cb->join], &ml[ci], mp);
		shm_toc_insert(pcxt->toc, KEY_MAP + ci, mp);
		ex->lookup_shared += ml[ci].nrow;
	}
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	par = MemoryContextAllocZero(ex->cxt, sizeof(AggPar));
	par->pcxt = pcxt;
	par->nlaunched = pcxt->nworkers_launched;
	par->mqh = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(shm_mq_handle *));
	par->done = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(bool));
	for (int i = 0; i < par->nlaunched; i++)
	{
		par->mqh[i] = shm_mq_attach((shm_mq *) (p + (Size) i * MQ_SIZE), pcxt->seg, NULL);
		shm_mq_set_handle(par->mqh[i], pcxt->worker[i].bgwhandle);
	}
	ex->par = par;
	ex->shared = ps;
}

static void
par_message(AggExec *ex, int w, const char *data, Size len)
{
	AggPar	   *par = ex->par;

	if (*(const uint32 *) data == DONE_MAGIC)
	{
		const WorkerDone *d = (const WorkerDone *) data;

		ex->n_units += d->n_units;
		ex->n_pre_units += d->n_pre_units;
		ex->n_rollup_miss += d->n_rollup_miss;
		ex->n_rollup_queued += d->n_rollup_queued;
		ex->n_rollup_cost += d->n_rollup_cost;
		ex->n_rows += d->n_rows;
		ex->n_tuples += d->n_tuples;
		ex->n_dense += d->n_dense;
		if (ex->topn)
			sift_topn_add_skipped(ex->topn, d->n_skipped);
		par->matches += d->n_matches;
		par->cc.units_pruned += d->cc.units_pruned;
		par->cc.rows_vetted += d->cc.rows_vetted;
		par->cc.heap_pages += d->cc.heap_pages;
		par->cc.rechecked += d->cc.rechecked;
		par->cc.recheck_units += d->cc.recheck_units;
		par->done[w] = true;
		par->ndone++;
		shm_mq_detach(par->mqh[w]);
	}
	else if (ex->topn)
		sift_topn_import(ex->topn, data, len);
	else if (ex->shared->ordered)
	{
		const BlobHdr *h = (const BlobHdr *) data;

		if (h->magic != BLOB_MAGIC)
			elog(ERROR, "sift: bad message of a parallel aggregate");
		lanes_put(ex, h->lane, data, len);
	}
	else
		blob_merge(ex->gtab, data);
}

/* the messages that are there; whether there were any */
static bool
par_receive(AggExec *ex)
{
	AggPar	   *par = ex->par;
	bool		any = false;

	for (int w = 0; w < par->nlaunched; w++)
		while (!par->done[w])
		{
			Size		len;
			void	   *data;
			shm_mq_result r = shm_mq_receive(par->mqh[w], &len, &data, true);

			if (r == SHM_MQ_WOULD_BLOCK)
				break;
			if (r == SHM_MQ_DETACHED)
				ereport(ERROR, (errmsg("sift: a worker of a parallel aggregate stopped without its result")));
			par_message(ex, w, data, len);
			any = true;
		}
	return any;
}

static void
par_poll(AggExec *ex)
{
	par_receive(ex);
	CHECK_FOR_INTERRUPTS();
}

/* waits for the workers' tables, then for the workers */
void
par_finish(AggExec *ex)
{
	AggPar	   *par = ex->par;

	while (par->ndone < par->nlaunched)
	{
		if (!par_receive(ex))
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
		CHECK_FOR_INTERRUPTS();
	}
	WaitForParallelWorkersToFinish(par->pcxt);
	DestroyParallelContext(par->pcxt);
	par->pcxt = NULL;
	ExitParallelMode();
}
