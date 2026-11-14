/*
 * bm25/merge_ranges.c - the merge of segments, written by several processes. The merge dumps every input segment to a run and merges the
 * runs in one process (step.c); here the segments are read where they are: the leader merges their docs, and the terms, cut into ranges
 * from the samples of the dictionaries, are written by the leader and max_parallel_maintenance_workers workers, each range from the
 * postings of the input segments (build_ranges.c). The merged segment is the one a single process writes, byte for byte.
 */
#include "build_int.h"

#include "access/xact.h"
#include "utils/snapmgr.h"

#include "core/store.h"

PGDLLEXPORT void sift_merge_worker(dsm_segment *seg, shm_toc *toc);

void
sift_merge_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	SiftBuildShared *shared;

	sift_par_worker_begin(&w, toc);
	shared = w.shared;
	SharedFileSetAttach(&shared->fileset, seg);
#ifdef SIFT_TESTING
	mem_watch(AllocSetContextCreate(CurrentMemoryContext, "sift merge worker", ALLOCSET_SMALL_SIZES));
#endif
	sift_ranges_worker(shared, w.index);
	sift_par_worker_end(&w);
}

/* The segments of the merge that have a live doc, opened, with what the merged segment's docs add up to */
static int
open_live(Relation index, const SiftSegEntry *in, int n, MemoryContext mcxt, SiftSegment **segs, uint64 *ndocs, uint64 *sum_dl)
{
	int			nlive = 0;

	for (int i = 0; i < n; i++)
	{
		SiftSegment *seg = sift_seg_open(index, &in[i], mcxt);
		uint64		d,
					dl;

		sift_seg_live_stats(seg, &d, &dl);
		if (d == 0)
		{
			sift_seg_close(seg);
			continue;
		}
		segs[nlive++] = seg;
		*ndocs += d;
		*sum_dl += dl;
	}
	return nlive;
}

/* Releases the buffers of the segments, which are read again when needed */
static void
close_live(SiftSegment **segs, int nlive)
{
	for (int i = 0; i < nlive; i++)
		sift_seg_close(segs[i]);
}

int
sift_merge_ranges(Relation index, SiftWriteTarget *wt, const SiftSegEntry *in, int n, const SiftScoreRef *ref, SiftSegEntry *neu)
{
	SiftSegment *segs[MAX_MERGE_FANIN];
	SiftTermCut *cuts;
	SiftParLeader *ldr;
	SiftBuildShared *shared;
	SiftSegmentWriter *sw;
	MemoryContext cxt;
	uint64		ndocs = 0,
				sum_dl = 0;
	int			nlive,
				ncuts,
				nworkers = Min(max_parallel_maintenance_workers, MAX_RANGES - 1);
	uint64		gen;

	/* the context of the workers needs a transaction id from before its parallel mode, which a parallel VACUUM's leader cannot take: one process merges */
	if (nworkers < 1 || n > MAX_MERGE_FANIN || sift_xid_unassignable())
		return -1;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment merge", ALLOCSET_DEFAULT_SIZES);
#ifdef SIFT_TESTING
	mem_watch(cxt);
#endif
	nlive = open_live(index, in, n, cxt, segs, &ndocs, &sum_dl);
	if (nlive == 0)
	{
		MemoryContextDelete(cxt);
		return 0;
	}
	cuts = palloc((MAX_RANGES - 1) * sizeof(SiftTermCut));
	ncuts = sift_segs_cuts(segs, nlive, TEST_KNOB(sift_test_range_bytes, RANGE_BYTES), Min(MAX_RANGES, RANGES_PER_PARTICIPANT * (nworkers + 1)), cuts);
	if (ncuts == 0)
	{
		close_live(segs, nlive);
		MemoryContextDelete(cxt);
		return -1;
	}
	nworkers = Min(nworkers, ncuts);
	/* the reads so far hold buffer locks (a reader keeps its page locked), and no worker starts while a process holds any */
	close_live(segs, nlive);
	/* the parallel mode takes no new transaction id and no new snapshot, and the workers start with the active one: the maintenance worker has neither */
	GetCurrentTransactionId();
	PushActiveSnapshot(GetTransactionSnapshot());
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_merge_worker", nworkers, sizeof(SiftBuildShared), sizeof(int), false))
	{
		PopActiveSnapshot();
		MemoryContextDelete(cxt);
		return -1;
	}
	shared = ldr->shared;
	MemSet(shared, 0, sizeof(SiftBuildShared));
	shared->common = (SiftParCommon) {InvalidOid, RelationGetRelid(index), false};
	shared->nparticipants = nworkers + 1;
	shared->budget = (Size) maintenance_work_mem * 1024 / (nworkers + 1);
	ConditionVariableInit(&shared->ranges.cv);
	SharedFileSetInit(&shared->fileset, ldr->pcxt->seg);
	SpinLockInit(&shared->mutex);
	shared->ranges.nsegs = nlive;
	for (int i = 0; i < nlive; i++)
		shared->ranges.seg[i] = segs[i]->entry;
	memcpy(shared->ranges.cut, cuts, ncuts * sizeof(SiftTermCut));
	shared->ranges.ref = *ref;
	shared->ranges.ref.avgdl = (float4) ((double) sum_dl / ndocs);
	if (!sift_par_launch(ldr, "sift merge for", index, nworkers))
	{
		PopActiveSnapshot();
		MemoryContextDelete(cxt);
		return -1;
	}
	gen = sift_store_new_gen(index);
	sift_ranges_publish(shared, ncuts);
	sw = sift_segs_write_docs(segs, nlive, shared->budget, wt, gen, ref, cxt);
	/* a process that waits for the others holds no buffer lock: it would not hear of their errors */
	close_live(segs, nlive);
	sift_ranges_stitch(shared, index, sw);
	sift_segw_finish(sw, neu);
	sift_ranges_close(shared);
	sift_par_wait(ldr);
	sift_par_end(ldr);
	PopActiveSnapshot();
	MemoryContextDelete(cxt);
	return 1;
}
