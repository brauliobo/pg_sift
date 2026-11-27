/*
 * agg/qual/step.h - the calendar step of an expression of a timestamp column (agg/qual/step.c): date_trunc, extract, date_part, the date of a timestamp and date_bin change their value only
 * where the column crosses a second, minute, hour, day or bin of its clock (the session's for a calendar call of a timestamptz, the instants themselves for date_bin), so an expression that reads
 * the column only through them keeps its value over such a step, and is evaluated once for each run of rows in one step.
 */
#ifndef SIFT_AGG_V6_STEP_H
#define SIFT_AGG_V6_STEP_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/vec.h"
#include "core/zoneoff.h"

/* The step of an expression: its microseconds, and the clock the steps are counted on (zone: the session's, opened in the caller's context; NULL: the stored values) */
typedef struct SiftV6Step
{
	int64		us;
	ZoneOff    *zone;
} SiftV6Step;

/*
 * The microseconds e keeps its value for: every Var of relid it reads is one timestamp column (*col) read only as the argument of a calendar call, and the step is the finest of those calls
 * (a calendar call over another one the coarser of the two, a date_bin over one or one over a date_bin the inner step). *local: the steps are on the clock of the session's zone (a calendar call of
 * a timestamptz). 0 for any other expression, or one that mixes the two clocks.
 */
extern int64 sift_v6_calendar_step(const Node *e, Index relid, const Var **col, bool *local);

/* The step of e as sift_v6_calendar_step finds it, the zone opened in cx when it is on the session's clock; false when e has none */
extern bool sift_v6_step_open(MemoryContext cx, const Node *e, Index relid, SiftV6Step *out, const Var **col);

/*
 * The runs of the active rows of bs over the timestamps v: heads the first row of each (the rows of bs in heads->mask), head_of[r] the head of row r's run. A run is the rows in a row of one
 * step and, on the session's clock, of one offset; a NULL or an infinite timestamp is a run of its own.
 */
extern void sift_v6_step_runs(const SiftV6Step *st, const SiftVec *v, const SiftBatchSel *bs, SiftBatchSel *heads, uint16 *head_of);

/* EXPLAIN: " (per day)", " (per hour)" .. " (per 900 seconds)" for a step */
extern const char *sift_v6_step_text(int64 us);

#endif							/* SIFT_AGG_V6_STEP_H */
