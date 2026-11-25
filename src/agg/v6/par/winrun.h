/*
 * agg/v6/par/winrun.h - the window node of SiftAggV6 (v6_window.h) on the compute threads of the backend (core/par/pool.h), for the windows that sort their partitions (the parts are sorted by the
 * PARTITION BY keys only, or by none: v6_win_partsort.h, v6_win_par.h).
 *
 * The leader reads the source (whose rows come with the rows of a partition together) and cuts it in items of whole partitions; a thread sorts the partitions of an item and runs the functions over them
 * (par/winpart.h); the leader gives the rows of each item to the node, in the order of the items, while the threads work on the next ones. The partitions in flight are bounded by work_mem (the
 * leader makes no item while the ones made hold the share of it). A partition that does not fit what agg/v6/win/partsort.h holds is sorted by a tuplesort that spills to files and run by the engine
 * of the serial node over the sorted stream (par/winspill.h), when its item is released.
 *
 * What the threads do not run stays with the serial node, which gives the same rows: a function or a column type of agg/window.c that par/winpart.h has not (numeric and text columns, the average of an
 * int8, a top-K of each partition), the windows that need no sort (the parts are in the order of the window), and a backend that has no threads.
 */
#ifndef SIFT_AGG_V6_PAR_WINRUN_H
#define SIFT_AGG_V6_PAR_WINRUN_H

#include "agg/v6/win/window.h"
#include "agg/v6/win/carry.h"

/* The row of a result: its tag and the value of each function (NULL where the bit f of res_nulls is set) */
typedef void (*SiftV6ParWinEmit) (void *arg, uint64 tag, const SiftWinVal *res, uint16 res_nulls);

/* Whether the threads run the window of spec over the input in (before sift_v6_window_partsort changes the slots of the keys) */
extern bool sift_v6_par_win_wanted(const SiftV6WindowSpec *spec, const SiftV6Input *in);

/* Runs it over src (the rows that pass the WHERE clauses, the rows of a partition together); returns the threads it used. carry: the columns of the rows, filled as the source is read (NULL: none) */
extern int	sift_v6_par_win_run(MemoryContext cx, const SiftV6WindowSpec *spec, const SiftV6Input *in, SiftV6Source *src, SiftWinCarry *carry, SiftV6ParWinEmit emit, void *emit_arg,
								SiftWinStats *stats);

#endif							/* SIFT_AGG_V6_PAR_WINRUN_H */
