/*
 * col/f2/prefetch/prefetch.h - the pages of a part that a scan is about to read, found from the granules the key marks leave, so that the reads of a cold part are asked of the storage together
 * (one ReadStream over many blocks, which io_uring or the I/O workers take as a queue) instead of one 8 KB read after another as the scan reaches each granule.
 *
 * The plan is the granule records of the surviving spans as runs of the pages of the part's byte stream: the records of neighbouring granules lie next to each other, so a span is a few long
 * runs; a hole of a few pages between two runs is read through rather than skipped, as a seek costs more than the pages. The plan holds at most a budget of pages (what a scan may ask of the
 * storage ahead of itself), and is handed out in batches that a stream reads and releases.
 */
#ifndef SIFT_F2_PREFETCH_H
#define SIFT_F2_PREFETCH_H

#include "core/host.h"

#define SIFT_PF_MIN_PAGES		16		/* a scan that reads fewer pages than this is not prefetched */
#define SIFT_PF_MAX_GAP_PAGES	2		/* the pages between two runs that are read through */
#define SIFT_PF_BUDGET_PAGES	8192	/* 64 MB of the stream */
#define SIFT_PF_BATCH_PAGES		256		/* the blocks one stream asks for at a time: 2 MB */

typedef struct SiftPfRun
{
	uint64		first;			/* of the part's byte stream, as a page of SIFT_PAGE_PAYLOAD bytes */
	uint32		npages;
} SiftPfRun;

typedef struct SiftPfPlan
{
	SiftPfRun  *runs;
	uint32		nruns;
	uint64		total_pages;
	uint32		cursor_run;		/* batches continue from here */
	uint32		cursor_page;
} SiftPfPlan;

#define SIFT_PF_WINDOW_GRANULES	32		/* the granules a scan asks of the storage ahead of itself when it has no key range to read whole spans of: a window, so that a selective scan reads little more than it uses */
#define SIFT_PF_WINDOW_SPANS	(SIFT_PF_WINDOW_GRANULES / 2 + 1)

/*
 * The granules of [g0, g0 + n) that a scan did not rule out (skipped[i] is granule g0 + i) as ascending disjoint spans [spans[i][0], spans[i][1]): how many. n is at most SIFT_PF_WINDOW_GRANULES,
 * so no more than SIFT_PF_WINDOW_SPANS spans.
 */
extern int	sift_pf_survivor_spans(const bool *skipped, uint32 g0, uint32 n, uint32 spans[][2]);

/*
 * Whether the window of granules ahead is read ahead: the first window of a part always is (it is bounded by SIFT_PF_WINDOW_GRANULES), a later one only while the filter keeps a row in at least half
 * of the granules it opens (sift_afilt_dense), as a scan that most granules leave nothing of reads their predicate chunks alone and would read the payload it skips.
 */
extern bool sift_pf_window_wanted(uint32 window_index, bool dense);

/* Where granule g's record lies in the section that holds the granules: false for a granule that has none */
typedef bool (*SiftPfRange) (void *ctx, uint32 g, uint64 *off, uint32 *len);

/*
 * The plan for the spans [spans[i][0], spans[i][1]) of granules, whose records are bytes of the stream from section_off on (the offsets range gives are from it). NULL when the spans take fewer
 * than SIFT_PF_MIN_PAGES pages: a stream for them would cost more than it saves.
 */
extern SiftPfPlan *sift_pf_plan(SiftPfRange range, void *ctx, uint64 section_off, uint32 page_payload, const uint32 spans[][2], int nspans);

/* The next pages of the plan, at most max of them, in ascending order: how many (0 at the end) */
extern uint32 sift_pf_batch(SiftPfPlan *plan, uint32 max, uint64 *pages);

extern void sift_pf_free(SiftPfPlan *plan);

#endif							/* SIFT_F2_PREFETCH_H */
