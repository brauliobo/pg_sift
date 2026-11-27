/*
 * agg/qual/step.h - the calendar step of an expression of a timestamp column (agg/qual/step.c): date_trunc, extract, date_part and the date of a timestamp change their value only where the
 * column crosses a second, minute, hour or day of its clock (the session's for a timestamptz), so an expression that reads the column only through them keeps its value over such a step.
 */
#ifndef SIFT_AGG_V6_STEP_H
#define SIFT_AGG_V6_STEP_H

#include "postgres.h"

#include "nodes/primnodes.h"

/*
 * The microseconds e keeps its value for: every Var of relid it reads is one timestamp column (*col) read only as the argument of a calendar call, and the step is the finest of those calls
 * (a call over another one the coarser of the two). 0 for any other expression. A timestamptz is cut on the clock of the session's zone, over a stretch of one offset.
 */
extern int64 sift_v6_calendar_step(const Node *e, Index relid, const Var **col);

/* EXPLAIN: " (per day)", " (per hour)" .. for a step */
extern const char *sift_v6_step_text(int64 us);

#endif							/* SIFT_AGG_V6_STEP_H */
