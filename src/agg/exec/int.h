/*
 * agg/exec/int.h - what the files of agg/exec/ share
 * The chunk of a top-N result and the functions the parts call in one another.
 */
#ifndef SIFT_AGG_EXEC_DIR_INT_H
#define SIFT_AGG_EXEC_DIR_INT_H

#include "agg/agg_int.h"

#ifdef SIFT_TESTING
extern int	sift_test_agg_pass_w;
#endif
extern const CustomExecMethods agg_exec_methods;
extern void agg_shutdown(CustomScanState *node);
extern void agg_explain(CustomScanState *node, List *ancestors, ExplainState *es);
extern void run_counters(AggExec *c, SiftSrcCounters *cc);
/* the group table holds no more than its memory: asserted where it is at its largest, at the end of a scan and of each of its passes */
extern void agg_mem_checkpoint(const AggExec *ex);

extern TupleTableSlot * topn_next(AggExec *ex, TupleTableSlot *slot);

#endif							/* SIFT_AGG_EXEC_DIR_INT_H */
