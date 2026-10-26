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
#include "agg/memo.h"

/*
 * The superblocks are split in items that the participants take from a counter. Float sums are order dependent, so that a result
 * does not depend on the number of workers: such a scan has SIFT_AGG_LANES items, lane l being the superblocks l, l + SIFT_AGG_LANES, ..., and
 * a participant keeps the float sums of each lane it scanned apart (AggAcc.lv), in the one table it scans into; the leader adds the lanes in lane
 * order, whoever scanned them. The other aggregates are exact in any order: items are MORSEL_SBS superblocks.
 */
#define MORSEL_SBS	2
#define MORSEL_SUPER_DIV	4

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

/* a serial scan of a table partitioned by (part of) the group key: an item is a partition, whose groups are final when its units end */
bool
part_flush_on(const AggExec *ex)
{
	return ex->spec->part_final && ex->spec->nparts > 0 && ex->spec->nworkers == 0;
}

void
shared_init(ScanShared *sh, AggExec *ex)
{
	sh->nsb = sift_colsrc_nsb(ex->src);
	if (ex->spec->nparts > 0)
		sift_aggpart_offsets(ex->src, sh->partoff);
	sh->ordered = sift_aggspec_ordered(ex->spec);
	sh->morsel = sift_aggspec_parallel_ok(ex->spec) ? MORSEL_SBS : Max(sh->nsb, 1);
	/* a superunit is taken whole by one item (a scan that has workers shares the span of four, so that the participants find more than two items) */
	if (ex->super_on)
		sh->morsel = Max(sh->morsel, sift_colsrc_super_span(ex->src) / (ex->spec->nworkers > 0 ? MORSEL_SUPER_DIV : 1));
	sh->nitems = sh->ordered ? SIFT_AGG_LANES : (sh->nsb + sh->morsel - 1) / sh->morsel;
	if (part_flush_on(ex))
		sh->nitems = ex->spec->nparts;
	pg_atomic_init_u32(&sh->next, 0);
	if (ex->topn)
		topn_shared_init(sh, ex);
}

/* The superblocks the zones and summaries of the source leave a unit in, for the scan: a range of none costs a few words, not a begin (colsrc.h kept) */
typedef struct SkipMap
{
	bool		ok;
	uint32		nsb;
	uint64	   *kept;
} SkipMap;

static SkipMap *
skip_map(AggExec *ex)
{
	SkipMap    *m = ex->skip;

	if (m == NULL)
	{
		uint32		nsb = sift_colsrc_nsb(ex->src);

		m = ex->skip = MemoryContextAllocZero(ex->cxt, sizeof(SkipMap));
		m->nsb = nsb;
		m->kept = MemoryContextAllocZero(ex->cxt, (nsb + 63) / 64 * sizeof(uint64));
		m->ok = sift_colsrc_kept(ex->src, ex->prune, ex->nzone, nsb, m->kept);
	}
	return m;
}

/* Is there no superblock of [lo, hi) the zones leave a unit in? */
static bool
range_skipped(AggExec *ex, uint32 lo, uint32 hi)
{
	SkipMap    *m = skip_map(ex);

	hi = Min(hi, m->nsb);
	if (!m->ok || lo >= hi)
		return false;
	for (uint32 w = lo / 64; w < (hi + 63) / 64; w++)
	{
		uint64		mask = ~UINT64CONST(0);

		if (w == lo / 64)
			mask &= ~UINT64CONST(0) << (lo % 64);
		if (w == (hi - 1) / 64 && hi % 64)
			mask &= (UINT64CONST(1) << (hi % 64)) - 1;
		if (m->kept[w] & mask)
			return false;
	}
	return true;
}

/* The first superblock from sb, a step apart, that the zones leave a unit in (nsb or more when there is none; sb itself without the map) */
static uint32
next_kept(AggExec *ex, uint32 sb, uint32 step)
{
	SkipMap    *m = skip_map(ex);

	if (!m->ok)
		return sb;
	while (sb < m->nsb)
	{
		uint64		rest = m->kept[sb / 64] >> (sb % 64);

		if (rest & 1)
			return sb;
		if (rest == 0)			/* nothing is kept to the end of the word: the first superblock of the next word on the same step */
			sb += ((sb / 64 + 1) * 64 - sb + step - 1) / step * step;
		else
			sb += step;
	}
	return sb;
}

void
scan_units(AggExec *ex, uint32 lo, uint32 hi)
{
	lo = Max(lo, ex->tid_sb_lo);
	hi = Min(hi, ex->tid_sb_hi);
	if (lo >= hi || range_skipped(ex, lo, hi))
		return;
	sift_colsrc_begin(ex->src, lo, hi, ex->prune, ex->nzone);
	while (sift_colsrc_next(ex->src, &ex->unit))
	{
		process_unit(ex);
		if (ex->spill)
			spill_check(ex);
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
	if (!ex->sb_heap)
	{
		/* units that are not superblocks of the heap hold rows of any block: the match set is the whole table's, made once by a participant */
		if (ex->search_all == NULL)
			ex->search_all = sift_search_range(ex->search, 0, sift_colsrc_nsb(ex->src));
		sift_colsrc_select(ex->src, ex->search_all);
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

	if (part_flush_on(ex))
		scan_range(ex, sh->partoff[item], sh->partoff[item + 1]);
	else if (sh->ordered)
		for (uint32 sb = next_kept(ex, item, SIFT_AGG_LANES); sb < sh->nsb; sb = next_kept(ex, sb + SIFT_AGG_LANES, SIFT_AGG_LANES))
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

/* the check that avg(float8) owes (float_guard): the inputs of the whole scan could have overflowed the sum of squares */
void
final_checks(AggExec *g, int64 ntuples)
{
	for (int i = 0; i < g->spec->naggs; i++)
		if (g->spec->agg[i].kind == SAA_AVG_F64 && 8.0 * ntuples * g->amax[i] * g->amax[i] >= DBL_MAX)
			float_overflow_error();
	for (int k = 0; k < g->spec->narms; k++)
		for (int i = 0; i < g->spec->arm[k].ninner; i++)
		{
			SiftAggKind kind = g->spec->arm[k].inner[i].kind;

			if ((kind == SAA_AVG_F64 || kind == SAA_X_AVG) && 8.0 * ntuples * g->arm[k].amax[i] * g->arm[k].amax[i] >= DBL_MAX)
				float_overflow_error();
		}
}

/*
 * A scan whose plan asks for workers runs them under a parallel context of its own (not under a Gather: the partial tables are
 * not tuples). The leader and the workers take items from the counter in the shared memory; a worker sends its tables to the
 * leader through a shm_mq, as one blob each, and the leader merges them.
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
#define HITS_MAGIC	0x53414733
#define SPINE_MAGIC	0x53414734
#define KEY_SPINE	UINT64CONST(0x53494654A006)
#define KEY_CHILD	UINT64CONST(0x53494654C000)	/* + the join: the statement whose plan is its correlated child */
#define KEY_SUB	UINT64CONST(0x53494654E000)	/* the statement whose sub-plans the expressions of the spec start */
#define KEY_FMAP	UINT64CONST(0x53494654F000)	/* + the combo: the table of a hybrid first-row lookup (agg/first.c) */
#define KEY_MEMO	UINT64CONST(0x53494654D000)	/* + the join: the rows of its child by parameter values (agg/memo.c) */

/* the rows of a preserved side that a worker's tuples matched: nrow flags of the table of combo ci */
typedef struct WorkerHits
{
	uint32		magic;
	uint32		ci;
	uint32		nrow;
} WorkerHits;

typedef struct WorkerDone
{
	uint32		magic;
	uint32		pad;
	int64		n_units;
	int64		n_pre_units;
	int64		n_super;
	int64		n_rollup_miss;
	int64		n_exc_units;
	int64		n_exc_scan;
	int64		n_dead_units;
	int64		n_dead_scan;
	int64		n_rollup_queued;
	int64		n_rollup_cost;
	int64		n_rows;
	int64		n_tuples;
	int64		n_dense;
	int64		n_combo_trim;
	int64		n_matches;
	int64		n_skipped;
	int64		n_runs;
	SiftSrcCounters cc;
} WorkerDone;

static void
send_blob(shm_mq_handle *mqh, const void *data, Size len)
{
	if (shm_mq_send(mqh, len, data, false, true) != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("sift: the leader of a parallel aggregate went away"),
				 errhint("More details may be available in the server log.")));
}

/* the float sums of the lane just scanned are added to those of the lanes before it, as the leader adds the table of each lane */
void
lane_fold(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		double	   *pg_restrict dt = ex->acc[i].dt;
		const double *pg_restrict d = ex->acc[i].d;
		uint32		g = 0;

		if (dt == NULL)
			continue;
		/* the pointers in locals: through the accumulator every store would reload them and keep the loop scalar */
		for (; g + 4 <= n; g += 4)
		{
			dt[g] += d[g];
			dt[g + 1] += d[g + 1];
			dt[g + 2] += d[g + 2];
			dt[g + 3] += d[g + 3];
		}
		for (; g < n; g++)
			dt[g] += d[g];
		memset(ex->acc[i].d, 0, n * sizeof(double));
	}
}

/* the float sums of the lane just scanned are kept apart: the lanes are added in order when every process is done (lanes_sum) */
static void
lane_keep(AggExec *ex, uint32 lane)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->lv == NULL)
			continue;
		a->lv[lane] = MemoryContextAllocZero(ex->tcxt, ex->gcap * sizeof(double));
		memcpy(a->lv[lane], a->d, n * sizeof(double));
		memset(a->d, 0, n * sizeof(double));
	}
}

/* the sums of the lanes, in lane order, then the sums of the rows after the lanes (the tail of a preserved side) */
void
lanes_sum(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->lv == NULL)
			continue;
		for (uint32 g = 0; g < n; g++)
		{
			double		t = 0;

			for (int l = 0; l < SIFT_AGG_LANES; l++)
				if (a->lv[l])
					t += a->lv[l][g];
			a->d[g] = t + a->d[g];
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
		/* the cells of the last row are groups of the item's table */
		arms_flush(ex);
		if (part_flush_on(ex))
			part_flush(ex);
		if (ex->lane_serial)
			lane_fold(ex);
		else if (ex->lanes)
			lane_keep(ex, item);
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
	ParamListInfo wparams;
	char	   *subtxt;

	shm_mq_set_sender(mq, MyProc);
	ex->mqh = shm_mq_attach(mq, seg, NULL);
	ex->spec = sift_aggspec_unpack((List *) stringToNode((char *) shm_toc_lookup(toc, KEY_SPEC, false)));
	ex->pmaps = palloc0(Max(ex->spec->ncombo, 1) * sizeof(char *));
	ex->pfmaps = palloc0(Max(ex->spec->ncombo, 1) * sizeof(char *));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		ex->pmaps[ci] = shm_toc_lookup(toc, KEY_MAP + ci, true);
		ex->pfmaps[ci] = shm_toc_lookup(toc, KEY_FMAP + ci, true);
	}
	engine_contexts(ex, CurrentMemoryContext);
	engine_rels(ex);
	ex->snap = snap;
	/* the executor states below start with the snapshot of the leader's statement as the active one (ExecutorStart asserts it), as in a backend */
	PushActiveSnapshot(snap);
	ex->partoff = ex->spec->nparts > 0 ? sh->partoff : NULL;
	wparams = RestoreParamList(&params);
	/* the statements below may hold scans of this kind in their other sub-plans */
	RegisterCustomScanMethods(&sift_agg_plan_methods);
	/* the sub-plans of the expressions (a hashed IN (SELECT ..)) start in an executor state of the statement, whose plan has no node of its own */
	if ((subtxt = shm_toc_lookup(toc, KEY_SUB, true)) != NULL)
	{
		ex->wsub = CreateQueryDesc((PlannedStmt *) stringToNode(subtxt), "", snap, InvalidSnapshot, None_Receiver, wparams, NULL, 0);
		standard_ExecutorStart(ex->wsub, 0);
		ex->css.ss.ps.state = ex->wsub->estate;
	}
	engine_open(ex, snap, wparams);
	ex->wchild = palloc0(Max(ex->spec->njoin, 1) * sizeof(QueryDesc *));
	ex->cmemo = palloc0(Max(ex->spec->njoin, 1) * sizeof(ChildMemo *));
	ex->cmslot = palloc0(Max(ex->spec->njoin, 1) * sizeof(TupleTableSlot *));
	for (int ji = 0; ji < ex->spec->njoin; ji++)
	{
		char	   *txt = shm_toc_lookup(toc, KEY_CHILD + ji, true);

		if (txt == NULL)
			continue;
		ex->wchild[ji] = CreateQueryDesc((PlannedStmt *) stringToNode(txt), "", snap, InvalidSnapshot, None_Receiver, wparams, NULL, 0);
		standard_ExecutorStart(ex->wchild[ji], 0);
		ex->cmemo[ji] = shm_toc_lookup(toc, KEY_MEMO + ji, true);
	}
	ex->shared = sh;
	ex->lanes = sh->ordered;
	if (ex->topn)
	{
		sift_topn_share(ex->topn, &sh->topn_bound);
		sift_topn_visit(ex->topn, sh->nsb, sh->topn_reverse);
	}
	scan_buffers(ex);
	table_init(ex);
	if (ex->spec->spine)
	{
		ex->spine_buf = shm_toc_lookup(toc, KEY_SPINE, false);
		spine_load(ex);
	}
	if (ex->topn)
	{
		StringInfoData b;

		topn_items(ex);
		initStringInfo(&b);
		sift_topn_export(ex->topn, &b);
		send_blob(ex->mqh, b.data, b.len);
		done.n_skipped = sift_topn_skipped(ex->topn);
		done.n_runs = sift_topn_runs_read(ex->topn);
	}
	else
		scan_items(ex);
	if (!ex->topn)
	{
		StringInfoData b;

		initStringInfo(&b);
		blob_dump(ex, -1, &b);
		send_blob(ex->mqh, b.data, b.len);
	}
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		const MapTab *m = ex->cs[ci].map;
		WorkerHits	h = {HITS_MAGIC, ci, 0};
		char	   *msg;

		if (m == NULL || !m->keep)
			continue;
		h.nrow = m->nrow;
		msg = palloc(sizeof(h) + h.nrow);
		memcpy(msg, &h, sizeof(h));
		memcpy(msg + sizeof(h), m->hit, h.nrow);
		send_blob(ex->mqh, msg, sizeof(h) + h.nrow);
		pfree(msg);
	}
	if (ex->spine)
	{
		WorkerHits	h = {SPINE_MAGIC, 0, ex->spine->nrow};
		char	   *msg = palloc(sizeof(h) + h.nrow);

		memcpy(msg, &h, sizeof(h));
		memcpy(msg + sizeof(h), ex->spine->hit, h.nrow);
		send_blob(ex->mqh, msg, sizeof(h) + h.nrow);
		pfree(msg);
	}
	done.n_units = ex->n_units;
	done.n_pre_units = ex->n_pre_units;
	done.n_super = ex->n_super;
	done.n_rollup_miss = ex->n_rollup_miss;
	done.n_exc_units = ex->n_exc_units;
	done.n_exc_scan = ex->n_exc_scan;
	done.n_dead_units = ex->n_dead_units;
	done.n_dead_scan = ex->n_dead_scan;
	done.n_rollup_queued = ex->n_rollup_queued;
	done.n_rollup_cost = ex->n_rollup_cost;
	done.n_rows = ex->n_rows;
	done.n_tuples = ex->n_tuples;
	done.n_dense = ex->n_dense;
	done.n_combo_trim = ex->n_combo_trim;
	done.n_matches = ex->search ? sift_search_matches(ex->search) : 0;
	sift_colsrc_counters(ex->src, &done.cc);
	send_blob(ex->mqh, &done, sizeof(done));
	shm_mq_detach(ex->mqh);
	/* before the executor states: the expression context of the lookups is a child of the state of the sub-plans */
	engine_close(ex);
	for (int ji = 0; ji < ex->spec->njoin; ji++)
		if (ex->wchild[ji])
		{
			ExecutorFinish(ex->wchild[ji]);
			ExecutorEnd(ex->wchild[ji]);
			FreeQueryDesc(ex->wchild[ji]);
		}
	if (ex->wsub)
	{
		ExecutorFinish(ex->wsub);
		ExecutorEnd(ex->wsub);
		FreeQueryDesc(ex->wsub);
	}
	PopActiveSnapshot();
	UnregisterSnapshot(snap);
}

/* does the packed spec (its expressions are Nodes among Integers and Strings) start a sub-plan? */
static bool
pack_has_subplan(Node *n, void *context)
{
	if (n == NULL || IsA(n, Integer) || IsA(n, String) || IsA(n, IntList) || IsA(n, OidList))
		return false;
	return IsA(n, SubPlan) || expression_tree_walker(n, pack_has_subplan, context);
}

/* the statement of the query with `tree` as its plan: what a worker starts (no row marks, no target tables, a plain SELECT) */
static PlannedStmt *
select_stmt(AggExec *ex, Plan *tree)
{
	PlannedStmt *ps = makeNode(PlannedStmt);

	*ps = *ex->css.ss.ps.state->es_plannedstmt;
	ps->planTree = tree;
	ps->rowMarks = NIL;
#if PG_VERSION_NUM >= 190000
	ps->resultRelationRelids = NULL;
#else
	ps->resultRelations = NIL;
#endif
	ps->utilityStmt = NULL;
	ps->commandType = CMD_SELECT;
	ps->canSetTag = false;
	return ps;
}

/* Starts the workers of the plan; the leader scans alone when none could be launched */
void
par_launch(AggExec *ex)
{
	ParallelContext *pcxt;
	char	   *spec_txt,
			   *p;
	Size		plen = EstimateParamListSpace(ex->params),
				slen = EstimateSnapshotSpace(ex->snap);
	int			nw = ex->spec->nworkers;
	ScanShared  *ps;
	AggPar	   *par;
	MapShared  *ml,
			   *mlf;
	Size		msize = 0;
	int			nmaps = 0,
				ncorr = 0;
	char	  **child_txt;
	char	   *sub_txt = NULL;

	if (nw <= 0 || IsParallelWorker())
		return;
	spec_txt = nodeToString(ex->pack);
	/* the rows of the spine are those of a plan the leader runs once, before the workers start: they read the rows from the shared memory */
	if (ex->spec->spine)
	{
		spine_fetch(ex);
		if (((const SpineHdr *) ex->spine_buf)->len > SHARE_MAX)
			return;
	}
	/* the plan of a correlated join, as a statement of its own: each worker starts it and runs it for the inputs it meets */
	child_txt = palloc0(Max(ex->spec->njoin, 1) * sizeof(char *));
	ex->cmemo = MemoryContextAllocZero(ex->cxt, Max(ex->spec->njoin, 1) * sizeof(ChildMemo *));
	ex->cmslot = MemoryContextAllocZero(ex->cxt, Max(ex->spec->njoin, 1) * sizeof(TupleTableSlot *));
	for (int ji = 0; ji < ex->spec->njoin; ji++)
		if (ex->spec->join[ji].params != NIL)
		{
			child_txt[ji] = nodeToString(select_stmt(ex, list_nth(ex->css.ss.ps.state->es_plannedstmt->subplans, ex->spec->join[ji].subplan - 1)));
			ncorr++;
		}
	/* the sub-plans of the expressions start in each worker (they hash a small table once): a statement of the query's sub-plans with no node of its own */
	if (pack_has_subplan((Node *) ex->pack, NULL))
		sub_txt = nodeToString(select_stmt(ex, (Plan *) makeNode(Result)));
	/* the tables of the joins are the leader's: it has them (it ran the child plans), and a worker reads them from the shared memory */
	ml = palloc0(Max(ex->spec->ncombo, 1) * sizeof(MapShared));
	mlf = palloc0(Max(ex->spec->ncombo, 1) * sizeof(MapShared));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		const SiftAggCombo *cb = &ex->spec->combo[ci];
		MapTab	   *m = ex->cs[ci].map;

		if (cb->join < 0)
			continue;
		/* a correlated child runs per worker, for the inputs the worker meets (its rows are the worker's own); the table of a first-row lookup is the leader's */
		if (ex->spec->join[cb->join].params != NIL)
		{
			if (ex->fjoin && ex->fjoin[cb->join] && ex->cs[ci].fmap)
			{
				msize += map_share_size(ex->cs[ci].fmap, ex->fjoin[cb->join], &mlf[ci]);
				nmaps++;
			}
			continue;
		}
		if (m == NULL)
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
		if (ex->spec->combo[ci].join >= 0 && ex->spec->join[ex->spec->combo[ci].join].params == NIL)
			shm_toc_estimate_chunk(&pcxt->estimator, ml[ci].size);
		else if (ex->spec->combo[ci].join >= 0 && ex->fjoin && ex->fjoin[ex->spec->combo[ci].join] && ex->cs[ci].fmap)
			shm_toc_estimate_chunk(&pcxt->estimator, mlf[ci].size);
	for (int ji = 0; ji < ex->spec->njoin; ji++)
		if (child_txt[ji])
		{
			shm_toc_estimate_chunk(&pcxt->estimator, strlen(child_txt[ji]) + 1);
			shm_toc_estimate_chunk(&pcxt->estimator, memo_size());
		}
	if (ex->spec->spine)
		shm_toc_estimate_chunk(&pcxt->estimator, ((const SpineHdr *) ex->spine_buf)->len);
	if (sub_txt)
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(sub_txt) + 1);
	shm_toc_estimate_keys(&pcxt->estimator, 5 + nmaps + (ex->spec->spine != NULL) + 2 * ncorr + (sub_txt != NULL));
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
	for (int ji = 0; ji < ex->spec->njoin; ji++)
		if (child_txt[ji])
		{
			char	   *cm = shm_toc_allocate(pcxt->toc, strlen(child_txt[ji]) + 1);

			strcpy(cm, child_txt[ji]);
			shm_toc_insert(pcxt->toc, KEY_CHILD + ji, cm);
			ex->cmemo[ji] = shm_toc_allocate(pcxt->toc, memo_size());
			memo_init(ex->cmemo[ji]);
			shm_toc_insert(pcxt->toc, KEY_MEMO + ji, ex->cmemo[ji]);
		}
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		const SiftAggCombo *cb = &ex->spec->combo[ci];
		char	   *mp;

		if (cb->join >= 0 && ex->spec->join[cb->join].params != NIL && ex->fjoin && ex->fjoin[cb->join] && ex->cs[ci].fmap)
		{
			mp = shm_toc_allocate(pcxt->toc, mlf[ci].size);
			map_share_write(ex->cs[ci].fmap, ex->fjoin[cb->join], &mlf[ci], mp);
			shm_toc_insert(pcxt->toc, KEY_FMAP + ci, mp);
			ex->lookup_shared += mlf[ci].nrow;
			continue;
		}
		if (cb->join < 0 || ex->spec->join[cb->join].params != NIL)
			continue;
		mp = shm_toc_allocate(pcxt->toc, ml[ci].size);
		map_share_write(ex->cs[ci].map, &ex->spec->join[cb->join], &ml[ci], mp);
		shm_toc_insert(pcxt->toc, KEY_MAP + ci, mp);
		ex->lookup_shared += ml[ci].nrow;
	}
	if (sub_txt)
	{
		char	   *sm = shm_toc_allocate(pcxt->toc, strlen(sub_txt) + 1);

		strcpy(sm, sub_txt);
		shm_toc_insert(pcxt->toc, KEY_SUB, sm);
	}
	if (ex->spec->spine)
	{
		Size		len = ((const SpineHdr *) ex->spine_buf)->len;
		char	   *sm = shm_toc_allocate(pcxt->toc, len);

		memcpy(sm, ex->spine_buf, len);
		shm_toc_insert(pcxt->toc, KEY_SPINE, sm);
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
		ex->n_super += d->n_super;
		ex->n_rollup_miss += d->n_rollup_miss;
		ex->n_exc_units += d->n_exc_units;
		ex->n_exc_scan += d->n_exc_scan;
		ex->n_dead_units += d->n_dead_units;
		ex->n_dead_scan += d->n_dead_scan;
		ex->n_rollup_queued += d->n_rollup_queued;
		ex->n_rollup_cost += d->n_rollup_cost;
		ex->n_rows += d->n_rows;
		ex->n_tuples += d->n_tuples;
		ex->n_dense += d->n_dense;
		ex->n_combo_trim += d->n_combo_trim;
		if (ex->topn)
			sift_topn_add_skipped(ex->topn, d->n_skipped, d->n_runs);
		par->matches += d->n_matches;
		par->cc.units_pruned += d->cc.units_pruned;
		par->cc.units_set_pruned += d->cc.units_set_pruned;
		par->cc.units_search_pruned += d->cc.units_search_pruned;
		par->cc.rows_vetted += d->cc.rows_vetted;
		par->cc.heap_pages += d->cc.heap_pages;
		par->cc.rechecked += d->cc.rechecked;
		par->cc.recheck_units += d->cc.recheck_units;
		par->cc.point_units += d->cc.point_units;
		par->cc.point_rows += d->cc.point_rows;
		par->cc.run_windows += d->cc.run_windows;
		par->done[w] = true;
		par->ndone++;
		shm_mq_detach(par->mqh[w]);
	}
	else if (*(const uint32 *) data == SPINE_MAGIC)
	{
		const WorkerHits *h = (const WorkerHits *) data;

		SIFT_INVARIANT(ex->spine != NULL && ex->spine->nrow == h->nrow, "sift: bad message of a parallel aggregate");
		for (uint32 r = 0; r < h->nrow; r++)
			ex->spine->hit[r] |= ((const char *) (h + 1))[r];
	}
	else if (*(const uint32 *) data == HITS_MAGIC)
	{
		const WorkerHits *h = (const WorkerHits *) data;
		MapTab	   *m = ex->cs[h->ci].map;

		SIFT_INVARIANT(m != NULL && m->keep && m->nrow == h->nrow, "sift: bad message of a parallel aggregate");
		for (uint32 r = 0; r < h->nrow; r++)
			m->hit[r] |= ((const char *) (h + 1))[r];
	}
	else if (ex->topn)
		sift_topn_import(ex->topn, data, len);
	else
		blob_merge(ex, data);
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
			{
				/* a worker that failed sent its error before it exited, but the signal that makes the interrupt raise it may not have been handled yet: read the error queues now, so that the error of the worker is the one raised */
#if PG_VERSION_NUM >= 180000
				ProcessParallelMessages();
#else
				HandleParallelMessages();
#endif
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("sift: a worker of a parallel aggregate stopped without its result"),
						 errhint("More details may be available in the server log.")));
			}
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
