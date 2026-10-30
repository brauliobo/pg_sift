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
	/* a parallel worker's runs go to files of the leader's fileset */
	RunHome		home;
	/* progress (pg_stat_progress_create_index): the phase, docs reported, scan done, docs of the segments written */
	int			phase;
	double		reported;
	bool		scanned;
	uint64		written;
} BuildState;

/* Subphases of the build, named by sift_ambuildphasename */
#define PHASE_SCAN		2
#define PHASE_SPILL		3
#define PHASE_MERGE		4
#define PHASE_WRITE		5
#define PROGRESS_DOCS	1024		/* docs analyzed between two reports of the scan */

#ifdef SIFT_TESTING
extern void mem_sample(void);
extern void sift_test_phase_wait(int phase);
#define MEM_SAMPLE()	mem_sample()
#define TEST_PHASE_GATE(phase)	sift_test_phase_wait(phase)
#else
#define MEM_SAMPLE()	((void) 0)
#define TEST_PHASE_GATE(phase)	((void) 0)
#endif

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
