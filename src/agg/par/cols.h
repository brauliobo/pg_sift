/*
 * agg/par/cols.h - a decoded column of a granule as the operators of this directory read it in pure C: the values of the rows, and which of them are NULL.
 */
#ifndef SIFT_AGG_V6_PAR_COLS_H
#define SIFT_AGG_V6_PAR_COLS_H

#include "postgres.h"

typedef struct SiftParCol
{
	const int64 *v;
	const bool *isnull;
} SiftParCol;

#endif							/* SIFT_AGG_V6_PAR_COLS_H */
