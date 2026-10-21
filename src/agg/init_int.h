/*
 * agg/init_int.h - the setup of the executor state (agg/init.c)
 */
#ifndef SIFT_AGG_INIT_INT_H
#define SIFT_AGG_INIT_INT_H

#include "agg/agg_fwd_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* init.c */
void extend_layout(AggExec *ex);
void init_states(AggExec *ex);
void register_preds(AggExec *ex);
void prune_init(AggExec *ex);
void str_init(AggExec *ex);
void filter_init(AggExec *ex);
void recheck_init(AggExec *ex);
void type_init(AggExec *ex);
void wide_init(AggExec *ex);
void tid_init(AggExec *ex, Snapshot snapshot);
void register_key(AggExec *ex, int ki);
void register_combos(AggExec *ex);
void join_filters(AggExec *ex, MemoryContext caller);
bool join_none(AggExec *ex);
void table_init(AggExec *ex);
void table_reset(AggExec *ex);
void scan_buffers(AggExec *ex);

#pragma GCC visibility pop

#endif
