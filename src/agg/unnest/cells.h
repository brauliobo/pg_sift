/*
 * agg/unnest/cells.h - the statements of a LATERAL unnest the cells of a cube of the elements answer, whatever the format that keeps the cube: a cell stands for the elements of a granule
 * that share the values of its dimensions, so it answers what the statement computes of those values alone. The keys, the derivations of them (left, CASE, coalesce, an arithmetic), the
 * clauses of the FILTER aggregates and the quals of the element read dimensions only; an aggregate counts the elements, counts a dimension, or reads a measure of the cube. The cells then go
 * through the derivations, the quals and the groups as the pairs of (row, element) do (sift_v6_pipe_cells).
 */
#ifndef SIFT_AGG_UNNEST_CELLS_H
#define SIFT_AGG_UNNEST_CELLS_H

#include "postgres.h"

#include "agg/engine.h"
#include "agg/unnest/test.h"

/* ee: the quals of the element (NULL: none). True when the cells of a cube answer the spec; *dims, bit c: the element column c is one the cube must have as a dimension */
extern bool sift_v6_elem_cells_ok(const SiftV6Spec *spec, const SiftV6ElemEval *ee, uint64 *dims);

#endif							/* SIFT_AGG_UNNEST_CELLS_H */
