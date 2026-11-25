/*
 * agg/win/x_glue.h - SiftWinx (v6_datebin_win.h) as the executor of the window node of SiftAggV6 (v6_window.c) when a function of the window has what the executor of agg/window.h has not: a
 * FILTER (WHERE ..) clause, or a numeric argument (count, sum, avg, min and max over the scaled integers of agg/vec.h, the average exact, with no float8 in it).
 *
 * The window is the one frame SiftWinx has, from the start of the partition to the current row (ROWS, or RANGE with the peers of the row in it), over input in the order of the partition keys and the
 * window order, which the node already gets from the sorted parts. The planner and the executor build the spec with the same function, as for the executor of agg/window.h: sift_v6_winx_spec says
 * whether SiftWinx runs the functions and adds the columns they read to the input. The FILTER clauses are compiled by sift_v6_winx_filters (the planner probes them with no parameters, the
 * executor compiles them with the ones of its execution), and each batch's bitmaps are what sift_v6_vexpr_filter makes of them.
 */
#ifndef SIFT_AGG_V6_WINX_GLUE_H
#define SIFT_AGG_V6_WINX_GLUE_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/params.h"

#include "agg/win/datebin.h"
#include "agg/vexpr.h"

typedef struct SiftV6Winx
{
	SiftWinxSpec spec;
	int			arg_slot[SIFT_WINX_MAX_FUNCS];	/* the slot of the input of each function's argument (-1: none) */
	Oid			arg_typid[SIFT_WINX_MAX_FUNCS];
	Oid			res_typid[SIFT_WINX_MAX_FUNCS];
	int			nfilters;
	int			filter_func[SIFT_WINX_MAX_FILTERS];	/* the function whose FILTER each clause is */
} SiftV6Winx;

/* Does a function of the window (a List of WindowFunc) carry a FILTER, or take a numeric: the executor of agg/window.h takes neither */
extern bool sift_v6_winx_wanted(const List *funcs);
/* The spec of SiftWinx for the functions; false when it does not have one of them. range: RANGE, not ROWS */
extern bool sift_v6_winx_spec(const List *funcs, bool range, SiftV6Input *in, SiftV6Winx *out);
/* The programs of the FILTER clauses, out[k] for the k-th clause of the spec; their columns are added to in. false: the machine does not run one */
extern bool sift_v6_winx_filters(MemoryContext cx, const List *funcs, Index rti, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out);
extern Datum sift_v6_winx_datum(const SiftV6Winx *w, int f, const SiftWinxVal *v);

/* Runs SiftWinx over the source: every row it emits goes to emit */
extern void sift_v6_winx_run(MemoryContext cx, const SiftV6Winx *w, const int *part_slot, const int *ord_slot, SiftV6Source *src, const SiftV6Vexpr *filters,
							 SiftWinxEmit emit, void *emit_arg, SiftWinxStats *stats);

#endif							/* SIFT_AGG_V6_WINX_GLUE_H */
