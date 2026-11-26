/*
 * agg/win/func.h - the spec of a window function of a SiftAggV6Window plan (v6_window.h).
 */
#ifndef SIFT_AGG_V6_WIN_FUNC_H
#define SIFT_AGG_V6_WIN_FUNC_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/win/window.h"

/* The spec of function f of the window, its argument added to the input; false when the engine does not run the function */
extern bool sift_v6_win_func_spec(SiftV6Input *in, const WindowFunc *wf, bool range, SiftV6WindowSpec *s, int f);

/* Whether the results of the window are those of any order of the rows of a partition: with no ORDER BY every row is a peer, so only RANGE frames, rank and dense_rank qualify
 * (a window of the whole set, OVER (), is the top-N scan's: agg/topn) */
extern bool sift_v6_win_order_free(const SiftWindowSpec *w);

#endif							/* SIFT_AGG_V6_WIN_FUNC_H */
