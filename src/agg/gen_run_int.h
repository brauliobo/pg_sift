/*
 * agg/gen_run_int.h - the generic aggregates: the update, flush and finish per chunk (agg/gen_run.c)
 */
#ifndef SIFT_AGG_GEN_RUN_INT_H
#define SIFT_AGG_GEN_RUN_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/accum_int.h"
#include "agg/gen_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* gen_run.c */
void gen_first_put(GenSt *st, AggAcc *acc, uint32 g, StringInfo vb);
void gen_first_merge(GenSt *st, AggAcc *acc, uint32 g, const char *at);
void gen_update(AggExec *ex, int ai, const uint64 *ok, int nsel);
void gen_flush(AggExec *ex);
void gen_finish(AggExec *ex);

#pragma GCC visibility pop

#endif
