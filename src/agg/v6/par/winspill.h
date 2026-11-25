/*
 * agg/v6/par/winspill.h - a partition of the window node on the compute threads (par/winrun.h) that is more than a thread may hold: its rows go to a sort that spills to files past work_mem
 * (agg/v6/win/spill.h), and the functions run over the sorted stream in the engine of the serial node (agg/window.h), which carries the state of the frame from one batch to the next. The rows
 * are the ones the threads would have run (the same order keys, ties in arrival order, the same frames), so the results are the same.
 */
#ifndef SIFT_AGG_V6_PAR_WINSPILL_H
#define SIFT_AGG_V6_PAR_WINSPILL_H

#include "postgres.h"

#include "agg/v6/par/winpart.h"

typedef struct SiftParWinSpill SiftParWinSpill;

/* The sort of one partition of at most max_bytes of memory, in a context of cx; arg_slot[f] >= 0: function f has an argument */
extern SiftParWinSpill *sift_par_win_spill_open(MemoryContext cx, const SiftParWinSpec *ws, const int *arg_slot, uint64 max_bytes);

/* A row: its tag, the value and NULL flag of each order key, and of the argument of each function that has one (the layout of a SiftParWinItem) */
extern void sift_par_win_spill_put(SiftParWinSpill *s, uint64 tag, const int64 *ord, const uint8 *ord_null, const int64 *arg, const uint8 *arg_null);

extern uint64 sift_par_win_spill_rows(const SiftParWinSpill *s);

/* Sorts the rows, runs the functions over them with at most max_bytes held by the frames, gives each row to emit, and ends the sort */
extern void sift_par_win_spill_run(SiftParWinSpill *s, uint64 max_bytes, SiftWinEmit emit, void *emit_arg);

#endif							/* SIFT_AGG_V6_PAR_WINSPILL_H */
