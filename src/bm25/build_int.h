/*
 * build_int.h - what build.c and build_runs.c share: the build state, the phases, the test hooks' macros and the functions that cross.
 */
#ifndef SIFT_BM25_BUILD_INT_H
#define SIFT_BM25_BUILD_INT_H

#include "postgres.h"

#include "access/heapam.h"
#include "access/parallel.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "tcop/tcopprot.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "bm25/am.h"
#include "bm25/build.h"
#include "core/parbuild.h"
#include "storage/condition_variable.h"
#include "table/table.h"
#include "core/visibility.h"

#define BATCH			1024
#define ULL(x)			((unsigned long long) (x))

typedef struct PageDoc
{
	ItemPointerData tid;
	uint32		off;			/* into the page's text arena */
	uint32		len;
} PageDoc;

typedef struct BuildState
{
	Relation	index;
	MemoryContext cxt;
	MemoryContext tmpcxt;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	Size		budget;
	Size		page_text;		/* bytes of text of a heap page held before they are analyzed (see page_text_max) */
	Size		small_doc;		/* docs of fewer bytes of text always fit the buffer that is not full */
	SiftWriteTarget *wt;
	SiftScoreRef ref;
	/* docs of the current heap page, sorted by offset before analysis */
	BlockNumber page;
	PageDoc    *docs;
	int			ndocs;
	int			maxdocs;
	StringInfoData text;
	/* forced segment boundaries (build_segments) */
	BlockNumber *bounds;
	int			nbounds;
	int			nextbound;
	/* runs of the current segment */
	SiftRun   **runs;
	int			nruns;
	int			maxruns;
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
	double		indexed;
	uint32		analyzer_hash;
	bool		positions;
	/* a parallel build's runs go to files of the fileset, which its participants read: the shared state, NULL when it is serial */
	struct SiftBuildShared *shared;
	RunHome		home;
	/* progress (pg_stat_progress_create_index): the phase, docs reported, scan done, docs of the segments written */
	int			phase;
	double		reported;
	bool		scanned;
	uint64		written;
} BuildState;

/*
 * Every participant scans its share of a parallel heap scan (ascending keys,
 * interleaved with the others') into sorted runs. The runs are files of a shared
 * fileset; the leader merges them into the segments a serial build would write, and
 * the participants write the terms of each (build_ranges.c).
 */
#define SCAN_OF(shared)	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(SiftBuildShared))))

/* The segment of a group of runs, written in ranges of its terms: the plan the leader publishes and the ranges the participants take */
typedef struct SiftBuildRanges
{
	ConditionVariable cv;
	int			nscanned;			/* workers that handed their runs over */
	int			gen;				/* the group of the plan, from 1 */
	bool		closed;				/* no more groups */
	int			nruns;
	struct
	{
		int			worker;
		int			id;
		uint64		trailer;
	}			run[MAX_MERGE_FANIN];
	SiftScoreRef ref;				/* with the average length of the group's docs */
	int			nranges;
	SiftTermCut cut[MAX_RANGES - 1];	/* range i has the terms from cut i - 1 up to cut i */
	int			next;				/* the next range to take */
	struct
	{
		bool		fin;			/* written */
		uint64		post_len;		/* of its blobs */
		uint64		nterms;
	}			range[MAX_RANGES];
} SiftBuildRanges;

typedef struct SiftBuildShared
{
	SiftParCommon common;
	int			nparticipants;
	Size		budget;				/* per participant */
	SharedFileSet fileset;
	slock_t		mutex;				/* also guards the counters of ranges (the plan itself is read after gen) */
	double		reltuples;
	double		indexed;
	int			peak_kb;			/* the most memory a participant allocated (SIFT_TESTING) */
	SiftBuildRanges ranges;
	/* the parallel table scan descriptor follows */
} SiftBuildShared;

/* The most runs a worker hands over (the leader opens each as it merges it) */
#define MAX_PART_RUNS	1024

/* A worker's runs, in the files sift_run_file(worker number, id) */
typedef struct SiftBuildPart
{
	int			nruns;
	struct
	{
		int			id;
		uint64		ndocs;
		uint64		sum_dl;
		uint64		trailer;
		uint64		maxdf;
		uint64		maxblob;
		uint64		dict;
	}			run[MAX_PART_RUNS];
} SiftBuildPart;

/* Subphases of the build, named by sift_ambuildphasename */
#define PHASE_SCAN		2
#define PHASE_SPILL		3
#define PHASE_MERGE		4
#define PHASE_WRITE		5
#define PROGRESS_DOCS	1024		/* docs analyzed between two reports of the scan */

#ifdef SIFT_TESTING
extern void mem_sample(void);
extern void mem_watch(MemoryContext cxt);
extern void sift_test_phase_wait(int phase);
#define MEM_SAMPLE()	mem_sample()
#define TEST_PHASE_GATE(phase)	sift_test_phase_wait(phase)
#else
#define MEM_SAMPLE()	((void) 0)
#define TEST_PHASE_GATE(phase)	((void) 0)
#endif

/* build_ranges.c */
extern void sift_ranges_wait_scanned(SiftBuildShared *shared, int nworkers);
extern void sift_ranges_worker(SiftBuildShared *shared);
extern bool sift_ranges_write(BuildState *bs, SiftRun **runs, int nruns, uint64 gen, SiftSegEntry *entry);
extern void sift_ranges_close(SiftBuildShared *shared);

extern void progress_phase(BuildState *bs, int phase);
extern void progress_docs(BuildState *bs, bool all);
extern void spill(BuildState *bs);
extern void end_segment(BuildState *bs);
extern void check_order(BuildState *bs, ItemPointer tid);
extern void flush_page(BuildState *bs);
extern Size page_text_max(const BuildState *bs);

/* Analyzes a doc into the buffer, which is spilled first when a large doc does not fit it */
static inline void
analyze_doc(BuildState *bs, ItemPointer tid, const char *text, int len)
{
	SiftTermFreq *terms;
	uint32		dl;
	int			n = sift_analyze_doc(bs->an, text, len, &terms, &dl);

	if (len >= bs->small_doc && !sift_inv_empty(bs->inv) && !sift_inv_fits(bs->inv, n, len))
		spill(bs);
	bs->indexed++;
	if (sift_inv_add(bs->inv, tid, terms, n, dl))
		spill(bs);
}

#endif
