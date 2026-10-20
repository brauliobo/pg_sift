/*
 * sift_parbuild.h - the parallel context of a parallel CREATE INDEX, shared by the BM25 build (bm25/build.c) and the column build
 * (sift_col_build_par.c): the leader's set-up of the dynamic shared memory (shared state, the workers' parts, their WAL and buffer
 * usage, the snapshot of a concurrent build, the query text), the launch, and the opening of the relations in a worker.
 *
 * The shared struct of a build starts with a SiftParCommon; the build allocates its parts as the workers' slots in one chunk.
 */
#ifndef SIFT_PARBUILD_H
#define SIFT_PARBUILD_H

#include "access/parallel.h"
#include "executor/instrument.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

typedef struct SiftParCommon
{
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
} SiftParCommon;

typedef struct SiftParLeader
{
	ParallelContext *pcxt;
	Snapshot	snapshot;
	void	   *shared;			/* shared_size bytes, for the build to initialize before sift_par_launch */
	void	   *parts;			/* parts_size bytes */
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} SiftParLeader;

/* Enters parallel mode with the snapshot of the build (the transaction's for a concurrent one, else none): table_parallelscan_estimate needs it */
extern SiftParLeader *sift_par_enter(bool concurrent);

/*
 * Creates the context of the function `worker` of pg_sift for nworkers workers, and its memory. False, with the context ended, when there is no
 * memory for it. ship_snapshot: the snapshot of a concurrent build goes to the workers.
 */
extern bool sift_par_setup(SiftParLeader *ldr, const char *worker, int nworkers, Size shared_size, Size parts_size, bool ship_snapshot);

/* Starts the workers; false, with the context left, when none could be started (the leader then builds alone). what: "sift", "sift_col" */
extern bool sift_par_launch(SiftParLeader *ldr, const char *what, Relation index, int nworkers);

/* Waits for the workers and adds their WAL and buffer usage to the leader's */
extern void sift_par_wait(SiftParLeader *ldr);

/* Destroys the context and leaves parallel mode */
extern void sift_par_end(SiftParLeader *ldr);

/* A worker: the shared state and parts of the build, its relations and snapshot, opened and closed as the leader locked them */
typedef struct SiftParWorker
{
	shm_toc    *toc;
	void	   *shared;
	void	   *parts;
	Relation	heap;
	Relation	index;
	IndexInfo  *info;
	Snapshot	snapshot;
	LOCKMODE	heaplock;
	LOCKMODE	indexlock;
	bool		restored;
} SiftParWorker;

extern void sift_par_worker_begin(SiftParWorker *w, shm_toc *toc);
extern void sift_par_worker_end(SiftParWorker *w);

#endif							/* SIFT_PARBUILD_H */
