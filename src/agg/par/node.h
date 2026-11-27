/*
 * agg/par/node.h - the parallel SiftAggV6 as exec.c sees it: an opaque node state and the callbacks of a parallel custom scan it forwards to. exec.c cannot include v6_par.h (the headers of the
 * executor and those of the hash tables declare the same comparison operators), so what it needs of the run is here, as plain values.
 */
#ifndef SIFT_AGG_V6_PAR_NODE_H
#define SIFT_AGG_V6_PAR_NODE_H

#include "postgres.h"

#include "access/parallel.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "agg/engine.h"
#include "agg/scan_src.h"

typedef struct SiftV6ParNode SiftV6ParNode;

/* What EXPLAIN prints of a parallel node: the strategy the planner chose, and once the node has shut down what its participants did */
typedef struct SiftV6ParSummary
{
	const char *strategy;
	bool		ran;
	bool		escalated;
	uint64		morsels_stolen;
	uint64		radix_parts;
	uint64		partial_recs;
	uint64		partial_bytes;
	uint64		grans_scanned;
	uint64		grans_preagg;
	uint64		grans_preagg_delta;
	uint64		heap_vet_rows;
	uint64		topk_dropped;
	uint64		workers;
} SiftV6ParSummary;

extern SiftV6ParNode *sift_v6_par_node_begin(const SiftV6Spec *spec, TupleDesc desc, const SiftV6ScanSrc *sources, int nsources, Snapshot snap, MemoryContext mcx,
											 MemoryContext tuple_cx);
/* false: there is no shared block (a Gather with no workers makes none) and the node runs as a serial one */
extern bool sift_v6_par_node_attached(const SiftV6ParNode *pn);
extern Size sift_v6_par_node_estimate(SiftV6ParNode *pn, ParallelContext *pcxt);
extern void sift_v6_par_node_initialize(SiftV6ParNode *pn, ParallelContext *pcxt, void *chunk);
extern void sift_v6_par_node_reinitialize(SiftV6ParNode *pn, void *chunk);
extern void sift_v6_par_node_worker(SiftV6ParNode *pn, void *chunk);
extern void sift_v6_par_node_shutdown(SiftV6ParNode *pn);
extern bool sift_v6_par_node_next(SiftV6ParNode *pn, Datum *values, bool *isnull);
extern void sift_v6_par_node_end(SiftV6ParNode *pn);
extern void sift_v6_par_node_summary(const SiftV6ParNode *pn, SiftV6ParSummary *out);
extern const char *sift_v6_par_strategy_name(int strategy);

#endif							/* SIFT_AGG_V6_PAR_NODE_H */
