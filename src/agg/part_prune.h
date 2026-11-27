/*
 * The partitions of a generic plan that the values of its parameters rule out, by PostgreSQL's pruning steps (partitioning/partprune.c), as an Append prunes its subplans
 */
#ifndef SIFT_AGG_PART_PRUNE_H
#define SIFT_AGG_PART_PRUNE_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

#include "agg/engine.h"

extern void sift_v6_part_prune_plan(PlannerInfo *root, RelOptInfo *parent, List *leaves, SiftV6Spec *spec);
extern void sift_v6_part_prune_exec(SiftV6Spec *spec, PlanState *ps, int eflags);

#endif
