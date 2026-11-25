/*
 * agg/v6/v6_cube.h - the statements a pre-aggregated cube can answer, whatever the format that keeps the cubes: plain keys of an integer, boolean or text column, and aggregates of a plain column.
 */
#ifndef SIFT_AGG_V6_CUBE_H
#define SIFT_AGG_V6_CUBE_H

#include "agg/v6/v6.h"

/* No set, bin, derived value, FILTER, function, extension, text query, document, PostgreSQL or generated expression, constant group, lock or row output; keys that are columns of an integer, boolean or text class
 * (not numeric), aggregates of a column that count, sum, average, take the least or the greatest of an integer, a float or a text, or count the distinct values of one */
extern bool sift_v6_spec_cubeable(const SiftV6Spec *spec);

#endif							/* SIFT_AGG_V6_CUBE_H */
