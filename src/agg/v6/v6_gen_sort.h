/*
 * agg/v6/v6_gen_sort.h - the inputs of the DISTINCT, ORDER BY and WITHIN GROUP aggregates of SiftAggV6 (v6_gen.h): one tuplesort for each aggregate of (the number of the group's sort,
 * the arguments that vary), ordered by the number and the sort keys of the call as nodeAgg orders the input of such an aggregate (the sort operator, the input collation and the
 * NULLS FIRST/LAST of each key; DISTINCT sorts on its own list, which starts with the ORDER BY keys). The memory is work_mem; a larger input goes to disk. DISTINCT drops an input
 * equal to the one before it in the same group.
 */
#ifndef SIFT_AGG_V6_GEN_SORT_H
#define SIFT_AGG_V6_GEN_SORT_H

#include "agg/v6/v6_gen.h"

typedef struct V6GenSort V6GenSort;

extern V6GenSort *v6_gen_sort_begin(V6GenAgg *ga, MemoryContext cx, int mem_kb);
/* an input of group sid: its arguments (the constants of the call are not kept) */
extern void v6_gen_sort_put(V6GenSort *s, uint32 sid, const Datum *args, const bool *nulls);
/* every group of the sort in turn goes through the transition function, then the final function; accs[sid] is the state of group sid */
extern void v6_gen_sort_drain(V6Gen *gen, V6GenAgg *ga, V6Acc **accs, uint32 nsid);
extern void v6_gen_sort_end(V6GenSort *s);

#endif							/* SIFT_AGG_V6_GEN_SORT_H */
