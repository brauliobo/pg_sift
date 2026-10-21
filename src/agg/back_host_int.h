/*
 * agg/back_host_int.h - the backend of the executor on PostgreSQL (agg/back_host.c)
 */
#ifndef SIFT_AGG_BACK_HOST_INT_H
#define SIFT_AGG_BACK_HOST_INT_H

#include "agg/agg_fwd_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* back_host.c */
bool backend_begin(AggExec *ex);
void backend_finish(AggExec *ex);

#pragma GCC visibility pop

#endif
