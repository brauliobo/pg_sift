/*
 * agg/v6/win/func.h - the spec of a window function of a SiftAggV6Window plan (v6_window.h).
 */
#ifndef SIFT_AGG_V6_WIN_FUNC_H
#define SIFT_AGG_V6_WIN_FUNC_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/v6/win/window.h"

/* The spec of function f of the window, its argument added to the input; false when the engine does not run the function */
extern bool sift_v6_win_func_spec(SiftV6Input *in, const WindowFunc *wf, bool range, SiftV6WindowSpec *s, int f);

#endif							/* SIFT_AGG_V6_WIN_FUNC_H */
