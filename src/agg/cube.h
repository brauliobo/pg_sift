/*
 * agg/cube.h - the statements a pre-aggregated cube can answer, whatever the format that keeps the cubes: plain keys of an integer, boolean or text column, and aggregates of a plain column.
 */
#ifndef SIFT_AGG_V6_CUBE_H
#define SIFT_AGG_V6_CUBE_H

#include "agg/engine.h"

/* An argument of a sum or an average that is mul * x + add of an integer column x, computed as PostgreSQL does in type (x + k, x - k, k - x, x * k, k * x of int2, int4 or int8 operators) */
typedef struct SiftV6Linear
{
	int64		mul;
	int64		add;
	Oid			type;
} SiftV6Linear;

/* The argument of a is a line of its column (a->arg.attno): a cube answers it as mul * sum(x) + add * count(x) where it shows the line keeps every x in its type */
extern bool sift_v6_agg_linear(const SiftV6Spec *spec, const SiftV6Agg *a, SiftV6Linear *lin);
/* mul * x + add stays in the type for every x of [lo, hi]: PostgreSQL's operator raises no error on a row of them */
extern bool sift_v6_linear_fits(const SiftV6Linear *lin, int64 lo, int64 hi);
/* mul * sum + add * n, the sum of the line over n values whose sum is sum: false when it leaves 128 bits */
extern bool sift_v6_linear_sum(const SiftV6Linear *lin, SiftI128 sum, int64 n, SiftI128 *out);

/* No set, bin, derived value (but a line of a column a sum or an average reads), FILTER, function, extension, text query, document, generated expression, constant group, lock or row output; keys that are columns of an integer, boolean or text class
 * (not numeric), aggregates of a column that count, sum, average, take the least or the greatest of an integer, a float or a text, or count the distinct values of one */
extern bool sift_v6_spec_cubeable(const SiftV6Spec *spec);

#endif							/* SIFT_AGG_V6_CUBE_H */
