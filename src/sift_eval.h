/*
 * sift_eval.h - query engine: bind a query to an index snapshot, then
 * iterate matches in heap order, rank top-k, or count (DESIGN.md (a), (e)).
 *
 * Independent of the planner: used by amgetbitmap, SiftScan, SiftCount,
 * selectivity estimation and sift.verify. Statistics (N, avgdl, df, idf)
 * are frozen at bind time, so rescans of the same plan are consistent.
 * Every object lives in the context passed to sift_state_open().
 *
 * Index semantics are relaxed (sift_query.h): a positive phrase is the AND
 * of its terms, a phrase under NOT excludes nothing, and the page match
 * says whether the candidates need the recheck. Landed in M2b: state, bind,
 * the heap-order iterator and the estimate; the ranker and the count are M3.
 */
#ifndef SIFT_EVAL_H
#define SIFT_EVAL_H

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

/* Parse + analyze q with the index analyzer; allocates in CurrentMemoryContext */
extern SiftQuery *sift_state_query(SiftIndexState *st, const char *q, int len);

#define SIFT_BIND_SCORE		0x01	/* load scoring data (tf, dl, bounds); M3 */
#define SIFT_BIND_CAPPED	0x02	/* stop expanding at the limit instead of raising (estimates) */

typedef struct SiftPlan SiftPlan;

/*
 * ANDs all queries (several @@> quals on one index) and resolves terms per
 * segment. The queries must outlive the plan. Wildcard, prefix, regex and
 * fuzzy leaves expand to the union of the segments' dictionary terms;
 * ERRCODE_PROGRAM_LIMIT_EXCEEDED above sift.max_expansions.
 */
extern SiftPlan *sift_plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags);

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

/*
 * Unordered matches in heap order, one page per call: offmask holds matched
 * live offsets (bit off-1); recheck when the query has a phrase; scores
 * stay NULL (M3). Pages of different segments sharing a block are merged
 * (disjoint by invariant I1). Holds no buffer between calls.
 */
typedef struct SiftPageMatch
{
	BlockNumber blk;
	uint64		offmask[SIFT_OFFMASK_WORDS];
	bool		recheck;
	float4	   *scores;
} SiftPageMatch;

typedef struct SiftMatchIter SiftMatchIter;

extern SiftMatchIter *sift_iter_begin(SiftPlan *plan);
extern bool sift_iter_next(SiftMatchIter *it, SiftPageMatch *out);
extern void sift_iter_end(SiftMatchIter *it);

/*
 * Ranked top-k with verified threshold. verify is called for every candidate
 * that would enter the top-k, without any buffer lock held; it checks
 * visibility, residual quals and (if recheck) the phrase. k <= 0 means
 * unbounded. sift_rank_next returns results in descending score, ties by
 * ascending ctid, and may continue past k. (M3)
 */
typedef bool (*SiftVerifyFn) (void *arg, ItemPointer tid, bool recheck);

typedef struct SiftRanker SiftRanker;

extern SiftRanker *sift_rank_begin(SiftPlan *plan, int64 k, SiftVerifyFn verify, void *arg);
extern bool sift_rank_next(SiftRanker *r, ItemPointer tid, float4 *score);
extern float4 sift_rank_max_score(SiftRanker *r);
extern void sift_rank_end(SiftRanker *r);

/*
 * count(*) of visible matches: VM all-visible pages are counted from bitmaps
 * (chunk counts on clean SBs), others through sift_vis_filter_page; phrase
 * candidates go through verify. (M3)
 */
extern int64 sift_eval_count(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg);

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
