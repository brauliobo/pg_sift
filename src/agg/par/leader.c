/*
 * agg/par/leader.c - the leader of a parallel scan
 * Reads the messages of the workers (tables, hits, counters), merges them and waits for the workers to finish.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

#include "agg/par/int.h"

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
			sift_topn_add_skipped(ex->topn, d->n_skipped, d->n_runs, d->n_lazy);
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
				sift_read_parallel_messages();
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

void
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
	par_forget_memo(ex);
	DestroyParallelContext(par->pcxt);
	par->pcxt = NULL;
	ExitParallelMode();
}
