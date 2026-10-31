/*
 * bm25/build_ranges.c - the segment of a parallel build, written by all its participants. The leader's merge of the runs is half
 * of a build with workers (the docs, then every term's postings through the posting writer), and it is one process. The terms of
 * the runs are cut into ranges (sift_runs_cuts), which the workers, once they handed their runs over, and the leader, once it has
 * merged the docs, take one at a time: each writes the blobs of its terms to a file and one record per term to another. The leader
 * appends the files in term order (sift_segw_adopt_range) and the segment is the one a single process writes, byte for byte.
 */
#include "build_int.h"

#include "utils/wait_event.h"

#ifdef SIFT_TESTING
int			sift_test_range_pieces = 0;
#endif

/* Ranges are cut from runs of this many bytes up, as many as four to a participant, so that one that is slower does not hold the others */
#define RANGE_BYTES		(512 * 1024)
#define RANGES_PER_PARTICIPANT	4

static const char *
range_file(int gen, int range, char kind)
{
	static char name[48];

	snprintf(name, sizeof(name), "sift-range-%d-%d-%c", gen, range, kind);
	return name;
}

static void
wait_for(SiftBuildShared *shared, bool (*done) (SiftBuildShared *, int), int arg)
{
	ConditionVariablePrepareToSleep(&shared->ranges.cv);
	while (!done(shared, arg))
		ConditionVariableSleep(&shared->ranges.cv, WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	ConditionVariableCancelSleep();
}

static bool
all_scanned(SiftBuildShared *shared, int nworkers)
{
	bool		ok;

	SpinLockAcquire(&shared->mutex);
	ok = shared->ranges.nscanned >= nworkers;
	SpinLockRelease(&shared->mutex);
	return ok;
}

/* The leader: waits until the workers handed their runs over */
void
sift_ranges_wait_scanned(SiftBuildShared *shared, int nworkers)
{
	wait_for(shared, all_scanned, nworkers);
}

/* Writes range i of the plan of the group: its blobs and entries to files of the fileset, which the leader adopts */
static void
write_range(SiftBuildShared *shared, int gen, int i, SiftRun **runs, int nruns)
{
	SiftBuildRanges *rg = &shared->ranges;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift range", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	BufFile    *post = BufFileCreateFileSet(&shared->fileset.fs, range_file(gen, i, 'p'));
	BufFile    *ent = BufFileCreateFileSet(&shared->fileset.fs, range_file(gen, i, 'e'));
	SiftStreamWriter *sw = sift_sw_begin_spool(post);
	SiftSegmentWriter *rw = sift_segw_begin_range(sw, ent, &rg->ref, cxt);

	sift_segw_spool(rw, sift_seg_postings(shared->budget) / 2);
	sift_runs_write_range(runs, nruns, shared->budget, i > 0 ? &rg->cut[i - 1] : NULL, i < rg->nranges - 1 ? &rg->cut[i] : NULL, rw);
	BufFileExportFileSet(post);
	BufFileExportFileSet(ent);
	SpinLockAcquire(&shared->mutex);
	rg->range[i].post_len = sift_sw_tell(sw);
	rg->range[i].nterms = sift_segw_range_terms(rw);
	rg->range[i].fin = true;
	SpinLockRelease(&shared->mutex);
	BufFileClose(post);
	BufFileClose(ent);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	ConditionVariableBroadcast(&rg->cv);
}

/* The runs of the plan, as this process opens them */
static SiftRun **
plan_runs(SiftBuildShared *shared)
{
	SiftBuildRanges *rg = &shared->ranges;
	SiftRun   **runs = palloc(rg->nruns * sizeof(SiftRun *));

	for (int i = 0; i < rg->nruns; i++)
	{
		runs[i] = palloc0(sizeof(SiftRun));
		runs[i]->fileset = &shared->fileset;
		runs[i]->worker = rg->run[i].worker;
		runs[i]->id = rg->run[i].id;
		runs[i]->trailer = rg->run[i].trailer;
	}
	return runs;
}

/* Range i of the plan of group gen, which this process has taken */
static void
write_one(SiftBuildShared *shared, int gen, int i)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift ranges", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftRun   **runs = plan_runs(shared);

	/* the plan stays as it is until the ranges taken are done */
	write_range(shared, gen, i, runs, shared->ranges.nruns);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* Takes the next range of the plan of group gen; -1 when there is none */
static int
take_range(SiftBuildShared *shared, int gen)
{
	SiftBuildRanges *rg = &shared->ranges;
	int			i = -1;

	SpinLockAcquire(&shared->mutex);
	if (rg->gen == gen && rg->next < rg->nranges)
		i = rg->next++;
	SpinLockRelease(&shared->mutex);
	return i;
}

/* Writes ranges of group gen until none is left */
static void
write_ranges(SiftBuildShared *shared, int gen)
{
	for (int i; (i = take_range(shared, gen)) >= 0;)
		write_one(shared, gen, i);
}

static bool
news(SiftBuildShared *shared, int seen)
{
	bool		ok;

	SpinLockAcquire(&shared->mutex);
	ok = shared->ranges.gen > seen || shared->ranges.closed;
	SpinLockRelease(&shared->mutex);
	return ok;
}

/* A worker, its runs handed over: writes ranges of the groups of runs the leader publishes, until it closes them */
void
sift_ranges_worker(SiftBuildShared *shared)
{
	SiftBuildRanges *rg = &shared->ranges;
	int			seen = 0;

#ifdef SIFT_TESTING
	/* the hooks of the build, which went with its context */
	mem_watch(AllocSetContextCreate(CurrentMemoryContext, "sift ranges worker", ALLOCSET_SMALL_SIZES));
#endif
	SpinLockAcquire(&shared->mutex);
	rg->nscanned++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&rg->cv);
	for (;;)
	{
		int			gen;

		wait_for(shared, news, seen);
		SpinLockAcquire(&shared->mutex);
		gen = rg->gen;
		SpinLockRelease(&shared->mutex);
		if (gen == seen)
			return;
		write_ranges(shared, gen);
		seen = gen;
	}
}

void
sift_ranges_close(SiftBuildShared *shared)
{
	SpinLockAcquire(&shared->mutex);
	shared->ranges.closed = true;
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&shared->ranges.cv);
}

static bool
range_fin(SiftBuildShared *shared, int i)
{
	bool		ok;

	SpinLockAcquire(&shared->mutex);
	ok = shared->ranges.range[i].fin;
	SpinLockRelease(&shared->mutex);
	return ok;
}

/* The leader gives the runs of the group to the participants: a file of a fileset is read by name, once its writer is done with it */
static bool
publish_runs(SiftRun **runs, int nruns)
{
	for (int i = 0; i < nruns; i++)
		if (runs[i]->fileset == NULL)
			return false;
	for (int i = 0; i < nruns; i++)
		if (runs[i]->file)
		{
			BufFileExportFileSet(runs[i]->file);
			BufFileClose(runs[i]->file);
			runs[i]->file = NULL;
		}
	return true;
}

/* Appends the files of range i to the segment, and deletes them */
static void
stitch(SiftBuildShared *shared, SiftSegmentWriter *sw, int gen, int i)
{
	SiftBuildRanges *rg = &shared->ranges;
	BufFile    *post = BufFileOpenFileSet(&shared->fileset.fs, range_file(gen, i, 'p'), O_RDONLY, false);
	BufFile    *ent = BufFileOpenFileSet(&shared->fileset.fs, range_file(gen, i, 'e'), O_RDONLY, false);

	sift_segw_adopt_range(sw, post, rg->range[i].post_len, ent, rg->range[i].nterms, rg->ref.avgdl);
	BufFileClose(post);
	BufFileClose(ent);
	BufFileDeleteFileSet(&shared->fileset.fs, range_file(gen, i, 'p'), false);
	BufFileDeleteFileSet(&shared->fileset.fs, range_file(gen, i, 'e'), false);
}

/*
 * The segment of the runs, written in ranges by all the participants; false, with nothing written, when the runs are too small to cut or
 * lack the samples of their terms (the leader then merges them alone).
 */
bool
sift_ranges_write(BuildState *bs, SiftRun **runs, int nruns, uint64 gen, SiftSegEntry *entry)
{
	SiftBuildShared *shared = bs->shared;
	SiftBuildRanges *rg = &shared->ranges;
	SiftTermCut *cuts = rg->cut;
	SiftSegmentWriter *sw;
	uint64		ndocs = 0,
				sum_dl = 0;
	int			ncuts,
				g;
	MemoryContext cxt;

	if (nruns > MAX_MERGE_FANIN || !publish_runs(runs, nruns) || !sift_runs_splittable(runs, nruns))
		return false;
	/* the plan is read by the workers once it has a gen: nothing of the last group is still in use */
	ncuts = sift_runs_cuts(runs, nruns, shared->budget, TEST_KNOB(sift_test_range_bytes, RANGE_BYTES),
						   Min(MAX_RANGES, RANGES_PER_PARTICIPANT * shared->nparticipants), cuts);
	if (ncuts == 0)
		return false;
	for (int i = 0; i < nruns; i++)
	{
		rg->run[i].worker = runs[i]->worker;
		rg->run[i].id = runs[i]->id;
		rg->run[i].trailer = runs[i]->trailer;
		ndocs += runs[i]->ndocs;
		sum_dl += runs[i]->sum_dl;
	}
	rg->nruns = nruns;
	rg->ref = bs->ref;
	rg->ref.avgdl = (float4) ((double) sum_dl / ndocs);
	rg->nranges = ncuts + 1;
	MemSet(rg->range, 0, sizeof(rg->range));
	SpinLockAcquire(&shared->mutex);
	g = ++rg->gen;
	rg->next = 0;
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&rg->cv);
#ifdef SIFT_TESTING
	sift_test_range_pieces += rg->nranges;
#endif
	cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment write", ALLOCSET_DEFAULT_SIZES);
	sw = sift_runs_write_docs(runs, nruns, bs->budget, bs->wt, gen, &bs->ref, cxt);
	/* the ranges are appended in order as they are done, between those the leader writes */
	for (int i = 0, next = 0; next < rg->nranges;)
	{
		if (range_fin(shared, next))
			stitch(shared, sw, g, next++);
		else if ((i = take_range(shared, g)) >= 0)
			write_one(shared, g, i);
		else
			wait_for(shared, range_fin, next);
	}
	sift_segw_finish(sw, entry);
	sift_runs_close(runs, nruns);
	MemoryContextDelete(cxt);
	return true;
}
