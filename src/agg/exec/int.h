/*
 * agg/exec/int.h - what the files of agg/exec/ share
 * The chunk of a top-N result and the functions the parts call in one another.
 */
#ifndef SIFT_AGG_EXEC_DIR_INT_H
#define SIFT_AGG_EXEC_DIR_INT_H

#include "agg/agg_int.h"

extern bool sift_test_agg_wait;
extern const CustomExecMethods agg_exec_methods;
extern void agg_explain(CustomScanState *node, List *ancestors, ExplainState *es);
extern void run_counters(AggExec *c, SiftSrcCounters *cc);

extern void agg_test_wait(void);
extern void finish_groups(AggExec *ex);
extern TupleTableSlot * topn_next(AggExec *ex, TupleTableSlot *slot);

#endif							/* SIFT_AGG_EXEC_DIR_INT_H */
