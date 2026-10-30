/*
 * agg/spec.h - the plan of a SiftAgg: what the planner proved about a query and the executor runs.
 *
 * The planner (agg/plan/) resolves every column, jsonb path and array element the query uses to a leaf of the column
 * source (agg/colsrc.h) and builds a SiftAggSpec; the spec travels in custom_private as a flat List (sift_aggspec_pack /
 * sift_aggspec_unpack: Integer, String and Node items in a fixed order) so the plan can be copied, cached and sent to
 * parallel workers like any other plan. Nothing in a spec points to the planner's data.
 *
 * Levels: a query without a set-returning function runs at row level (one unit row = one tuple). With one (jsonb_to_recordset,
 * jsonb_array_elements, jsonb_each over an array or object leaf) it runs at element level: one tuple per element of the group
 * leaf `group`, the row level columns of its parent row being available to every tuple.
 */
#ifndef SIFT_AGGSPEC_H
#define SIFT_AGGSPEC_H

#include "agg/spec/expr.h"
#include "agg/spec/join.h"
#include "agg/spec/spec.h"

#endif							/* SIFT_AGGSPEC_H */
