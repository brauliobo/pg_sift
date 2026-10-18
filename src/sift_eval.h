/*
 * sift_eval.h - query engine: bind a query to an index snapshot, then
 * iterate matches in heap order, rank top-k, or count.
 *
 * Independent of the planner: used by amgetbitmap, SiftScan, SiftCount,
 * selectivity estimation and sift.verify. Statistics (N, avgdl, df, idf)
 * are frozen at bind time, so rescans of the same plan are consistent.
 * Every object lives in the context passed to sift_state_open().
 *
 * Index semantics are relaxed (sift_query.h): a positive phrase is the AND
 * of its terms (narrowed by the stored positions where the segment has them),
 * a phrase under NOT excludes nothing, and the page match says whether the
 * candidates need the recheck.
 */
#ifndef SIFT_EVAL_H
#define SIFT_EVAL_H

#include "sift_filter.h"
#include "sift_parallel.h"
#include "sift_query.h"
#include "sift_store.h"
#include "sift_visibility.h"

/* Opened index: directory snapshot, analyzer, options, lazily opened segments */
typedef struct SiftIndexState
{
	Relation	index;
	SiftDirSnapshot dir;
	SiftAnalyzer *analyzer;
	const SiftOptions *opts;
	MemoryContext mcxt;
	void	   *priv;			/* eval-private: opened segments */
} SiftIndexState;

/*
 * Reads the directory (the caller's snapshot keeps the segments alive, store
 * P4). ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE when the analyzer options
 * changed since the build.
 */
extern SiftIndexState *sift_state_open(Relation index, MemoryContext mcxt);
extern void sift_state_close(SiftIndexState *st);	/* releases buffers only */

/* The same on a directory snapshot read elsewhere: the participants of a parallel scan share one */
extern SiftIndexState *sift_state_open_dir(Relation index, MemoryContext mcxt, const SiftDirSnapshot *dir);

/* Parse + analyze q with the index analyzer; allocates in CurrentMemoryContext */
extern SiftQuery *sift_state_query(SiftIndexState *st, const char *q, int len);

#define SIFT_BIND_SCORE		0x01	/* weigh the leaves for the ranker (idf from all segments) */
#define SIFT_BIND_CAPPED	0x02	/* stop expanding at the limit instead of raising (estimates) */

typedef struct SiftPlan SiftPlan;

/*
 * ANDs all queries (several @@> quals on one index) and resolves terms per
 * segment. The queries must outlive the plan. Wildcard, prefix, regex and
 * fuzzy leaves expand to the union of the segments' dictionary terms; above
 * sift.max_expansions a leaf is wide (one term, see sift_prefix.h).
 */
extern SiftPlan *sift_plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags);

/*
 * Restricts the ranked search to the tuples the filter keeps, a superset of those that pass the
 * caller's own quals: superblocks, blocks and documents it drops are never bounded, scored or
 * verified. Call before sift_rank_begin; the filter must outlive the ranker. Iteration and counts
 * ignore it.
 */
extern void sift_plan_set_filter(SiftPlan *plan, const SiftFilter *filter);

/* The documents the bound query is expected to match (the estimate of sift_eval_estimate, from the bound dictionaries) */
extern double sift_plan_docs(const SiftPlan *plan);

/*
 * Parallel scoring. Scores depend on the directory and on the mutable
 * segments, which move under concurrent writers, so one participant binds,
 * exports what the others cannot recompute (the directory snapshot, the
 * mutable segments' statistics and matches) and everyone, the exporter
 * included, binds from that. The export is palloc'd in the current context.
 */
extern void *sift_plan_export(const SiftPlan *plan, Size *len);
extern const SiftDirSnapshot *sift_shared_dir(const void *shared);
extern SiftPlan *sift_plan_bind_shared(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags,
									   const void *shared);

/* Statistics after bind and iteration, for EXPLAIN */
typedef struct SiftPlanStats
{
	double		ndocs;
	double		avgdl;
	int			nsegments;
	int			nmutable;
	int64		sb_scanned;
	int64		blocks_scored;
	int64		blocks_skipped;
	int64		candidates;
	int64		verified;
	float4		theta;
} SiftPlanStats;

extern const SiftPlanStats *sift_plan_stats(const SiftPlan *plan);

/* Phrase recheck of a heap-order candidate's text; needed when the plan has a phrase */
extern bool sift_plan_recheck(SiftPlan *plan, text *doc);
extern bool sift_plan_recheck_needed(const SiftPlan *plan);

/*
 * Unordered matches in heap order, one page per call: offmask holds matched
 * live offsets (bit off-1); recheck when some of them need the text (a phrase
 * the stored positions do not decide: segments without them, mutable matches,
 * a negated phrase), all but those in sure, which the index decides alone. Pages of different segments sharing a block are
 * merged (disjoint by invariant I1). Holds no buffer between calls.
 */
typedef struct SiftPageMatch
{
	BlockNumber blk;
	uint64		offmask[SIFT_OFFMASK_WORDS];
	uint64		sure[SIFT_OFFMASK_WORDS];	/* subset of offmask, only with recheck */
	bool		recheck;
} SiftPageMatch;

static inline bool
sift_page_sure(const SiftPageMatch *pm, int off)
{
	return pm->sure[(off - 1) / 64] >> ((off - 1) % 64) & 1;
}

typedef struct SiftMatchIter SiftMatchIter;

/*
 * A parallel scan visits the superblock ranges [lo, hi) that next hands out,
 * ascending, instead of all of them (next returns false when none is left).
 */
typedef bool (*SiftNextRangeFn) (void *arg, uint32 *lo, uint32 *hi);

extern SiftMatchIter *sift_iter_begin(SiftPlan *plan);
extern void sift_iter_ranges(SiftMatchIter *it, SiftNextRangeFn next, void *arg);
extern bool sift_iter_next(SiftMatchIter *it, SiftPageMatch *out);
extern void sift_iter_end(SiftMatchIter *it);

/*
 * Ranked top-k with verified threshold (plan bound with SIFT_BIND_SCORE).
 * verify is called for every candidate that would enter the top-k, without
 * any buffer lock held: it returns whether the tuple is visible and passes
 * the caller's residual quals, and with want_doc (the plan has a phrase) also
 * the indexed text in *doc, on which the ranker runs the phrase recheck and
 * the exact scoring. *tid goes in as the index entry and comes out as the
 * visible tuple's ctid (they differ along a HOT chain): ties break on that
 * ctid, the one the user sees. k <= 0 means unbounded. sift_rank_next returns
 * the index entries in descending score, ties by ascending ctid, and continues
 * past k by searching again with a larger k; scores stay those of the bind.
 */
typedef bool (*SiftVerifyFn) (void *arg, ItemPointer tid, bool want_doc, text **doc);

typedef struct SiftRanker SiftRanker;

extern SiftRanker *sift_rank_begin(SiftPlan *plan, int64 k, SiftVerifyFn verify, void *arg);
extern bool sift_rank_next(SiftRanker *r, ItemPointer tid, float4 *score);
extern float4 sift_rank_max_score(SiftRanker *r);	/* best verified score, runs the search */
extern void sift_rank_end(SiftRanker *r);

/*
 * Makes the ranker one of the participants of a parallel search (call before
 * the first sift_rank_next; k must be known). Its first search scores the
 * units it claims with the shared threshold; the participant that finishes
 * last returns the merged result, identical to the serial one, and the others
 * return nothing. Asking for more than k rows makes the emitter search serially
 * again, as a serial ranker does.
 */
extern void sift_rank_parallel(SiftRanker *r, SiftPar *par);

/*
 * count(*) of visible matches: VM all-visible pages are counted from bitmaps,
 * others through sift_vis_filter_page; with a phrase every candidate goes
 * through verify (visibility and text) and the recheck.
 */
extern int64 sift_eval_count(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg);

/* The same over the superblock ranges next hands out (next may be NULL: all) */
extern int64 sift_eval_count_ranges(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg,
									SiftNextRangeFn next, void *next_arg);

/*
 * The visible matches page by page in heap order, what sift_eval_count counts: fn gets the offsets
 * (bit off-1) that survive visibility and the phrase recheck of every page with any. Returns the
 * total.
 */
typedef void (*SiftPageFn) (void *arg, BlockNumber blk, const uint64 *offmask);

extern int64 sift_eval_pages(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg,
							 SiftPageFn fn, void *fnarg);

/*
 * Planner estimate from term statistics only: matching documents under
 * independence (AND product, OR union, NOT complement), the postings the
 * query reads and its dictionary terms. Never raises the expansion limit
 * (it stops expanding there); parse errors still raise. Opens and closes
 * its own state.
 */
typedef struct SiftEstimate
{
	double		docs;
	double		postings;
	double		terms;
	double		ndocs;			/* documents in the index */
} SiftEstimate;

extern void sift_eval_estimate(Relation index, text **queries, int nqueries, SiftEstimate *est);

#endif							/* SIFT_EVAL_H */
