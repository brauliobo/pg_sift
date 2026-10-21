/*
 * core/parbuild.c - the parallel context of a parallel CREATE INDEX (core/parbuild.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"

#include "core/parbuild.h"

#define KEY_SHARED		UINT64CONST(0x5100000000000001)
#define KEY_PARTS		UINT64CONST(0x5100000000000002)
#define KEY_WAL_USAGE	UINT64CONST(0x5100000000000003)
#define KEY_BUFFER_USAGE UINT64CONST(0x5100000000000004)
#define KEY_SNAPSHOT	UINT64CONST(0x5100000000000005)
#define KEY_QUERY_TEXT	UINT64CONST(0x5100000000000006)

#ifdef SIFT_TESTING
int			sift_test_par_workers = 0;
#endif

SiftParLeader *
sift_par_enter(bool concurrent)
{
	SiftParLeader *ldr = palloc0(sizeof(SiftParLeader));

	ldr->snapshot = concurrent ? RegisterSnapshot(GetTransactionSnapshot()) : SnapshotAny;
	EnterParallelMode();
	return ldr;
}

bool
sift_par_setup(SiftParLeader *ldr, const char *worker, int nworkers, Size shared_size, Size parts_size, bool ship_snapshot)
{
	ParallelContext *pcxt = ldr->pcxt = CreateParallelContext("pg_sift", worker, nworkers);
	Size		snaplen = ship_snapshot ? EstimateSnapshotSpace(ldr->snapshot) : 0;
	int			nkeys = 4 + (snaplen != 0) + (debug_query_string != NULL);

	shm_toc_estimate_chunk(&pcxt->estimator, shared_size);
	shm_toc_estimate_chunk(&pcxt->estimator, parts_size);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(WalUsage), nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(BufferUsage), nworkers));
	if (snaplen)
		shm_toc_estimate_chunk(&pcxt->estimator, snaplen);
	if (debug_query_string)
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(debug_query_string) + 1);
	shm_toc_estimate_keys(&pcxt->estimator, nkeys);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		sift_par_end(ldr);
		return false;
	}
	ldr->shared = shm_toc_allocate(pcxt->toc, shared_size);
	ldr->parts = shm_toc_allocate(pcxt->toc, parts_size);
	ldr->walusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(WalUsage), nworkers));
	ldr->bufferusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(BufferUsage), nworkers));
	shm_toc_insert(pcxt->toc, KEY_SHARED, ldr->shared);
	shm_toc_insert(pcxt->toc, KEY_PARTS, ldr->parts);
	shm_toc_insert(pcxt->toc, KEY_WAL_USAGE, ldr->walusage);
	shm_toc_insert(pcxt->toc, KEY_BUFFER_USAGE, ldr->bufferusage);
	if (snaplen)
	{
		char	   *snap = shm_toc_allocate(pcxt->toc, snaplen);

		SerializeSnapshot(ldr->snapshot, snap);
		shm_toc_insert(pcxt->toc, KEY_SNAPSHOT, snap);
	}
	if (debug_query_string)
	{
		char	   *query = shm_toc_allocate(pcxt->toc, strlen(debug_query_string) + 1);

		strcpy(query, debug_query_string);
		shm_toc_insert(pcxt->toc, KEY_QUERY_TEXT, query);
	}
	return true;
}

bool
sift_par_launch(SiftParLeader *ldr, const char *what, Relation index, int nworkers)
{
	ParallelContext *pcxt = ldr->pcxt;

	LaunchParallelWorkers(pcxt);
#ifdef SIFT_TESTING
	sift_test_par_workers = pcxt->nworkers_launched;
#endif
	if (pcxt->nworkers_launched == 0)
	{
		ereport(DEBUG1, errmsg("building %s index \"%s\" without parallel workers: none could be launched", what,
							   RelationGetRelationName(index)));
		sift_par_end(ldr);
		return false;
	}
	WaitForParallelWorkersToAttach(pcxt);
	ereport(DEBUG1, errmsg("building %s index \"%s\" with %d parallel workers", what, RelationGetRelationName(index),
						   pcxt->nworkers_launched));
	if (pcxt->nworkers_launched < nworkers)
		ereport(DEBUG1, errmsg("%s index \"%s\": %d of the %d parallel workers requested could not be launched", what,
							   RelationGetRelationName(index), nworkers - pcxt->nworkers_launched, nworkers));
	return true;
}

void
sift_par_wait(SiftParLeader *ldr)
{
	WaitForParallelWorkersToFinish(ldr->pcxt);
	for (int w = 0; w < ldr->pcxt->nworkers_launched; w++)
		InstrAccumParallelQuery(&ldr->bufferusage[w], &ldr->walusage[w]);
}

void
sift_par_end(SiftParLeader *ldr)
{
	DestroyParallelContext(ldr->pcxt);
	ExitParallelMode();
	if (IsMVCCSnapshot(ldr->snapshot))
		UnregisterSnapshot(ldr->snapshot);
}

void
sift_par_worker_begin(SiftParWorker *w, shm_toc *toc)
{
	SiftParCommon *common;
	char	   *snap = shm_toc_lookup(toc, KEY_SNAPSHOT, true);

	w->toc = toc;
	w->shared = shm_toc_lookup(toc, KEY_SHARED, false);
	w->parts = shm_toc_lookup(toc, KEY_PARTS, false);
	common = w->shared;
	w->heaplock = common->isconcurrent ? ShareUpdateExclusiveLock : ShareLock;
	w->indexlock = common->isconcurrent ? RowExclusiveLock : AccessExclusiveLock;
	debug_query_string = shm_toc_lookup(toc, KEY_QUERY_TEXT, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);
	w->heap = table_open(common->heaprelid, w->heaplock);
	w->index = index_open(common->indexrelid, w->indexlock);
	w->info = BuildIndexInfo(w->index);
	w->info->ii_Concurrent = common->isconcurrent;
	w->restored = snap != NULL;
	w->snapshot = snap ? RegisterSnapshot(RestoreSnapshot(snap)) : SnapshotAny;
	InstrStartParallelQuery();
}

void
sift_par_worker_end(SiftParWorker *w)
{
	InstrEndParallelQuery(&((BufferUsage *) shm_toc_lookup(w->toc, KEY_BUFFER_USAGE, false))[ParallelWorkerNumber],
						  &((WalUsage *) shm_toc_lookup(w->toc, KEY_WAL_USAGE, false))[ParallelWorkerNumber]);
	if (w->restored)
		UnregisterSnapshot(w->snapshot);
	index_close(w->index, w->indexlock);
	table_close(w->heap, w->heaplock);
}
