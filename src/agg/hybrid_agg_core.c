/*
 * agg/hybrid_agg_core.c - the two directions of a folded hybrid scan (agg/hybrid_agg_core.h).
 */
#include "core/host.h"

#include "agg/hybrid_agg_core.h"

SiftHybridDir
sift_hybrid_agg_choose(SiftHybridEst *est, SiftHybridCosts *costs)
{
	SiftHybridDir dir;

	est->agg_mode = true;
	dir = sift_hybrid_choose(est, costs);
	if (est->reads_score)
		return sift_hybrid_beats(costs->bm25_to_col, costs) ? SIFT_HYB_BM25_TO_COL : SIFT_HYB_NONE;
	return dir;
}

uint64
sift_hybrid_agg_b(SiftHybridRun *run, SiftHybridHitFn hits, void *hits_arg, bool reads_score, SiftHybridSink fold, void *fold_arg)
{
	uint64		chunks = 0;

	while (sift_hybrid_b_chunk(run, hits, hits_arg, reads_score))
	{
		sift_hybrid_b_emit(run, fold, fold_arg);
		chunks++;
	}
	return chunks;
}

void
sift_hybrid_agg_a_from(SiftHybridRun *b_run, const SiftTidBridge *cand, SiftHybridCandHits cand_hits, void *arg, SiftHybridSink fold, void *fold_arg)
{
	void	   *hits_arg;
	SiftHybridHitFn hits = cand_hits(arg, cand, &hits_arg);

	sift_hybrid_agg_b(b_run, hits, hits_arg, false, fold, fold_arg);
}

bool
sift_hybrid_agg_a(SiftHybridRun *a_run, SiftHybridRun *b_run, SiftHybridCandHits cand_hits, void *arg, SiftHybridSink fold, void *fold_arg)
{
	const SiftTidBridge *cand = sift_hybrid_a_bridge(a_run, false, false);

	if (a_run->stats.empty)
		return false;
	sift_hybrid_agg_a_from(b_run, cand, cand_hits, arg, fold, fold_arg);
	return true;
}
