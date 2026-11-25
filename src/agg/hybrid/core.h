/*
 * agg/hybrid/core.h - SiftHybridAgg without the executor: the rows of a ranked text match and a structured scan, folded into groups with no heap fetch where the store decides
 * visibility, and no score computed where the query never reads one. It drives the two directions of agg/hybrid/hybrid.h and hands every batch to a fold: the executor's fold is the
 * vectors and groups of agg/, the test's a handful of accumulators. Portable (core/host.h).
 *
 * B-Agg: the text scan streams its hits (unordered, unscored when no sift.score is read) in chunks that fit the byte budget; the store scan, bound to each chunk's bridge, folds the
 * rows of the bridge that its quals keep.
 * A-Agg: three passes, for a query that no structured qual makes selective in the other direction and that reads no score. The store scan fills the candidate bridge; the text scan,
 * restricted to the candidates, yields the hits among them; the store scan, bound to the bridge of those hits and still holding every structured qual, folds their rows. A candidate
 * bridge that went lossy under its byte budget yields hits that no qual of the store has seen: the third pass keeps the quals, so they are decided there, and none is counted wrongly.
 */
#ifndef SIFT_HYBRID_AGG_CORE_H
#define SIFT_HYBRID_AGG_CORE_H

#include "agg/hybrid/hybrid.h"

/* The direction of a folded scan: B when a score is read (A cannot bring it), else the cheaper of the two that apply under the cost of an unscored match; NONE when neither beats the standard plan */
extern SiftHybridDir sift_hybrid_agg_choose(SiftHybridEst *est, SiftHybridCosts *costs);

/* B-Agg: every chunk of hits folded; returns the chunks */
extern uint64 sift_hybrid_agg_b(SiftHybridRun *run, SiftHybridHitFn hits, void *hits_arg, bool reads_score, SiftHybridSink fold, void *fold_arg);

/*
 * The text scan's hits among the candidates of the bridge cand: the iterator is made when the candidates are known, and *hits_arg is its state.
 */
typedef SiftHybridHitFn (*SiftHybridCandHits) (void *arg, const SiftTidBridge *cand, void **hits_arg);

/* A-Agg's last two passes, for a candidate bridge the caller built (sift_hybrid_a_bridge on its A run): the hits among the candidates, then the store scan bound to their bridges, folded */
extern void sift_hybrid_agg_a_from(SiftHybridRun *b_run, const SiftTidBridge *cand, SiftHybridCandHits cand_hits, void *arg, SiftHybridSink fold, void *fold_arg);

/* A-Agg with its two runs (a_run direction A, b_run direction B, on the same store); false when the store keeps no row (the text scan never ran) */
extern bool sift_hybrid_agg_a(SiftHybridRun *a_run, SiftHybridRun *b_run, SiftHybridCandHits cand_hits, void *arg, SiftHybridSink fold, void *fold_arg);

#endif							/* SIFT_HYBRID_AGG_CORE_H */
