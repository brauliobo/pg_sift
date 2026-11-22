/*
 * agg/plan/format_ops.h - the aggregate engine of each format of a sift_col index. The format of an index is the one its Block 0 says (core/meta_peek.h), and the engine
 * of that format alone plans and runs the aggregate over it: Format 1 the plans of agg/plan/paths.c, Format 2.0 the node of agg/v6/. A table that has indexes of both gets
 * the paths of each engine, which plans only the indexes of its own format, and the planner keeps the cheapest.
 */
#ifndef SIFT_AGG_PLAN_FORMAT_OPS_H
#define SIFT_AGG_PLAN_FORMAT_OPS_H

#include "postgres.h"

#include "nodes/pathnodes.h"

typedef struct SiftAggFormatOps
{
	uint16		format_major;	/* the format major of the Block 0 of the indexes this engine plans for */
	const char *name;
	void		(*upper_paths) (PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
} SiftAggFormatOps;

/* The engine of a format major, NULL for one no engine reads */
extern const SiftAggFormatOps *sift_agg_format_ops(uint16 format_major);

/* Is the index of an oid a sift_col index of the engine's format? An index without a block 0 of its own yet (being built) is the new format's */
extern bool sift_agg_format_ops_owns(const SiftAggFormatOps *ops, Oid indexoid);

#endif
