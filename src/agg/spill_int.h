/*
 * agg/spill_int.h - the group table past hash memory (agg/spill.c)
 */
#ifndef SIFT_AGG_SPILL_INT_H
#define SIFT_AGG_SPILL_INT_H

#include "agg/agg_fwd_int.h"

struct AggSpill;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* spill.c */
void spill_begin(AggExec *ex);
void spill_floor(AggExec *ex);
void spill_check(AggExec *ex);
void spill_finish(AggExec *ex);
bool spill_more(AggExec *ex);
TupleTableSlot *spill_output(AggExec *ex, TupleTableSlot *slot, bool (*next) (AggExec *, TupleTableSlot *));
bool spill_rescan(AggExec *ex);
void spill_end(AggExec *ex);
bool spill_presize_ok(const AggExec *ex);
bool spill_started(const AggExec *ex);
bool spill_would(AggExec *ex, struct AggSpill *sp);

#pragma GCC visibility pop

#endif
