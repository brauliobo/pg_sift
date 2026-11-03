/*
 * agg/par/worker.c - the entry point of a worker of a parallel scan
 * A worker opens the leader's statement from the shared memory, takes items, and sends its tables, hits and counters through its queue.
 */
#include "postgres.h"
#include "storage/proc.h"

#include "agg/agg.h"
#include "agg/rows.h"
#include "col/dictshm.h"
#include "agg/memo.h"
#include "agg/par/int.h"

static void
send_blob(shm_mq_handle *mqh, const void *data, Size len)
{
	if (shm_mq_send(mqh, len, data, false, true) != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("sift: the leader of a parallel aggregate went away"),
				 errhint("More details may be available in the server log.")));
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

	if (shm_toc_lookup(toc, KEY_DICTSHM, true) != NULL)
		sift_dictshm_join();
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
	/* the statements below may hold the nodes of this extension in their other sub-plans (SiftAgg scans, SiftAggRows) */
	RegisterCustomScanMethods(&sift_agg_plan_methods);
	RegisterCustomScanMethods(&sift_agg_rows_plan_methods);
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
	if (sh->gpart_n)
		split_begin(ex, ex->spec->gpart - 1, sh->gpart_n, 1u << (ParallelWorkerNumber + 1));
	if (ex->topn)
	{
		sift_topn_share(ex->topn, &sh->topn_bound);
		sift_topn_visit(ex->topn, sh->nsb, sh->topn_reverse);
	}
	scan_buffers(ex);
	table_init(ex);
	sets_fold_begin(ex);
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
		done.n_lazy = sift_topn_runs_lazy(ex->topn);
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
	/* the participants of a split scan all read every unit: the leader's counts are those of the scan, the workers' only the tuples they aggregated */
	if (sh->gpart_n)
		done = (WorkerDone) {.magic = DONE_MAGIC,.n_tuples = done.n_tuples,.n_dense = done.n_dense,.n_combo_trim = done.n_combo_trim,.n_matches = done.n_matches,
			.n_skipped = done.n_skipped,.n_runs = done.n_runs,.n_lazy = done.n_lazy};
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
