/*
 * agg/par/pg.h - the parallel SiftAggV6 as a node of the executor: what the node keeps (SiftV6ParNode), the five callbacks of a parallel custom scan it implements (v6_par_node.c), the engine
 * over the groups (v6_par_group.c) and the paths the planner adds (v6_par_path.c).
 */
#ifndef SIFT_AGG_V6_PAR_PG_H
#define SIFT_AGG_V6_PAR_PG_H

#include "postgres.h"

#include "access/parallel.h"
#include "nodes/pathnodes.h"

#include "agg/engine.h"
#include "agg/par/node.h"
#include "agg/par/par.h"
#include "agg/par/topk.h"

struct SiftV6ParNode
{
	const SiftV6Spec *spec;
	TupleDesc	desc;
	const SiftV6ScanSrc *sources;	/* the relations of the scan: one, or a partition each */
	int			nsources;
	Snapshot	snap;
	MemoryContext mcx;			/* of the query */
	MemoryContext tuple_cx;		/* of the tuple the node returns */

	SiftV6ParDsmHdr *hdr;		/* NULL: the node runs in one process with no shared memory, as a serial node */
	SiftV6ParShape shape;
	SiftF2ParPlan plan;			/* the units of every source, each tagged with its source (child_idx); staged_dead is the deletes of every source one after the other */
	uint64	   *staged_start;	/* [nsources + 1]: where the deletes of each source start in staged_dead */
	SiftV6ParEngine eng;
	SiftV6ParRun *run;
	SiftV6ParTopK *topk;		/* ORDER BY .. LIMIT of the groups: the participant gives its best K only */

	/* what the participants did, read from the shared block when the node shuts down */
	SiftV6ParWorkerStats total;
	uint64		ring_recs;
	uint64		ring_bytes;
	uint32		radix_parts;
	bool		escalated;
	bool		have_total;
};

extern void sift_v6_par_engine_make(SiftV6ParEngine *eng, const SiftV6Spec *spec, MemoryContext cx, MemoryContext tuple_cx, TupleDesc desc, SiftV6ParDsmHdr *hdr, const SiftV6ScanSrc *sources,
									Snapshot snap);
extern void sift_v6_par_engine_end(SiftV6ParEngine *eng);

/*
 * Adds the Gather of a parallel node next to the serial path of add_group_path, when sift_v6_par_choose_strategy picks a parallel strategy; the spec of the path carries it. Returns the rule that
 * keeps the scan in one process when none is added (NULL: one is, or the session allows no workers); children: the serial path reads the plans of its joins; as_written: the node returns the
 * expressions of the target as the statement writes them
 */
extern const char *sift_v6_par_add_path(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *output_rel, const SiftV6Spec *serial_spec, const CustomPathMethods *methods, List *scan_tlist,
										List *having, double groups, Cost serial_total, bool children, bool as_written);
/* The rule that keeps a Top-N of the rows of rel in one process (NULL: the session allows no workers); joins: it reads the plans of lookup tables */
extern const char *sift_v6_par_rows_serial_why(PlannerInfo *root, const RelOptInfo *rel, bool joins);
/* Prices the serial path by the compute threads of the backend that run its scan, when the spec is one they take (agg/par/run.h, agg/par/pinrun.h) */
extern void sift_v6_threads_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6Spec *spec, double groups, Path *path);

#endif							/* SIFT_AGG_V6_PAR_PG_H */
