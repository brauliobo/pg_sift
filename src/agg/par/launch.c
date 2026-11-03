/*
 * agg/par/launch.c - the start of the workers of a parallel scan
 * The shared memory the workers read (spec, parameters, snapshot, lookup tables, child plans) and the launch of the parallel context.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "storage/proc.h"

#include "agg/agg.h"
#include "col/dictshm.h"
#include "agg/memo.h"
#include "agg/par/int.h"

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
	sift_clear_result_relations(ps);
	ps->utilityStmt = NULL;
	ps->commandType = CMD_SELECT;
	ps->canSetTag = false;
	return ps;
}

/*
 * The units the zones and the keys of the lookups leave are known when the workers start. A scan whose workers the planner priced for the rows it
 * reads (workers_ratio: PostgreSQL counts the join of its lookups at a handful of rows) keeps one process when the units it keeps hold fewer rows than
 * sift.agg_par_min_rows: a few rows take less than the workers do to start and merge, and the lists the planner has no statistics for may keep
 * a handful of units. 300,000 rows of 900 bytes, 60 units kept (65,000 rows): 40 ms alone, 58 with 4 workers; 276 units: 493 and 328; 101,000 rows
 * of unnested documents: 990 ms alone, 750 with 4. The rows are those of the units kept, not their number times the average unit of the index: units
 * cut by a key hold a few hundred rows for a small key and hundreds of thousands for a large one.
 */
int			sift_agg_par_min_rows = 80000;

static bool
par_scan_small(AggExec *ex)
{
	SkipMap    *m;

	if (ex->spec->wide_unit_rows <= 0)
		return false;
	m = skip_map(ex);
	return m->ok && m->rows < sift_agg_par_min_rows;
}

/* The shared child rows live in the segment of the parallel context: the leader reads none of them once it is destroyed */
void
par_forget_memo(AggExec *ex)
{
	memset(ex->cmemo, 0, Max(ex->spec->njoin, 1) * sizeof(ChildMemo *));
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
	bool		dictshm;
	int			nmaps = 0,
				ncorr = 0;
	char	  **child_txt;
	char	   *sub_txt = NULL;

	/* a scan run again starts from none */
	ex->gp_n = ex->gp_mine = 0;
	if (nw <= 0 || IsParallelWorker() || par_scan_small(ex))
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
	dictshm = sift_dictshm_prepare();
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
	if (dictshm)
		shm_toc_estimate_chunk(&pcxt->estimator, 1);
	shm_toc_estimate_keys(&pcxt->estimator, 5 + nmaps + (ex->spec->spine != NULL) + 2 * ncorr + (sub_txt != NULL) + dictshm);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	if (dictshm)
		shm_toc_insert(pcxt->toc, KEY_DICTSHM, shm_toc_allocate(pcxt->toc, 1));
	ps = shm_toc_allocate(pcxt->toc, shared_size(ex->spec, ex->nvorder));
	shared_init(ps, ex);
	/* the participants split the groups by the key the plan chose when the scan can: a worker takes the share of its number + 1, the leader that of the workers that did not start too */
	ps->gpart_n = split_run_ok(ex) ? nw + 1 : 0;
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
		par_forget_memo(ex);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	if (ps->gpart_n)
		split_begin(ex, ex->spec->gpart - 1, ps->gpart_n, ((UINT64CONST(1) << ps->gpart_n) - 1) & ~(((UINT64CONST(1) << (pcxt->nworkers_launched + 1)) - 1) & ~UINT64CONST(1)));
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
