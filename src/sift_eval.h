/*
 * sift_eval.h - query engine: bind a query to an index snapshot, then
 * iterate matches in heap order, rank top-k, or count (DESIGN.md (a), (e)).
 *
 * Independent of the planner: used by amgetbitmap, SiftScan, SiftCount,
 * selectivity estimation and sift.verify. Statistics (N, avgdl, df, idf)
 * are frozen at bind time, so rescans of the same plan are consistent.
 * Every object lives in the context passed to sift_state_open().
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
	void	   *priv;			/* eval-private: opened segments, mutable docs */
} SiftIndexState;

extern SiftIndexState *sift_state_open(Relation index, MemoryContext mcxt);
extern void sift_state_close(SiftIndexState *st);	/* releases buffers only */

/* Parse + analyze q with the index analyzer */
extern SiftQuery *sift_state_query(SiftIndexState *st, const char *q, int len);

#define SIFT_BIND_SCORE		0x01	/* load scoring data (tf, dl, bounds) */

typedef struct SiftPlan SiftPlan;

/* ANDs all queries (several @@> quals on one index) and resolves terms per segment */
extern SiftPlan *sift_plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags);

/* Statistics after bind, for EXPLAIN and sift.max_score */
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
 * live offsets; recheck when the query has a phrase; scores (slot off-1)
 * filled only with SIFT_BIND_SCORE. Pages of different segments sharing a
 * block are merged (disjoint by invariant I1).
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

/* Upcoming heap blocks for ReadStream prefetch; InvalidBlockNumber at end */
extern BlockNumber sift_iter_peek_block(SiftMatchIter *it, int ahead);
extern void sift_iter_end(SiftMatchIter *it);

/*
 * Ranked top-k with verified threshold. verify is called for every candidate
 * that would enter the top-k, without any buffer lock held; it checks
 * visibility, residual quals and (if recheck) the phrase. k <= 0 means
 * unbounded. sift_rank_next returns results in descending score, ties by
 * ascending ctid, and may continue past k.
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
 * candidates go through verify.
 */
extern int64 sift_eval_count(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg);

/* Planner estimate of matching docs from term statistics only */
extern double sift_eval_estimate(Relation index, SiftQuery **queries, int nqueries);

#endif							/* SIFT_EVAL_H */
