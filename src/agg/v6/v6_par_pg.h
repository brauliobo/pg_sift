/*
 * agg/v6/v6_par_pg.h - the parallel SiftAggV6 as a node of the executor: what the node keeps (SiftV6ParNode), the five callbacks of a parallel custom scan it implements (v6_par_node.c), the engine
 * over the groups (v6_par_group.c) and the paths the planner adds (v6_par_path.c).
 */
#ifndef SIFT_AGG_V6_PAR_PG_H
#define SIFT_AGG_V6_PAR_PG_H

#include "postgres.h"

#include "access/parallel.h"
#include "nodes/pathnodes.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_par_node.h"
#include "agg/v6/v6_par.h"
#include "col/f2/f2_sink.h"

struct SiftV6ParNode
{
	const SiftV6Spec *spec;
	TupleDesc	desc;
	Relation	heap;
	Relation	index;
	ScanKey		keys;
	int			nkeys;
	Snapshot	snap;
	MemoryContext mcx;			/* of the query */
	MemoryContext tuple_cx;		/* of the tuple the node returns */

	SiftV6ParDsmHdr *hdr;		/* NULL: the node runs in one process with no shared memory, as a serial node */
	SiftV6ParShape shape;
	SiftF2ParPlan plan;
	SiftV6ParEngine eng;
	SiftV6ParRun *run;

	/* what the participants did, read from the shared block when the node shuts down */
	SiftV6ParWorkerStats total;
	uint64		ring_recs;
	uint64		ring_bytes;
	uint32		radix_parts;
	bool		escalated;
	bool		have_total;
};

extern void sift_v6_par_engine_make(SiftV6ParEngine *eng, const SiftV6Spec *spec, MemoryContext cx, MemoryContext tuple_cx, TupleDesc desc, SiftV6ParDsmHdr *hdr, Relation heap, Relation index,
									ScanKey keys, int nkeys, Snapshot snap);
extern void sift_v6_par_engine_end(SiftV6ParEngine *eng);

/* Adds the Gather of a parallel node next to the serial path of add_group_path, when sift_v6_par_choose_strategy picks a parallel strategy; the spec of the path carries it */
extern void sift_v6_par_add_path(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *output_rel, const SiftV6Spec *serial_spec, const CustomPathMethods *methods, List *scan_tlist, List *having,
								 double groups);

#endif							/* SIFT_AGG_V6_PAR_PG_H */
