/*
 * col/f2/f2_build_par.c - the parallel CREATE INDEX and CREATE INDEX CONCURRENTLY of format major 2 (col/f2/f2_build.h). The leader writes the metapage, then every participant, the leader
 * included, takes blocks of the heap from one parallel scan, shreds them into rows of its own, sorts and encodes each run that fills its share of maintenance_work_mem and writes the
 * part straight into the index (its blocks come from the allocator, the generation from the manifest) before the manifest takes it in: no row, run or part goes through a
 * temporary file.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/tableam.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/f2_build.h"
#include "core/parbuild.h"

typedef struct F2BuildShared
{
	SiftParCommon common;
	int			nparticipants;
	slock_t		mutex;
	double		reltuples;
	uint64		indexed;
} F2BuildShared;

#define SCAN_OF(sh) ((ParallelTableScanDesc) ((char *) (sh) + BUFFERALIGN(sizeof(F2BuildShared))))

static void
add_counts(F2BuildShared *sh, double reltuples, uint64 indexed)
{
	SpinLockAcquire(&sh->mutex);
	sh->reltuples += reltuples;
	sh->indexed += indexed;
	SpinLockRelease(&sh->mutex);
}

/* A synchronized scan starts in the middle of the heap: the participants take blocks in the order of the heap whatever else is reading it */
static void
init_scan(Relation heap, F2BuildShared *sh, Snapshot snapshot)
{
	int			nest = NewGUCNestLevel();

	set_config_option("synchronize_seqscans", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	table_parallelscan_initialize(heap, SCAN_OF(sh), snapshot);
	AtEOXact_GUC(false, nest);
}

bool
sift_f2_build_par(Relation heap, Relation index, IndexInfo *info, SiftF2Build *leader, IndexBuildResult *res)
{
	int			nworkers = info->ii_ParallelWorkers;
	SiftParLeader *ldr;
	F2BuildShared *sh;
	double		reltuples;

	if (nworkers < 1)
		return false;
	ldr = sift_par_enter(info->ii_Concurrent);
	if (!sift_par_setup(ldr, "sift_f2_build_worker", nworkers, add_size(BUFFERALIGN(sizeof(F2BuildShared)), table_parallelscan_estimate(heap, ldr->snapshot)),
						sizeof(uint64), false))
		return false;
	sh = ldr->shared;
	*sh = (F2BuildShared) {.common = {RelationGetRelid(heap), RelationGetRelid(index), info->ii_Concurrent},.nparticipants = nworkers + 1};
	SpinLockInit(&sh->mutex);
	init_scan(heap, sh, ldr->snapshot);
	if (!sift_par_launch(ldr, "sift_col", index, nworkers))
		return false;
	reltuples = sift_f2_build_scan(leader, info, sift_table_beginscan_parallel(heap, SCAN_OF(sh)), true);
	sift_par_wait(ldr);
	res->heap_tuples = reltuples + sh->reltuples;
	res->index_tuples = sh->indexed;
	sift_par_end(ldr);
	return true;
}

PGDLLEXPORT void sift_f2_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_f2_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	F2BuildShared *sh;
	SiftF2Build *bs;
	double		reltuples;

	sift_par_worker_begin(&w, toc);
	sh = w.shared;
	sift_col_building = RelationGetRelid(w.index);
	bs = sift_f2_build_begin(w.heap, w.index, sh->nparticipants);
	reltuples = sift_f2_build_scan(bs, w.info, sift_table_beginscan_parallel(w.heap, SCAN_OF(sh)), false);
	add_counts(sh, reltuples, sift_f2_build_rows(bs));
	sift_f2_build_end(bs);
	sift_par_worker_end(&w);
}
