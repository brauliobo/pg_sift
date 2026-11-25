/*
 * col/f2/v6/proj/distinct.h - count(DISTINCT col) from the cells of a SIFT_PROJ_AGG projection, exactly and under deletes. The projection keeps col as its last group column, so each stored row is
 * one (group, value) pair with the rows it has: the multiplicity. The statement's distinct count of a group is the number of values whose multiplicity, summed over the stored rows, the rows taken
 * away and the rows added, is above 0. Counting the pairs and not the values is what lets two parts that both hold a value count it once, and a delete of one of two equal rows leave it counted.
 *
 * The multiplicities are a cube of their own, keyed by the statement's groups and the value, with no measure: the rows of a cube are the multiplicity (f2_proj.h). It is bounded: when it holds
 * more pairs than the caller allows, the statement is read from the base (it spills there, as count(DISTINCT) of a scan does).
 */
#ifndef SIFT_COL_F2_V6_PROJ_DISTINCT_H
#define SIFT_COL_F2_V6_PROJ_DISTINCT_H

#include "core/host.h"

#include "col/f2/proj/proj.h"

typedef struct SiftProjDistinct SiftProjDistinct;

/*
 * query: the statement's groups; src: the projection; map: how the statement's group columns come from the stored row (as for the cube of the statement); leaf: the column counted;
 * measure: the statement's measure it answers (its FILTER, tested on the group columns of the stored rows and on the columns of the rows); max_pairs: the bound.
 */
extern SiftProjDistinct *sift_proj_distinct_new(const SiftProjSpec *query, const SiftProjSpec *src, const SiftProjColumns *cols, const SiftProjRollup *map, int measure, int64 max_pairs);
extern void sift_proj_distinct_free(SiftProjDistinct *d);

/* A stored row of the projection (layout lay): its multiplicity goes to its (group, value); filter_ok: the FILTER of the measure holds on the row's group columns */
extern void sift_proj_distinct_stored(SiftProjDistinct *d, const SiftProjLayoutCol *lay, int nlay, const SiftCell *stored, bool filter_ok);

/* A row of the relation, added (sign 1) or taken away (sign -1) */
extern void sift_proj_distinct_row(SiftProjDistinct *d, const SiftCell *row, int sign);

/* false: the pairs outgrew the bound */
extern bool sift_proj_distinct_ok(const SiftProjDistinct *d);

/* Adds the distinct count of every group to the state of its measure in cube (its cnt) */
extern void sift_proj_distinct_finish(SiftProjDistinct *d, SiftProjCube *cube);

#endif							/* SIFT_COL_F2_V6_PROJ_DISTINCT_H */
