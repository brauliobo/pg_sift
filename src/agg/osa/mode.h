/*
 * agg/osa/mode.h - mode() WITHIN GROUP (ORDER BY x) in the groups of SiftAggV6 as a count of the (group, value) pairs instead of a sort of every value. The values of a type whose
 * equal values have one image (integers, dates, timestamps, bool, float8, text of a deterministic collation) are counted in a hash of their own memory, an equal part of work_mem as
 * a sort of the aggregate holds; when it is full, every pair goes to the sort of the aggregate (v6_gen_sort.h) as many times as it was counted, and the sort runs the aggregate from then on.
 * The answer of a group is the value counted most, the first in the order of the call among the values counted as often (nodeAgg's mode_final).
 */
#ifndef SIFT_AGG_V6_OSA_MODE_H
#define SIFT_AGG_V6_OSA_MODE_H

#include "agg/gen/gen.h"

typedef struct V6Mode V6Mode;

/* the count of a mode() whose values the hash can count, NULL for any other aggregate */
extern V6Mode *v6_mode_new(const V6GenAgg *ga, MemoryContext cx, int mem_kb);
/* the value of an input of group sid: counted, or false when the hash was full and moved to the sort (ga->mode is NULL then, and the sort takes this input as well) */
extern bool v6_mode_take(V6GenAgg *ga, uint32 sid, Datum value);
/* the scan has ended: each group of the count has its answer in the results' memory; accs[sid] is the state of group sid */
extern void v6_mode_finish(V6Gen *gen, V6GenAgg *ga, V6Acc **accs, uint32 nsid);
extern void v6_mode_free(V6Mode *m);
/* every (group, value) pair counted, with its count */
extern void v6_mode_each(const V6Mode *m, V6PairFn fn, void *arg);

#endif							/* SIFT_AGG_V6_OSA_MODE_H */
