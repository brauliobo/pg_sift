/*
 * bm25/hitspill.h - the hits of a ranked search that has no limit, sorted outside memory: a search that would hold more hits than work_mem keeps the rest in a sort
 * that spills to temporary files (PostgreSQL's tuplesort) and reads them back best first, by descending score and ascending key. A host that has no sorter begins none (NULL):
 * the search then fails past work_mem as it always did.
 */
#ifndef SIFT_HITSPILL_H
#define SIFT_HITSPILL_H

#include "core/host.h"

#include "bm25/kernels/kernels.h"

typedef struct SiftHitSpill SiftHitSpill;

/* The sort, holding at most work_mem_kb in memory, made in mcxt; NULL where the host has none */
extern SiftHitSpill *sift_hitspill_begin(MemoryContext mcxt, int work_mem_kb);
extern void sift_hitspill_put(SiftHitSpill *s, SiftKey key, float4 score);
/* After the last put and before the first next */
extern void sift_hitspill_sort(SiftHitSpill *s);
/* The next hit, best first; false at the end */
extern bool sift_hitspill_next(SiftHitSpill *s, SiftKey *key, float4 *score);
extern void sift_hitspill_end(SiftHitSpill *s);

#endif
